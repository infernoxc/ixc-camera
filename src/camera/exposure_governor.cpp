#include "camera/exposure_governor.h"

#include <algorithm>
#include <cmath>

namespace ixc::camera {

int ExposureGovernor::ExposureForFps(double fps) {
    if (fps <= 0) return -5;
    // floor(log2(interval)): 30 FPS (33.3 ms) → -5 (31.25 ms); 60 → -6; 15 → -4.
    return static_cast<int>(std::floor(std::log2(1.0 / fps)));
}

void ExposureGovernor::Reset(bool enabled, double nominalFps, const Hint& hint) {
    state_ = enabled && nominalFps > 0 ? State::Observing : State::Disabled;
    nominalFps_ = nominalFps;
    count_ = 0;
    skip_ = 0;
    rounds_ = 0;
    reasserts_ = 0;
    sumInterval_ = sumLuma_ = 0;
    lumaBefore_ = 0;
    compensationEv_ = 0;
    changed_ = false;
    fromHint_ = false;
    flicker_ = false;
    exposure_ = ExposureForFps(nominalFps);
    if (state_ == State::Observing && hint.flicker && std::abs(hint.fps - nominalFps) < 0.5) {
        // Fixed exposure banded under this room's lights last time: stay on auto exposure.
        state_ = State::Disabled;
        flicker_ = true;
        return;
    }
    if (state_ == State::Observing) skip_ = cfg_.settleFrames;  // auto exposure still converging at start
    if (state_ == State::Observing && hint.valid && std::abs(hint.fps - nominalFps) < 0.5) {
        // Start from the decision that worked last time; the first frame requests it.
        fromHint_ = true;
        exposure_ = hint.exposure;
        compensationEv_ = hint.compensationEv;
        state_ = State::SwitchRequested;
    }
}

ExposureGovernor::Hint ExposureGovernor::CurrentHint() const {
    Hint h;
    h.valid = state_ == State::Locked || state_ == State::Refining;
    h.fps = nominalFps_;
    h.exposure = exposure_;
    h.compensationEv = compensationEv_;
    h.flicker = flicker_;
    return h;
}

double ExposureGovernor::Compensation(double lumaAfter) const {
    if (lumaBefore_ <= 0 || lumaAfter <= 1 || lumaBefore_ <= lumaAfter) return 0;
    return std::clamp(std::log2(lumaBefore_ / lumaAfter), 0.0, cfg_.maxCompensationEv);
}

ExposureGovernor::Action ExposureGovernor::OnFrame(double intervalMs, double meanLuma) {
    if (state_ == State::SwitchRequested && fromHint_ && !changed_) return Action::SetManualExposure;  // remembered decision
    if (state_ == State::SwitchRequested || state_ == State::Disabled) return Action::None;
    if (intervalMs <= 0) return Action::None;  // first frame: no interval yet
    if (skip_ > 0) {
        --skip_;  // sensor still transitioning to the new exposure
        return Action::None;
    }
    sumInterval_ += intervalMs;
    sumLuma_ += meanLuma;
    ++count_;
    const int needed = state_ == State::Observing   ? cfg_.observeFrames
                       : state_ == State::Verifying ? cfg_.verifyFrames
                       : state_ == State::Refining  ? cfg_.refineFrames
                                                    : cfg_.watchFrames;
    if (count_ < needed) return Action::None;

    const double fps = 1000.0 * count_ / sumInterval_;
    const double luma = sumLuma_ / count_;
    count_ = 0;
    sumInterval_ = sumLuma_ = 0;

    if (state_ == State::Observing) {
        if (fps >= nominalFps_ * cfg_.underSpeedRatio) return Action::None;  // already smooth: keep watching
        lumaBefore_ = luma;
        state_ = State::SwitchRequested;
        return Action::SetManualExposure;
    }

    if (state_ == State::Refining || state_ == State::Locked) {
        if (fps < nominalFps_ * cfg_.underSpeedRatio) {
            // Slow again although the fixed exposure proved fast: something else (another app
            // sharing the camera, ending its own session) gave the camera back its auto exposure.
            ++reasserts_;
            return Action::SetManualExposure;
        }
        if (state_ == State::Locked) return Action::None;
        // Brightness only: some cameras keep adjusting gain for a few seconds after the switch.
        compensationEv_ = Compensation(luma);
        if (++rounds_ >= cfg_.refineRounds) state_ = State::Locked;
        return Action::None;
    }

    // Verifying the fixed exposure.
    if (fps < nominalFps_ * cfg_.underSpeedRatio) {
        compensationEv_ = 0;
        changed_ = false;
        if (fromHint_) {
            // Conditions changed since last time: back to auto and decide from scratch.
            fromHint_ = false;
            exposure_ = ExposureForFps(nominalFps_);
            skip_ = cfg_.settleFrames;
            state_ = State::Observing;
        } else {
            state_ = State::Disabled;  // the camera didn't speed up: automatic exposure is better
        }
        return Action::RestoreAutoExposure;
    }
    // A remembered decision keeps its compensation (there's no auto-exposure baseline this session).
    if (!fromHint_) compensationEv_ = Compensation(luma);
    rounds_ = 0;
    state_ = !fromHint_ && cfg_.refineFrames > 0 && cfg_.refineRounds > 0 ? State::Refining : State::Locked;
    return Action::None;
}

ExposureGovernor::Action ExposureGovernor::AbortForFlicker() {
    if (state_ == State::Disabled && !changed_) return Action::None;
    const bool restore = changed_;
    changed_ = false;
    compensationEv_ = 0;
    fromHint_ = false;
    flicker_ = true;
    state_ = State::Disabled;
    return restore ? Action::RestoreAutoExposure : Action::None;
}

void ExposureGovernor::OnExposureApplied(bool ok) {
    if (state_ == State::Refining || state_ == State::Locked) {  // watchdog re-assert
        skip_ = cfg_.settleFrames;
        count_ = 0;
        sumInterval_ = sumLuma_ = 0;
        return;
    }
    if (state_ != State::SwitchRequested) return;
    if (!ok) {
        state_ = State::Disabled;  // no manual exposure control on this camera
        return;
    }
    changed_ = true;
    state_ = State::Verifying;
    skip_ = cfg_.settleFrames;
    count_ = 0;
    sumInterval_ = sumLuma_ = 0;
}

}  // namespace ixc::camera
