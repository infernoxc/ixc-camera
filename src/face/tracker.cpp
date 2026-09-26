#include "face/tracker.h"

#include <algorithm>
#include <cmath>

namespace ixc::face {

namespace {
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
void Blend(PointF& s, const PointF& m, float a) {
    s.x = Lerp(s.x, m.x, a);
    s.y = Lerp(s.y, m.y, a);
}
}  // namespace

void Tracker::Reset() {
    count_ = 0;
    // IDs keep increasing: a face that comes back is a new track (consumers restart effects).
}

void Tracker::Shift(TrackedFace& f, float dx, float dy) {
    f.box.x += dx;
    f.box.y += dy;
    for (PointF* p : {&f.lm.leftEye, &f.lm.rightEye, &f.lm.nose, &f.lm.mouthLeft, &f.lm.mouthRight}) {
        p->x += dx;
        p->y += dy;
    }
}

void Tracker::Correct(Track& tr, const Detection& d, double t) const {
    const double dt = t - tr.t;
    // Predicted state at t (same extrapolation as Predict), then blended with the measurement.
    TrackedFace pred = tr.f;
    const double pdt = std::clamp(dt, 0.0, cfg_.maxPredictMs);
    Shift(pred, static_cast<float>(tr.vx * pdt), static_cast<float>(tr.vy * pdt));

    const PointF oldC = tr.f.box.Center(), newC = d.box.Center();
    const float size = std::max(1e-3f, d.box.w);
    const float motion = std::hypot(newC.x - pred.box.Center().x, newC.y - pred.box.Center().y) / size;
    const float a = Lerp(cfg_.alphaStill, cfg_.alphaMoving, std::clamp(motion / cfg_.fastMotion, 0.0f, 1.0f));

    TrackedFace f = pred;
    f.box.x = Lerp(pred.box.x, d.box.x, a);
    f.box.y = Lerp(pred.box.y, d.box.y, a);
    f.box.w = Lerp(pred.box.w, d.box.w, a);
    f.box.h = Lerp(pred.box.h, d.box.h, a);
    if (d.landmarksPlausible) {
        if (tr.f.landmarksValid) {
            Blend(f.lm.leftEye, d.lm.leftEye, a);
            Blend(f.lm.rightEye, d.lm.rightEye, a);
            Blend(f.lm.nose, d.lm.nose, a);
            Blend(f.lm.mouthLeft, d.lm.mouthLeft, a);
            Blend(f.lm.mouthRight, d.lm.mouthRight, a);
        } else {
            f.lm = d.lm;  // first valid landmarks: take them as they are
        }
        f.landmarksValid = true;
    } else {
        // Implausible landmarks this time: keep the old ones, moved with the box.
        f.landmarksValid = tr.f.landmarksValid && tr.f.detections > 0;
    }
    f.confidence = Lerp(tr.f.confidence, d.confidence, 0.5f);
    f.detections = tr.f.detections + 1;
    f.missed = 0;

    // Velocity from the smoothed centre movement; reset after long gaps (stale).
    const PointF c = f.box.Center();
    if (dt > 1 && dt < 600) {
        const float nvx = static_cast<float>((c.x - oldC.x) / dt), nvy = static_cast<float>((c.y - oldC.y) / dt);
        tr.vx = Lerp(tr.vx, nvx, 0.6f);
        tr.vy = Lerp(tr.vy, nvy, 0.6f);
    } else {
        tr.vx = tr.vy = 0;
    }
    tr.motion = motion;
    tr.f = f;
    tr.t = t;
    tr.matched = true;
}

void Tracker::Update(const Detection* d, int n, double timeMs, int maxFaces) {
    maxFaces = std::clamp(maxFaces, 1, kMaxFaces);
    for (int i = 0; i < count_; ++i) tracks_[i].matched = false;

    bool used[kMaxFaces * 4] = {};
    n = std::min(n, kMaxFaces * 4);
    // Greedy association, best-confidence detections first.
    for (int j = 0; j < n; ++j) {
        int best = -1;
        float bestIoU = cfg_.matchIoU;
        for (int i = 0; i < count_; ++i) {
            if (tracks_[i].matched) continue;
            TrackedFace pred = tracks_[i].f;
            const double pdt = std::clamp(timeMs - tracks_[i].t, 0.0, cfg_.maxPredictMs);
            Shift(pred, static_cast<float>(tracks_[i].vx * pdt), static_cast<float>(tracks_[i].vy * pdt));
            const float iou = IoU(pred.box, d[j].box);
            if (iou >= bestIoU) {
                bestIoU = iou;
                best = i;
            }
        }
        if (best >= 0) {
            Correct(tracks_[best], d[j], timeMs);
            used[j] = true;
        }
    }
    // Misses: lower confidence, drop after maxMissed.
    for (int i = 0; i < count_;) {
        Track& tr = tracks_[i];
        if (!tr.matched) {
            ++tr.f.missed;
            tr.f.confidence *= 0.6f;
            tr.vx *= 0.5f;
            tr.vy *= 0.5f;
            if (tr.f.missed > cfg_.maxMissed) {
                tracks_[i] = tracks_[--count_];
                continue;
            }
        }
        ++i;
    }
    // New faces, while there's room.
    for (int j = 0; j < n && count_ < maxFaces; ++j) {
        if (used[j]) continue;
        Track& tr = tracks_[count_++];
        tr = Track{};
        tr.f.id = nextId_++;
        tr.f.box = d[j].box;
        tr.f.lm = d[j].lm;
        tr.f.landmarksValid = d[j].landmarksPlausible;
        tr.f.confidence = d[j].confidence;
        tr.f.detections = 1;
        tr.t = timeMs;
        tr.matched = true;
        tr.motion = 1;  // new: not stable yet
    }
}

void Tracker::Predict(double timeMs, FaceSnapshot& out) const {
    out.count = 0;
    for (int i = 0; i < count_; ++i) {
        const Track& tr = tracks_[i];
        TrackedFace f = tr.f;
        const double dt = std::clamp(timeMs - tr.t, 0.0, cfg_.maxPredictMs);
        Shift(f, static_cast<float>(tr.vx * dt), static_cast<float>(tr.vy * dt));
        DeriveGeometry(f);
        out.faces[static_cast<size_t>(out.count++)] = f;
    }
}

bool Tracker::Stable() const {
    if (count_ == 0) return false;
    for (int i = 0; i < count_; ++i) {
        const Track& tr = tracks_[i];
        if (!tr.matched || tr.motion > cfg_.stableMotion || tr.f.detections < 3) return false;
        // Well predicted isn't enough: a steadily moving face still needs frequent detections.
        const float speed = std::hypot(tr.vx, tr.vy) * 1000.0f / std::max(1e-3f, tr.f.box.w);  // box widths per second
        if (speed > cfg_.stableSpeed) return false;
    }
    return true;
}

float Tracker::SmallestFace() const {
    float s = 0;
    for (int i = 0; i < count_; ++i) s = i == 0 ? tracks_[i].f.box.w : std::min(s, tracks_[i].f.box.w);
    return s;
}

}  // namespace ixc::face
