#include "camera/flicker_detector.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>

namespace ixc::camera {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kStandingFrames = 15;   // fixed-exposure frames averaged for the standing-band check
constexpr double kMinShare = 0.45;    // a clear sinusoid dominates the profile
constexpr int kWindow = 32, kNeededHits = 14;
}  // namespace

int FlickerDetector::RowMeans(const std::uint8_t* y, int stride, int width, int height, float* out) {
    if (!y || width < 16 || height < 16) return 0;
    const int n = std::min(height, kMaxRows);
    for (int i = 0; i < n; ++i) {
        const int r = static_cast<int>((static_cast<long long>(i) * 2 + 1) * height / (2 * n));
        const std::uint8_t* row = y + static_cast<std::ptrdiff_t>(r) * stride;
        unsigned sum = 0, cnt = 0;
        for (int x = 4; x < width; x += 16) {
            sum += row[x];
            ++cnt;
        }
        out[i] = cnt ? static_cast<float>(sum) / static_cast<float>(cnt) : 0.0f;
    }
    return n;
}

double FlickerDetector::PeriodicShare(const float* d, int n, double& amplitude) {
    amplitude = 0;
    if (n < 32) return 0;
    // Remove the best-fit line (overall brightness change, top-to-bottom gradients).
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; ++i) {
        sx += i;
        sy += d[i];
        sxx += static_cast<double>(i) * i;
        sxy += static_cast<double>(i) * d[i];
    }
    const double den = n * sxx - sx * sx;
    const double slope = den != 0 ? (n * sxy - sx * sy) / den : 0;
    const double icpt = (sy - slope * sx) / n;
    double var = 0;
    for (int i = 0; i < n; ++i) {
        const double r = d[i] - (icpt + slope * i);
        var += r * r;
    }
    var /= n;
    if (var < 1e-6) return 0;
    double best = 0, bestW = 0, bestRe = 0, bestIm = 0;
    for (double c = 1.5; c <= 8.0 + 1e-9; c += 0.25) {
        // Rotating phasor instead of a sin/cos per sample.
        const double w = 2 * kPi * c / n;
        const double cw = std::cos(w), sw = std::sin(w);
        double re = 0, im = 0, pc = 1, ps = 0;
        for (int i = 0; i < n; ++i) {
            const double r = d[i] - (icpt + slope * i);
            re += r * pc;
            im -= r * ps;
            const double npc = pc * cw - ps * sw;
            ps = ps * cw + pc * sw;
            pc = npc;
        }
        const double p = 2 * (re * re + im * im) / (static_cast<double>(n) * n);  // = A^2/2 for a sinusoid of amplitude A
        if (p > best) {
            best = p;
            bestW = w;
            bestRe = re;
            bestIm = im;
        }
    }
    amplitude = std::sqrt(2 * best);
    // Bands cover the whole picture; a moving person or a lamp switching on changes one part. The
    // fitted sinusoid must explain the signal in every quarter of the frame.
    const double a = 2 * bestRe / n, b = -2 * bestIm / n;
    for (int q = 0; q < 4; ++q) {
        double sig = 0, err = 0;
        for (int i = q * n / 4; i < (q + 1) * n / 4; ++i) {
            const double r = d[i] - (icpt + slope * i);
            const double fit = a * std::cos(bestW * i) + b * std::sin(bestW * i);
            sig += r * r;
            err += (r - fit) * (r - fit);
        }
        if (sig <= 0 || err > 0.75 * sig) return 0;  // explains < 25% of this quarter
    }
    return std::min(1.0, best / var);
}

void FlickerDetector::Reset() {
    n_ = refCount_ = fixedCount_ = 0;
    havePrev_ = standingChecked_ = banding_ = false;
    hits_ = 0;
    seen_ = 0;
    ref_.fill(0);
    fixed_.fill(0);
}

void FlickerDetector::AddReference(const float* rows, int n) {
    if (n <= 0 || n > kMaxRows) return;
    if (n != n_) {
        Reset();
        n_ = n;
    }
    // Sum of up to 30 frames, then a running average of the most recent ~30 (the frames right
    // before the switch are the ones that matter).
    constexpr int kRefFrames = 30;
    if (refCount_ < kRefFrames) {
        for (int i = 0; i < n; ++i) ref_[static_cast<size_t>(i)] += rows[i];
        ++refCount_;
    } else {
        for (int i = 0; i < n; ++i) ref_[static_cast<size_t>(i)] += rows[i] - ref_[static_cast<size_t>(i)] / static_cast<float>(kRefFrames);
    }
}

bool FlickerDetector::AddFixed(const float* rows, int n) {
    if (banding_) return true;
    if (n <= 0 || n > kMaxRows) return false;
    if (n != n_) {  // a new stream size: references from another size don't apply
        Reset();
        n_ = n;
    }
    double mean = 0;
    for (int i = 0; i < n; ++i) mean += rows[i];
    mean /= n;

    // Moving bands: sinusoid in the frame-to-frame difference of the row profile.
    bool hit = false;
    if (havePrev_) {
        for (int i = 0; i < n; ++i) work_[static_cast<size_t>(i)] = rows[i] - prev_[static_cast<size_t>(i)];
        double amp = 0;
        const double share = PeriodicShare(work_.data(), n, amp);
        hit = share >= kMinShare && amp >= std::max(1.0, 0.02 * mean);
        ++seen_;
    }
    std::copy(rows, rows + n, prev_.begin());
    havePrev_ = true;
    hits_ = (hits_ << 1) | (hit ? 1u : 0u);
    if (seen_ >= kWindow / 2 && std::popcount(seen_ >= kWindow ? hits_ : hits_ & ((1u << seen_) - 1)) >= kNeededHits) banding_ = true;

    // Standing bands: fixed-exposure profile relative to the auto-exposure profile.
    if (!standingChecked_ && refCount_ >= 5) {
        for (int i = 0; i < n; ++i) fixed_[static_cast<size_t>(i)] += rows[i];
        if (++fixedCount_ >= kStandingFrames) {
            standingChecked_ = true;
            bool usable = true;
            for (int i = 0; i < n && usable; ++i) {
                const double a = ref_[static_cast<size_t>(i)] / static_cast<double>(refCount_);
                const double f = fixed_[static_cast<size_t>(i)] / static_cast<double>(fixedCount_);
                usable = a >= 8;  // too dark to tell
                work_[static_cast<size_t>(i)] = usable ? static_cast<float>(f / a) : 0.0f;
            }
            if (usable) {
                double amp = 0;
                const double share = PeriodicShare(work_.data(), n, amp);
                if (share >= kMinShare && amp >= 0.04) banding_ = true;  // >= 4% brightness ripple
            }
        }
    }
    return banding_;
}

}  // namespace ixc::camera
