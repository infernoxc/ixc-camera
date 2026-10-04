#pragma once

// Auto-framing: keeps the tracked face framed by zooming and panning smoothly inside the
// rectangle the user's crop/zoom settings show (the "base" rectangle). Pure logic: no threads,
// no allocation, unit-tested (test_auto_framer.cpp).
//
// Behaviour:
//   * Target view: the face takes ~30% of the view height, with its centre 40% from the top
//     (head and shoulders). Zoom is limited to 2.5x the base view (digital zoom: beyond that the
//     picture gets visibly soft).
//   * Deadband: the target only moves when the face leaves the comfortable middle of the view or
//     changes size noticeably, so small head movements don't make the picture wander.
//   * Motion: the view eases towards the target (time constant ~0.7 s), never jumps.
//   * No face for 2 s: eases back out to the base view.
//
// Coordinates are normalized to the source frame (like the face tracker and PipelineParams).
// The view always keeps the base rectangle's aspect ratio, so output pixels stay square.

#include "face/face_types.h"

namespace ixc::face {

struct ViewRect {
    double x = 0, y = 0, w = 1, h = 1;
    bool operator==(const ViewRect&) const = default;
};

class AutoFramer {
public:
    static constexpr double kFaceHeightFraction = 0.30;
    static constexpr double kFaceCentreFromTop = 0.40;
    static constexpr double kMaxZoom = 2.5;
    static constexpr double kTimeConstantMs = 700;
    static constexpr double kLostFaceMs = 2000;

    void Reset() { *this = AutoFramer(); }

    // Per frame. base: the view the user's settings produce. face: the largest tracked face
    // (nullptr when none). Returns the view to show this frame (inside base).
    ViewRect Update(const ViewRect& base, const RectF* face, double nowMs);

    // The view Update last returned (base until the first update).
    const ViewRect& Current() const { return current_; }
    // True while the view differs from the base view.
    bool Framing() const { return framing_; }

private:
    ViewRect TargetFor(const ViewRect& base, const RectF& face) const;

    bool started_ = false;
    bool framing_ = false;
    ViewRect current_, target_, lastBase_;
    double lastMs_ = 0;
    double lastFaceMs_ = -1e18;
};

// The largest face in a snapshot, or nullptr.
const RectF* LargestFace(const FaceSnapshot& s);

}  // namespace ixc::face
