#include "face/face_types.h"

#include <algorithm>
#include <cmath>

namespace ixc::face {

namespace {
constexpr float kRadToDeg = 57.2957795f;

bool Inside(const PointF& p, const RectF& r) {
    // Landmarks may sit slightly outside a tight box (mouth corners on a turned face).
    const float mx = r.w * 0.1f, my = r.h * 0.1f;
    return p.x >= r.x - mx && p.x <= r.x + r.w + mx && p.y >= r.y - my && p.y <= r.y + r.h + my;
}
}  // namespace

bool IsPlausible(const Detection& d) {
    const Landmarks& l = d.lm;
    if (d.box.w <= 0 || d.box.h <= 0) return false;
    for (const PointF* p : {&l.leftEye, &l.rightEye, &l.nose, &l.mouthLeft, &l.mouthRight}) {
        if (!Inside(*p, d.box)) return false;
    }
    const float eyeY = (l.leftEye.y + l.rightEye.y) / 2, mouthY = (l.mouthLeft.y + l.mouthRight.y) / 2;
    return l.leftEye.x < l.rightEye.x && l.mouthLeft.x < l.mouthRight.x && eyeY < l.nose.y && l.nose.y < mouthY;
}

void DeriveGeometry(TrackedFace& f) {
    const Landmarks& l = f.lm;
    f.mouthCenter = {(l.mouthLeft.x + l.mouthRight.x) / 2, (l.mouthLeft.y + l.mouthRight.y) / 2};

    const float ex = l.rightEye.x - l.leftEye.x, ey = l.rightEye.y - l.leftEye.y;
    const float eyeDist = std::sqrt(ex * ex + ey * ey);
    f.rollDeg = std::atan2(ey, ex) * kRadToDeg;

    // Brows: a box above each eye, sized from the eye distance.
    const float bw = eyeDist * 0.55f, bh = eyeDist * 0.22f, lift = eyeDist * 0.30f;
    f.leftBrow = {l.leftEye.x - bw / 2, l.leftEye.y - lift - bh / 2, bw, bh};
    f.rightBrow = {l.rightEye.x - bw / 2, l.rightEye.y - lift - bh / 2, bw, bh};

    // Yaw: where the nose sits between the eyes (0.5 = frontal). ±0.5 of offset ≈ ±60°.
    if (eyeDist > 1e-6f) {
        const float t = ((l.nose.x - l.leftEye.x) * ex + (l.nose.y - l.leftEye.y) * ey) / (eyeDist * eyeDist);
        f.yawDeg = std::clamp((t - 0.5f) * 120.0f, -60.0f, 60.0f);
    }
    // Pitch: nose height between the eye line and the mouth line (≈0.5 frontal). Rough.
    const float eyeY = (l.leftEye.y + l.rightEye.y) / 2;
    const float span = f.mouthCenter.y - eyeY;
    if (span > 1e-6f) {
        const float t = (l.nose.y - eyeY) / span;
        f.pitchDeg = std::clamp((t - 0.55f) * 120.0f, -45.0f, 45.0f);
    }
}

float IoU(const RectF& a, const RectF& b) {
    const float ix = std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    const float iy = std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    const float inter = ix * iy;
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0;
}

PointF MapToOutput(const PointF& p, const OutputMapping& m) {
    PointF o{(p.x - m.x) / m.w, (p.y - m.y) / m.h};
    if (m.mirror) o.x = 1 - o.x;
    return o;
}

RectF MapToOutput(const RectF& r, const OutputMapping& m) {
    RectF o{(r.x - m.x) / m.w, (r.y - m.y) / m.h, r.w / m.w, r.h / m.h};
    if (m.mirror) o.x = 1 - o.x - o.w;
    return o;
}

}  // namespace ixc::face
