#pragma once

// "Smooth motion": keeps the camera at its negotiated frame rate in low light.
//
// Measured on the Lenovo FHD Webcam (docs/performance.md): with automatic exposure in a normally
// lit room the camera chose ~1/16 s exposures and delivered an uneven ~20 FPS (48/64 ms frames)
// on a 30 FPS mode, which looks choppy on a 60 FPS canvas. The same camera with a fixed 1/32 s
// exposure delivers a steady 30 FPS but a darker picture.
//
// So, while IXC Camera streams:
//   1. Observe: with the camera's automatic exposure, measure the real frame rate and brightness.
//   2. If it's clearly below the negotiated rate for ~2 s, request the longest fixed exposure that
//      fits one frame interval (UVC exposure units: log2 seconds, e.g. -5 = 1/32 s).
//   3. Verify the frame rate recovered and measure how much darker the picture became; compensate
//      in software (a lookup-table gain, capped) so brightness stays close to before.
//   4. If the camera doesn't speed up, give the exposure back to automatic and stop trying.
// The caller restores automatic exposure when the session ends, so other apps see the camera
// exactly as before.
//
// Pure logic: the caller feeds frame times and brightness, and applies the requested actions.

#include <cstdint>

namespace ixc::camera {

class ExposureGovernor {
public:
    struct Config {
        int observeFrames = 20;          // ~1 s at 20 FPS before deciding
        double underSpeedRatio = 0.85;   // below this fraction of the nominal rate = too slow
        int settleFrames = 8;            // frames ignored at session start and right after switching
        int verifyFrames = 20;           // frames measured after switching
        int refineFrames = 30;           // later windows that re-measure brightness (gain may drift)
        int refineRounds = 3;
        int watchFrames = 30;            // once locked: frame-rate check window (timing only)
        double maxCompensationEv = 1.5;  // cap on software brightness gain (noise grows with gain)
    };

    // A decision that worked in an earlier session (same camera, same frame rate). Starting from
    // it avoids ~2 s of choppy video at every session start; it's still verified.
    struct Hint {
        bool valid = false;
        double fps = 0;
        int exposure = 0;
        double compensationEv = 0;
        bool flicker = false;  // fixed exposure caused mains-light bands at this rate: don't fix it
    };

    enum class State { Observing, SwitchRequested, Verifying, Refining, Locked, Disabled };
    enum class Action { None, SetManualExposure, RestoreAutoExposure };

    ExposureGovernor() : ExposureGovernor(Config{}) {}
    explicit ExposureGovernor(Config c) : cfg_(c) {}

    // New session. enabled = user setting; nominalFps = negotiated rate; hint = last good decision.
    void Reset(bool enabled, double nominalFps, const Hint& hint);
    void Reset(bool enabled, double nominalFps) { Reset(enabled, nominalFps, Hint{}); }
    // The decision to remember for the next session (valid only when Locked).
    Hint CurrentHint() const;

    // Per frame: interval since the previous frame (ms, <= 0 for the first) and mean luma (0..255).
    // Returns the action the caller must perform now (at most one per call).
    Action OnFrame(double intervalMs, double meanLuma);
    // Result of applying SetManualExposure (false = the camera refused: give up).
    void OnExposureApplied(bool ok);
    // The fixed exposure produces mains-light bands (camera/flicker_detector.h): bands are worse
    // than a lower frame rate. Returns RestoreAutoExposure when the exposure must be given back;
    // the governor then stays disabled, and CurrentHint() remembers why.
    Action AbortForFlicker();
    bool FlickerAborted() const { return flicker_; }

    int RequestedExposure() const { return exposure_; }  // log2 seconds, valid with SetManualExposure
    double CompensationEv() const { return compensationEv_; }
    bool ExposureChanged() const { return changed_; }     // caller must restore auto at session end
    State state() const { return state_; }
    int Reasserts() const { return reasserts_; }  // watchdog re-applications (diagnostics)
    // Brightness samples are only needed while deciding; once locked only frame times are.
    bool NeedsLuma() const { return state_ == State::Observing || state_ == State::Verifying || state_ == State::Refining; }

    // Longest UVC exposure value (log2 seconds) whose duration fits one frame interval.
    static int ExposureForFps(double fps);

private:
    double Compensation(double lumaAfter) const;

    Config cfg_;
    State state_ = State::Disabled;
    double nominalFps_ = 30;
    int count_ = 0;
    int skip_ = 0;    // settle frames still to ignore
    int rounds_ = 0;  // refine windows done
    int reasserts_ = 0;
    double sumInterval_ = 0, sumLuma_ = 0;
    double lumaBefore_ = 0;
    int exposure_ = 0;
    double compensationEv_ = 0;
    bool changed_ = false;
    bool fromHint_ = false;  // current attempt started from a remembered decision
    bool flicker_ = false;   // gave up because of mains-light bands
};

}  // namespace ixc::camera
