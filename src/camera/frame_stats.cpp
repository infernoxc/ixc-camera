#include "camera/frame_stats.h"

#include <algorithm>
#include <cmath>

namespace ixc::camera {

FrameStats::FrameStats(std::int64_t expectedIntervalUs) { Reset(expectedIntervalUs); }

void FrameStats::Reset(std::int64_t expectedIntervalUs) {
    expectedUs_ = expectedIntervalUs > 0 ? expectedIntervalUs : 33333;
    // A stall is a gap of more than three frame intervals, and never less than 250 ms,
    // so ordinary jitter at high frame rates isn't counted.
    stallThresholdUs_ = std::max<std::int64_t>(expectedUs_ * 3, 250000);
    count_ = 0;
    head_ = 0;
    haveLast_ = false;
    latencySumMs_ = 0;
    latencyCount_ = 0;
    snap_ = {};
}

void FrameStats::OnFrame(std::int64_t arrivalUs, std::int64_t sampleTimeUs, std::int64_t latencyUs) {
    ++snap_.framesReceived;
    if (haveLast_) {
        if (arrivalUs - lastArrival_ > stallThresholdUs_) ++snap_.stalls;
        const std::int64_t dt = sampleTimeUs - lastSampleTime_;
        if (dt <= 0 || dt > stallThresholdUs_) ++snap_.timestampJumps;
    }
    lastArrival_ = arrivalUs;
    lastSampleTime_ = sampleTimeUs;
    haveLast_ = true;

    arrivals_[head_] = arrivalUs;
    head_ = (head_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;

    if (latencyUs >= 0) {
        const double ms = static_cast<double>(latencyUs) / 1000.0;
        snap_.lastLatencyMs = ms;
        latencySumMs_ += ms;
        ++latencyCount_;
    }
}

FrameStatsSnapshot FrameStats::Snapshot() const {
    FrameStatsSnapshot s = snap_;
    s.meanLatencyMs = latencyCount_ ? latencySumMs_ / static_cast<double>(latencyCount_) : -1;
    s.nominalFps = 1'000'000.0 / static_cast<double>(expectedUs_);
    if (count_ < 2) return s;

    // Walk the ring oldest → newest.
    const size_t oldest = (head_ + kWindow - count_) % kWindow;
    double sum = 0, sumSq = 0, maxI = 0;
    for (size_t k = 1; k < count_; ++k) {
        const std::int64_t a = arrivals_[(oldest + k - 1) % kWindow];
        const std::int64_t b = arrivals_[(oldest + k) % kWindow];
        const double iv = static_cast<double>(b - a) / 1000.0;
        sum += iv;
        sumSq += iv * iv;
        maxI = std::max(maxI, iv);
    }
    const double n = static_cast<double>(count_ - 1);
    s.meanIntervalMs = sum / n;
    s.maxIntervalMs = maxI;
    s.jitterMs = std::sqrt(std::max(0.0, sumSq / n - s.meanIntervalMs * s.meanIntervalMs));
    s.fps = s.meanIntervalMs > 0 ? 1000.0 / s.meanIntervalMs : 0;
    s.nominalFps = 1'000'000.0 / static_cast<double>(expectedUs_);
    // Judge only on a full window (~2 s at 30 FPS) so start-up ramp doesn't trigger it.
    s.underSpeed = count_ == kWindow && s.fps < s.nominalFps * 0.85;
    return s;
}

}  // namespace ixc::camera
