#include "face/auto_framer.h"

#include <algorithm>
#include <cmath>

namespace ixc::face {

namespace {

// Moves r (keeping its size) so it lies inside base.
ViewRect ClampInside(ViewRect r, const ViewRect& base) {
    r.w = std::min(r.w, base.w);
    r.h = std::min(r.h, base.h);
    // max(): when r is as large as base, base.x + base.w - r.w can round to just below base.x,
    // and std::clamp requires lo <= hi.
    r.x = std::clamp(r.x, base.x, std::max(base.x, base.x + base.w - r.w));
    r.y = std::clamp(r.y, base.y, std::max(base.y, base.y + base.h - r.h));
    return r;
}

bool SameRect(const ViewRect& a, const ViewRect& b, double eps) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.w - b.w) < eps && std::abs(a.h - b.h) < eps;
}

}  // namespace

const RectF* LargestFace(const FaceSnapshot& s) {
    const RectF* best = nullptr;
    for (int i = 0; i < s.count && i < kMaxFaces; ++i) {
        const RectF& b = s.faces[static_cast<size_t>(i)].box;
        if (b.w > 0 && b.h > 0 && (!best || b.w * b.h > best->w * best->h)) best = &b;
    }
    return best;
}

ViewRect AutoFramer::TargetFor(const ViewRect& base, const RectF& face) const {
    const double aspect = base.w / base.h;  // normalized width per normalized height
    double h = std::clamp(static_cast<double>(face.h) / kFaceHeightFraction, base.h / kMaxZoom, base.h);
    ViewRect t;
    t.h = h;
    t.w = h * aspect;
    if (t.w > base.w) {  // can't be wider than the base view
        t.w = base.w;
        t.h = t.w / aspect;
    }
    const double cx = face.x + face.w / 2.0, cy = face.y + face.h / 2.0;
    t.x = cx - t.w / 2;
    t.y = cy - kFaceCentreFromTop * t.h;
    return ClampInside(t, base);
}

ViewRect AutoFramer::Update(const ViewRect& base, const RectF* face, double nowMs) {
    if (!started_ || !SameRect(base, lastBase_, 1e-9)) {
        // First frame, or the user changed crop/zoom: restart from their view.
        const bool wasStarted = started_;
        started_ = true;
        lastBase_ = base;
        current_ = target_ = base;
        if (!wasStarted) lastMs_ = nowMs;
    }
    const double dt = std::clamp(nowMs - lastMs_, 0.0, 250.0);  // a stall never causes a jump
    lastMs_ = nowMs;

    if (face) {
        lastFaceMs_ = nowMs;
        const ViewRect want = TargetFor(base, *face);
        // Deadband: keep the current target while the face stays comfortably framed in it.
        const double fx = face->x + face->w / 2.0, fy = face->y + face->h / 2.0;
        const bool inside = fx > target_.x + 0.3 * target_.w && fx < target_.x + 0.7 * target_.w && fy > target_.y + 0.2 * target_.h &&
                            fy < target_.y + 0.6 * target_.h;
        const double sizeRatio = want.h / target_.h;
        if (!inside || sizeRatio < 0.85 || sizeRatio > 1.18) target_ = want;
    } else if (nowMs - lastFaceMs_ > kLostFaceMs) {
        target_ = base;
    }

    // Ease towards the target: exponential approach, frame-rate independent.
    const double k = 1.0 - std::exp(-dt / kTimeConstantMs);
    current_.x += (target_.x - current_.x) * k;
    current_.y += (target_.y - current_.y) * k;
    current_.w += (target_.w - current_.w) * k;
    current_.h += (target_.h - current_.h) * k;
    current_.h = current_.w / (base.w / base.h);  // exact aspect despite rounding
    current_ = ClampInside(current_, base);
    // Snap the last fraction of a pixel so a still scene settles on a fixed rectangle (the
    // geometry tables are then not rebuilt every frame).
    if (SameRect(current_, target_, 2e-4)) current_ = target_;
    framing_ = !SameRect(current_, base, 1e-6);
    return current_;
}

}  // namespace ixc::face
