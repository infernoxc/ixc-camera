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

FramerParams FramerParamsFor(double tightness, double speed, double maxZoom) {
    const double t = std::clamp(tightness, 0.0, 100.0) / 100.0;
    const double v = std::clamp(speed, 0.0, 100.0) / 100.0;
    FramerParams p;
    // Piecewise: 0 = waist up (face 14% of the view), 50 = head and shoulders (30%), 100 = face (55%).
    p.faceHeightFraction = t < 0.5 ? 0.14 + (0.30 - 0.14) * (t / 0.5) : 0.30 + (0.55 - 0.30) * ((t - 0.5) / 0.5);
    p.faceCentreFromTop = t < 0.5 ? 0.32 + (0.40 - 0.32) * (t / 0.5) : 0.40 + (0.46 - 0.40) * ((t - 0.5) / 0.5);
    // Geometric: 0 → 1.6 s, 50 → ~440 ms, 100 → 120 ms.
    p.timeConstantMs = 1600.0 * std::pow(120.0 / 1600.0, v);
    p.deadband = 1.0 - 0.6 * v;  // fast framing re-centres on smaller movements
    p.maxZoom = std::clamp(maxZoom, 1.0, 4.0);
    return p;
}

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
    double h = std::clamp(static_cast<double>(face.h) / params_.faceHeightFraction, base.h / params_.maxZoom, base.h);
    ViewRect t;
    t.h = h;
    t.w = h * aspect;
    if (t.w > base.w) {  // can't be wider than the base view
        t.w = base.w;
        t.h = t.w / aspect;
    }
    const double cx = face.x + face.w / 2.0, cy = face.y + face.h / 2.0;
    t.x = cx - t.w / 2;
    t.y = cy - params_.faceCentreFromTop * t.h;
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
        // The comfortable zone shrinks with the deadband (fast framing re-centres sooner).
        const double db = std::clamp(params_.deadband, 0.1, 1.0);
        const double cyRef = params_.faceCentreFromTop;
        const bool inside = std::abs(fx - (target_.x + 0.5 * target_.w)) < 0.2 * db * target_.w &&
                            std::abs(fy - (target_.y + cyRef * target_.h)) < 0.2 * db * target_.h;
        const double sizeRatio = want.h / target_.h;
        if (!inside || sizeRatio < 1.0 - 0.15 * db || sizeRatio > 1.0 + 0.18 * db) target_ = want;
    } else if (nowMs - lastFaceMs_ > kLostFaceMs) {
        target_ = base;
    }

    // Ease towards the target: exponential approach, frame-rate independent.
    const double k = 1.0 - std::exp(-dt / std::max(1.0, params_.timeConstantMs));
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
