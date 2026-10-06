#include "segmentation/mask_refine.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace ixc::seg {

namespace {
constexpr int kGuideRadius = 4;       // pixels at mask resolution (~1.5% of the width)
}  // namespace

bool MaskRefiner::Init(int netW, int netH, int outW, int outH) {
    if (netW < 2 || netH < 2 || outW < 2 || outH < 2) return false;
    netW_ = netW;
    netH_ = netH;
    w_ = outW;
    h_ = outH;
    const size_t n = static_cast<size_t>(outW) * outH;
    try {
        for (auto* v : {&p_, &i_, &meanI_, &meanP_, &varI_, &covIp_, &tmp_}) v->assign(n, 0.0f);
    } catch (const std::bad_alloc&) {
        for (auto* v : {&p_, &i_, &meanI_, &meanP_, &varI_, &covIp_, &tmp_}) std::vector<float>().swap(*v);
        return false;
    }
    havePrevious_ = false;
    return true;
}

size_t MaskRefiner::MemoryBytes() const {
    return (p_.capacity() + i_.capacity() + meanI_.capacity() + meanP_.capacity() + varI_.capacity() + covIp_.capacity() + tmp_.capacity()) *
           sizeof(float);
}

void MaskRefiner::Upsample(const float* prob, float* out) const {
    // Pixel-centre aligned bilinear (same convention as the network's own resize layers).
    const float sx = static_cast<float>(netW_) / static_cast<float>(w_), sy = static_cast<float>(netH_) / static_cast<float>(h_);
    for (int y = 0; y < h_; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) * sy - 0.5f, 0.0f, static_cast<float>(netH_ - 1) - 0.001f);
        const int y0 = static_cast<int>(fy);
        const float wy = fy - static_cast<float>(y0);
        const float* r0 = prob + static_cast<size_t>(y0) * netW_;
        const float* r1 = r0 + netW_;
        float* o = out + static_cast<size_t>(y) * w_;
        for (int x = 0; x < w_; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) * sx - 0.5f, 0.0f, static_cast<float>(netW_ - 1) - 0.001f);
            const int x0 = static_cast<int>(fx);
            const float wx = fx - static_cast<float>(x0);
            const float top = r0[x0] + (r0[x0 + 1] - r0[x0]) * wx, bottom = r1[x0] + (r1[x0 + 1] - r1[x0]) * wx;
            o[x] = top + (bottom - top) * wy;
        }
    }
}

void MaskRefiner::BoxFilter(const float* in, float* out, int r) {
    // Separable running sums; windows are clipped at the borders and normalized by their size.
    float* t = tmp_.data();
    for (int y = 0; y < h_; ++y) {
        const float* src = in + static_cast<size_t>(y) * w_;
        float* dst = t + static_cast<size_t>(y) * w_;
        float sum = 0;
        int count = 0;
        for (int x = 0; x <= std::min(r, w_ - 1); ++x, ++count) sum += src[x];
        for (int x = 0; x < w_; ++x) {
            dst[x] = sum / static_cast<float>(count);
            const int add = x + r + 1, remove = x - r;
            if (add < w_) { sum += src[add]; ++count; }
            if (remove >= 0) { sum -= src[remove]; --count; }
        }
    }
    for (int x = 0; x < w_; ++x) {
        float sum = 0;
        int count = 0;
        for (int y = 0; y <= std::min(r, h_ - 1); ++y, ++count) sum += t[static_cast<size_t>(y) * w_ + x];
        for (int y = 0; y < h_; ++y) {
            out[static_cast<size_t>(y) * w_ + x] = sum / static_cast<float>(count);
            const int add = y + r + 1, remove = y - r;
            if (add < h_) { sum += t[static_cast<size_t>(add) * w_ + x]; ++count; }
            if (remove >= 0) { sum -= t[static_cast<size_t>(remove) * w_ + x]; --count; }
        }
    }
}

void MaskRefiner::GuidedFilter(const std::uint8_t* guide, float* p) {
    const size_t n = static_cast<size_t>(w_) * h_;
    float* I = i_.data();
    for (size_t k = 0; k < n; ++k) I[k] = static_cast<float>(guide[k]) * (1.0f / 255.0f);
    // varI_ and covIp_ first hold I*I and I*p, then their local (co)variances.
    for (size_t k = 0; k < n; ++k) {
        varI_[k] = I[k] * I[k];
        covIp_[k] = I[k] * p[k];
    }
    BoxFilter(I, meanI_.data(), kGuideRadius);
    BoxFilter(p, meanP_.data(), kGuideRadius);
    BoxFilter(varI_.data(), varI_.data(), kGuideRadius);  // BoxFilter reads `in` row by row before writing: safe in place
    BoxFilter(covIp_.data(), covIp_.data(), kGuideRadius);
    // a = cov / (var + eps), b = meanP - a * meanI; stored in varI_ / covIp_.
    for (size_t k = 0; k < n; ++k) {
        const float var = varI_[k] - meanI_[k] * meanI_[k];
        const float cov = covIp_[k] - meanI_[k] * meanP_[k];
        const float a = cov / (var + eps_);
        varI_[k] = a;
        covIp_[k] = meanP_[k] - a * meanI_[k];
    }
    BoxFilter(varI_.data(), meanI_.data(), kGuideRadius);   // mean a
    BoxFilter(covIp_.data(), meanP_.data(), kGuideRadius);  // mean b
    for (size_t k = 0; k < n; ++k) p[k] = std::clamp(meanI_[k] * I[k] + meanP_[k], 0.0f, 1.0f);
}

void MaskRefiner::SetParams(float temporal, float hair) {
    temporal_ = std::clamp(temporal, 0.0f, 1.0f);
    // Edge sensitivity: smaller eps keeps finer guide detail (hair), larger is calmer on noise.
    eps_ = 0.012f * std::pow(0.08f, std::clamp(hair, 0.0f, 1.0f));  // 0.012 .. 0.001 (0.5 → ~0.0034)
}

std::uint8_t MaskRefiner::Temporal(int previous, int current, float strength) {
    // Agreement → heavier averaging (kills edge flicker); disagreement → follow the new mask
    // (a moving arm doesn't leave a ghost). strength scales the averaging (0.5 = default).
    const int d = std::abs(current - previous);
    const int base = d < 24 ? 5 : d < 64 ? 3 : d < 128 ? 1 : 0;  // eighths of the previous value
    const int keep = std::clamp(static_cast<int>(static_cast<float>(base) * strength * 2.0f + 0.5f), 0, d < 128 ? 7 : 0);
    return static_cast<std::uint8_t>((current * (8 - keep) + previous * keep + 4) / 8);
}

void MaskRefiner::Refine(const float* prob, const std::uint8_t* guide, std::uint8_t* mask) {
    float* p = p_.data();
    Upsample(prob, p);
    GuidedFilter(guide, p);
    const size_t n = static_cast<size_t>(w_) * h_;
    for (size_t k = 0; k < n; ++k) {
        // Soft threshold: smoothstep over 0.3..0.7 of the refined probability.
        const float t = std::clamp((p[k] - 0.3f) * 2.5f, 0.0f, 1.0f);
        const int v = static_cast<int>(t * t * (3 - 2 * t) * 255.0f + 0.5f);
        mask[k] = havePrevious_ ? Temporal(mask[k], v, temporal_) : static_cast<std::uint8_t>(v);
    }
    havePrevious_ = true;
}

}  // namespace ixc::seg
