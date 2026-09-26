#pragma once

// Face tracking data shared by the detector, tracker and consumers (preview overlay, effects).
// All coordinates are normalized to the SOURCE frame (0..1, top-left origin, before IXC's crop,
// zoom and mirror), so they don't depend on resolution. MapToOutput converts them to the
// processed output (see processing::PipelineParams).

#include <array>
#include <cstdint>

namespace ixc::face {

inline constexpr int kMaxFaces = 4;

struct PointF {
    float x = 0, y = 0;
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    PointF Center() const { return {x + w / 2, y + h / 2}; }
};

// Five-point landmarks as the detector reports them. "Left"/"right" are image left/right (the
// person's right eye is on the image left for a non-mirrored camera).
struct Landmarks {
    PointF leftEye, rightEye, nose, mouthLeft, mouthRight;
};

// One detector result.
struct Detection {
    RectF box;
    Landmarks lm;
    float confidence = 0;         // 0..1
    bool landmarksPlausible = false;  // geometry sanity check passed (see IsPlausible)
};

// A face followed over time. Everything is smoothed; between detections it is predicted.
struct TrackedFace {
    int id = 0;                   // stable while the face stays tracked; never reused in a session
    RectF box;
    Landmarks lm;
    bool landmarksValid = false;  // false: consumers should hold/fade landmark-anchored effects
    float confidence = 0;         // 0..1, decays while the face isn't re-detected
    // Derived (estimates from the five points; enough to anchor 2D effects):
    PointF mouthCenter;
    RectF leftBrow, rightBrow;    // regions just above each eye
    float rollDeg = 0;            // head tilt: angle of the eye line (positive = clockwise in the image)
    float yawDeg = 0;             // left/right turn estimated from the nose offset between the eyes
    float pitchDeg = 0;           // up/down estimated from the nose position between eyes and mouth
    int detections = 0;           // times matched by the detector
    int missed = 0;               // consecutive detections without a match
};

struct FaceSnapshot {
    std::array<TrackedFace, kMaxFaces> faces{};
    int count = 0;
};

// Sanity check of the five points (inside the box, eyes above the nose above the mouth, left/right
// order). The 160x90 input fails this ~30% of the time, so landmark effects must honour it.
bool IsPlausible(const Detection& d);

// Fills the derived fields (mouth centre, brows, roll/yaw/pitch) from box + landmarks.
void DeriveGeometry(TrackedFace& f);

float IoU(const RectF& a, const RectF& b);

// Source → processed output (crop/zoom rectangle and mirror, as in processing::PipelineParams).
struct OutputMapping {
    float x = 0, y = 0, w = 1, h = 1;  // source rectangle shown in the output
    bool mirror = false;
};
PointF MapToOutput(const PointF& p, const OutputMapping& m);
RectF MapToOutput(const RectF& r, const OutputMapping& m);

}  // namespace ixc::face
