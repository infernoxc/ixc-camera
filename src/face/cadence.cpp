#include "face/cadence.h"

#include <algorithm>

namespace ixc::face {

int InputWidth(InputSize s) { return s == InputSize::Small ? 160 : s == InputSize::Medium ? 240 : 320; }
int InputHeight(InputSize s) { return s == InputSize::Small ? 90 : s == InputSize::Medium ? 135 : 180; }

void Cadence::Reset(double nowMs) {
    size_ = InputSize::Medium;
    cost_ = mediumCost_ = 0;
    samples_ = 0;
    lastFaceMs_ = now_ = nowMs;
    searching_ = true;
    tooSlow_ = false;
}

void Cadence::Resize(InputSize s) {
    size_ = s;
    cost_ = 0;
    samples_ = 0;
}

void Cadence::OnDetection(double costMs, double nowMs, int faces, float smallestFace) {
    now_ = nowMs;
    // (The engine warms the detector up once before the first timed run.)
    cost_ = samples_ == 0 ? costMs : cost_ * 0.7 + costMs * 0.3;
    ++samples_;
    if (size_ == InputSize::Medium) mediumCost_ = cost_;

    searching_ = faces == 0;
    if (faces > 0) lastFaceMs_ = nowMs;
    if (samples_ < 3) return;  // decide on a settled measurement

    // Can this size reach minHz within the budget?
    const double hzAffordable = cost_ > 0 ? cfg_.budget * 1000.0 / cost_ : cfg_.maxHz;
    if (hzAffordable < cfg_.minHz) {
        if (size_ == InputSize::Large) Resize(InputSize::Medium);
        else if (size_ == InputSize::Medium) Resize(InputSize::Small);
        else tooSlow_ = true;
        return;
    }
    // Larger input for small or undetected faces, only on fast CPUs.
    const bool fast = mediumCost_ > 0 && mediumCost_ < cfg_.largeMaxMediumMs;
    const bool wantLarge = (faces > 0 && smallestFace < 0.12f) || (faces == 0 && nowMs - lastFaceMs_ > 2000);
    if (size_ == InputSize::Medium && fast && wantLarge) {
        Resize(InputSize::Large);
    } else if (size_ == InputSize::Large && faces > 0 && smallestFace > 0.2f) {
        Resize(InputSize::Medium);
    }
}

double Cadence::IntervalMs(bool stable, double frameIntervalMs) const {
    if (cfg_.fixedIntervalFrames > 0 && frameIntervalMs > 0) {
        // User override, still never beyond the budget.
        const double fixed = cfg_.fixedIntervalFrames * frameIntervalMs;
        return cost_ > 0 ? std::max(fixed, cost_ / cfg_.budget) : fixed;
    }
    double hz = searching_ ? (now_ - lastFaceMs_ > cfg_.idleAfterMs ? cfg_.idleHz : cfg_.searchHz)
                           : (stable ? cfg_.stableHz : cfg_.movingHz);
    if (cost_ > 0) hz = std::min(hz, cfg_.budget * 1000.0 / cost_);
    hz = std::clamp(hz, cfg_.minHz, cfg_.maxHz);
    return 1000.0 / hz;
}

}  // namespace ixc::face
