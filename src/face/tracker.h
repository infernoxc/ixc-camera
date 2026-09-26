#pragma once

// Temporal face tracking between detector runs (pure logic, unit-tested without a camera).
//
// The detector runs a few times per second; consumers ask for faces on every video frame.
// The tracker:
//   * associates detections with existing tracks (IoU against the predicted box), so each
//     face keeps its ID;
//   * smooths boxes and landmarks adaptively: strongly when still (no jitter), lightly when
//     moving (no lag);
//   * predicts positions between detections from the smoothed velocity (bounded), so effects
//     follow motion at the video frame rate even though inference is slower;
//   * lowers confidence and finally drops a face after consecutive misses.

#include "face/face_types.h"

namespace ixc::face {

struct TrackerConfig {
    float matchIoU = 0.2f;        // minimum overlap to continue a track
    int maxMissed = 2;            // misses in a row before a track is dropped
    float alphaStill = 0.35f;     // smoothing weight of a new measurement when the face is still
    float alphaMoving = 0.9f;     // ... when it moves by >= fastMotion
    float fastMotion = 0.12f;     // box-widths per detection considered "fast"
    double maxPredictMs = 200;    // never extrapolate further than this
    float stableMotion = 0.04f;   // prediction error (box widths) below which a track can be "stable"
    float stableSpeed = 0.15f;    // ... and speed (box widths per second)
};

class Tracker {
public:
    Tracker() : Tracker(TrackerConfig{}) {}
    explicit Tracker(TrackerConfig c) : cfg_(c) {}

    void Reset();
    // Detections (highest confidence first) measured on the frame captured at timeMs.
    void Update(const Detection* d, int n, double timeMs, int maxFaces);
    // Faces predicted for timeMs (usually "now").
    void Predict(double timeMs, FaceSnapshot& out) const;

    int Count() const { return count_; }
    // Every track was re-detected last time and is barely moving: detection can slow down.
    bool Stable() const;
    // Smallest tracked face width (normalized), 0 without faces.
    float SmallestFace() const;

private:
    struct Track {
        TrackedFace f;          // smoothed state at t
        double t = 0;
        float vx = 0, vy = 0;   // velocity of the box centre, normalized units per ms
        float motion = 0;       // last measured motion, box widths
        bool matched = false;
    };
    void Correct(Track& tr, const Detection& d, double t) const;
    static void Shift(TrackedFace& f, float dx, float dy);

    TrackerConfig cfg_;
    Track tracks_[kMaxFaces];
    int count_ = 0;
    int nextId_ = 1;
};

}  // namespace ixc::face
