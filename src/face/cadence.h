#pragma once

// Adaptive detection cadence and detector input size (pure logic, unit-tested).
//
// The video never waits for the detector. This decides how often to run it and on how large an
// image, within a CPU budget (a fraction of ONE core, so it means the same on a dual-core):
//   * rate: 8 Hz while faces move, 3 Hz when tracking is stable, 4 Hz while searching, 2 Hz after
//     several seconds without a face;
//   * the budget caps the rate: rate <= budget / measured detection cost;
//   * size: 240x135 by default; 320x180 for small/distant faces when the CPU is fast;
//     160x90 when 240x135 can't reach 2 Hz within the budget (face box only; landmarks
//     unreliable there);
//   * when even 160x90 can't reach 2 Hz within budget, tracking is turned off (TooSlow) rather
//     than slowing the camera.

namespace ixc::face {

enum class InputSize { Small, Medium, Large };  // 160x90, 240x135, 320x180
int InputWidth(InputSize s);
int InputHeight(InputSize s);

struct CadenceConfig {
    double budget = 0.10;           // fraction of one CPU core
    double minHz = 2, maxHz = 10;
    double movingHz = 8, stableHz = 3, searchHz = 4, idleHz = 2;
    double idleAfterMs = 5000;      // searching this long without a face → idleHz
    double largeMaxMediumMs = 6;    // Large allowed only when Medium costs less than this
    int fixedIntervalFrames = 0;    // user override (profile): detect every N frames; 0 = adaptive
};

class Cadence {
public:
    Cadence() : Cadence(CadenceConfig{}) {}
    explicit Cadence(CadenceConfig c) : cfg_(c) {}

    void Reset(double nowMs);
    // Cost of the detection just finished (thread CPU time, ms) at the current size.
    void OnDetection(double costMs, double nowMs, int faces, float smallestFace);
    // Milliseconds until the next detection should start.
    double IntervalMs(bool stable, double frameIntervalMs) const;

    InputSize Size() const { return size_; }
    bool TooSlow() const { return tooSlow_; }
    double CostMs() const { return cost_; }  // smoothed cost at the current size

private:
    void Resize(InputSize s);

    CadenceConfig cfg_;
    InputSize size_ = InputSize::Medium;
    double cost_ = 0;          // EMA of the detection cost at size_
    int samples_ = 0;
    double mediumCost_ = 0;    // last known Medium cost (to decide about Large)
    double lastFaceMs_ = 0;
    bool searching_ = true;
    bool tooSlow_ = false;
    double now_ = 0;
};

}  // namespace ixc::face
