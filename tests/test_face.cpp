#include "face/cadence.h"
#include "face/detector.h"
#include "face/downscale.h"
#include "face/face_engine.h"
#include "face/face_types.h"
#include "face/tracker.h"
#include "ixc_test.h"

#include <cmath>
#include <vector>

using namespace ixc::face;

namespace {
// A frontal face with plausible landmarks at (x, y), size s (normalized).
Detection MakeFace(float x, float y, float s, float conf = 0.9f) {
    Detection d;
    d.box = {x, y, s, s * 1.2f};
    d.lm.leftEye = {x + s * 0.3f, y + s * 0.4f};
    d.lm.rightEye = {x + s * 0.7f, y + s * 0.4f};
    d.lm.nose = {x + s * 0.5f, y + s * 0.62f};
    d.lm.mouthLeft = {x + s * 0.35f, y + s * 0.85f};
    d.lm.mouthRight = {x + s * 0.65f, y + s * 0.85f};
    d.confidence = conf;
    d.landmarksPlausible = IsPlausible(d);
    return d;
}
bool Near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
}  // namespace

IXC_TEST(Face_PlausibilityRejectsSwappedLandmarks) {
    Detection d = MakeFace(0.3f, 0.3f, 0.2f);
    IXC_CHECK(IsPlausible(d));
    std::swap(d.lm.leftEye, d.lm.rightEye);
    IXC_CHECK(!IsPlausible(d));
    d = MakeFace(0.3f, 0.3f, 0.2f);
    d.lm.nose.y = d.lm.mouthLeft.y + 0.05f;  // nose below the mouth
    IXC_CHECK(!IsPlausible(d));
    d = MakeFace(0.3f, 0.3f, 0.2f);
    d.lm.leftEye.x = 0.9f;                   // far outside the box
    IXC_CHECK(!IsPlausible(d));
}

IXC_TEST(Face_DerivedGeometry) {
    TrackedFace f;
    const Detection d = MakeFace(0.4f, 0.3f, 0.2f);
    f.box = d.box;
    f.lm = d.lm;
    DeriveGeometry(f);
    IXC_CHECK(Near(f.rollDeg, 0));
    IXC_CHECK(Near(f.yawDeg, 0, 0.5f));
    IXC_CHECK(Near(f.mouthCenter.x, 0.5f));
    IXC_CHECK(f.leftBrow.y + f.leftBrow.h < f.lm.leftEye.y);  // brows above the eyes
    // Tilt the eye line by 45°: roll follows.
    f.lm.rightEye = {f.lm.leftEye.x + 0.05f, f.lm.leftEye.y + 0.05f};
    DeriveGeometry(f);
    IXC_CHECK(Near(f.rollDeg, 45, 0.01f));
    // Nose shifted towards the right eye: turned head, positive yaw.
    f = TrackedFace{};
    f.box = d.box;
    f.lm = d.lm;
    f.lm.nose.x = d.lm.leftEye.x + (d.lm.rightEye.x - d.lm.leftEye.x) * 0.75f;
    DeriveGeometry(f);
    IXC_CHECK(f.yawDeg > 20 && f.yawDeg < 40);
}

IXC_TEST(Face_IoUAndOutputMapping) {
    IXC_CHECK(Near(IoU({0, 0, 1, 1}, {0, 0, 1, 1}), 1));
    IXC_CHECK(Near(IoU({0, 0, 1, 1}, {2, 2, 1, 1}), 0));
    IXC_CHECK(Near(IoU({0, 0, 2, 1}, {1, 0, 2, 1}), 1.0f / 3));
    // 2x zoom on the centre, mirrored.
    const OutputMapping m{0.25f, 0.25f, 0.5f, 0.5f, true};
    const PointF p = MapToOutput(PointF{0.25f, 0.5f}, m);
    IXC_CHECK(Near(p.x, 1) && Near(p.y, 0.5f));
    const RectF r = MapToOutput(RectF{0.25f, 0.25f, 0.1f, 0.1f}, m);
    IXC_CHECK(Near(r.x, 0.8f) && Near(r.w, 0.2f) && Near(r.y, 0));
}

IXC_TEST(Face_TrackerKeepsIdsAndSmoothsJitter) {
    Tracker t;
    Detection d = MakeFace(0.40f, 0.30f, 0.2f);
    t.Update(&d, 1, 0, 1);
    FaceSnapshot s;
    t.Predict(0, s);
    IXC_CHECK_EQ(s.count, 1);
    const int id = s.faces[0].id;
    // Detector noise of ±0.004 around a still face: the output moves much less.
    float maxJump = 0, prevX = s.faces[0].box.x;
    for (int i = 1; i <= 20; ++i) {
        d = MakeFace(0.40f + ((i % 2) ? 0.004f : -0.004f), 0.30f, 0.2f);
        t.Update(&d, 1, i * 200.0, 1);
        t.Predict(i * 200.0, s);
        IXC_CHECK_EQ(s.faces[0].id, id);
        maxJump = std::max(maxJump, std::abs(s.faces[0].box.x - prevX));
        prevX = s.faces[0].box.x;
    }
    IXC_CHECK(maxJump < 0.006f);  // raw jumps are 0.008
    IXC_CHECK(t.Stable());
}

IXC_TEST(Face_TrackerFollowsMotionAndPredicts) {
    Tracker t;
    // Moving right at 0.001 per ms (a quick head move), detections every 125 ms (8 Hz).
    for (int i = 0; i <= 8; ++i) {
        const Detection d = MakeFace(0.2f + 0.125f * i * 0.8f, 0.3f, 0.2f);
        t.Update(&d, 1, i * 125.0, 1);
    }
    FaceSnapshot s;
    t.Predict(8 * 125.0, s);
    const float atDetection = s.faces[0].box.x;
    IXC_CHECK(std::abs(atDetection - (0.2f + 0.8f)) < 0.03f);  // little lag while moving
    t.Predict(8 * 125.0 + 60, s);                                // between detections
    IXC_CHECK(s.faces[0].box.x > atDetection + 0.02f);           // keeps moving
    t.Predict(8 * 125.0 + 5000, s);                              // long gap: bounded
    IXC_CHECK(s.faces[0].box.x < atDetection + 0.2f * 1.01f + 0.01f);
    IXC_CHECK(!t.Stable());
}

IXC_TEST(Face_TrackerDropsLostFacesAndCapsCount) {
    Tracker t;
    const Detection two[2] = {MakeFace(0.1f, 0.3f, 0.2f), MakeFace(0.6f, 0.3f, 0.2f, 0.8f)};
    t.Update(two, 2, 0, 1);
    IXC_CHECK_EQ(t.Count(), 1);  // maxFaces = 1: the best face only
    t.Update(two, 2, 100, 2);
    IXC_CHECK_EQ(t.Count(), 2);
    FaceSnapshot s;
    t.Predict(100, s);
    IXC_CHECK(s.faces[0].id != s.faces[1].id);
    // Face 2 disappears: kept for maxMissed detections with falling confidence, then dropped.
    const float c0 = s.faces[1].confidence;
    const int id2 = s.faces[1].id;
    t.Update(two, 1, 200, 2);
    t.Predict(200, s);
    IXC_CHECK_EQ(s.count, 2);
    IXC_CHECK(s.faces[1].confidence < c0);
    t.Update(two, 1, 300, 2);
    t.Update(two, 1, 400, 2);
    IXC_CHECK_EQ(t.Count(), 1);
    // Comes back: new ID.
    t.Update(two, 2, 500, 2);
    t.Predict(500, s);
    IXC_CHECK_EQ(s.count, 2);
    IXC_CHECK(std::max(s.faces[0].id, s.faces[1].id) > id2);
}

IXC_TEST(Face_TrackerHoldsLandmarksWhenImplausible) {
    Tracker t;
    Detection d = MakeFace(0.4f, 0.3f, 0.2f);
    t.Update(&d, 1, 0, 1);
    d = MakeFace(0.4f, 0.3f, 0.2f);
    d.lm.leftEye = {0.9f, 0.9f};
    d.landmarksPlausible = IsPlausible(d);
    IXC_CHECK(!d.landmarksPlausible);
    t.Update(&d, 1, 100, 1);
    FaceSnapshot s;
    t.Predict(100, s);
    IXC_CHECK(s.faces[0].landmarksValid);          // previous landmarks held
    IXC_CHECK(s.faces[0].lm.leftEye.x < 0.6f);     // not the bad point
}

IXC_TEST(Face_CadenceRatesAndBudget) {
    CadenceConfig c;
    c.budget = 0.10;
    Cadence k(c);
    k.Reset(0);
    for (int i = 0; i < 5; ++i) k.OnDetection(5.0, i * 100.0, 1, 0.25f);  // 5 ms per detection, face found
    IXC_CHECK(Near(static_cast<float>(k.IntervalMs(false, 33.3)), 125));  // moving: 8 Hz
    IXC_CHECK(Near(static_cast<float>(k.IntervalMs(true, 33.3)), 333.333f, 0.01f));  // stable: 3 Hz
    // 25 ms per detection → budget allows 4 Hz: moving is capped.
    for (int i = 0; i < 10; ++i) k.OnDetection(25.0, 1000 + i * 100.0, 1, 0.25f);
    IXC_CHECK(Near(static_cast<float>(k.IntervalMs(false, 33.3)), 250, 10));  // EMA still converging
    IXC_CHECK(k.Size() == InputSize::Medium);
    // No face for a long time → 2 Hz.
    for (int i = 0; i < 5; ++i) k.OnDetection(25.0, 20000 + i * 100.0, 0, 0);
    IXC_CHECK(Near(static_cast<float>(k.IntervalMs(false, 33.3)), 500, 1));
}

IXC_TEST(Face_CadenceDegradesThenGivesUp) {
    Cadence k;  // budget 0.10 → at most 50 ms per detection for 2 Hz
    k.Reset(0);
    for (int i = 0; i < 3; ++i) k.OnDetection(80.0, i * 500.0, 1, 0.3f);
    IXC_CHECK(k.Size() == InputSize::Small);  // 240x135 too slow → 160x90
    IXC_CHECK(!k.TooSlow());
    for (int i = 0; i < 5; ++i) k.OnDetection(30.0, 5000 + i * 500.0, 1, 0.3f);
    IXC_CHECK(k.Size() == InputSize::Small && !k.TooSlow());  // fits now
    for (int i = 0; i < 8; ++i) k.OnDetection(70.0, 9000 + i * 500.0, 1, 0.3f);
    IXC_CHECK(k.TooSlow());  // even 160x90 can't reach 2 Hz within budget
}

IXC_TEST(Face_CadenceUsesLargeInputForSmallFacesOnFastCpus) {
    Cadence k;
    k.Reset(0);
    for (int i = 0; i < 4; ++i) k.OnDetection(4.0, i * 100.0, 1, 0.08f);  // small face, fast CPU
    IXC_CHECK(k.Size() == InputSize::Large);
    for (int i = 0; i < 4; ++i) k.OnDetection(8.0, 1000 + i * 100.0, 1, 0.3f);  // face came closer
    IXC_CHECK(k.Size() == InputSize::Medium);

    Cadence slow;
    slow.Reset(0);
    for (int i = 0; i < 4; ++i) slow.OnDetection(15.0, i * 100.0, 1, 0.08f);  // no-AVX2 class CPU
    IXC_CHECK(slow.Size() == InputSize::Medium);
}

IXC_TEST(Face_CadenceFixedInterval) {
    CadenceConfig c;
    c.fixedIntervalFrames = 10;
    Cadence k(c);
    k.Reset(0);
    k.OnDetection(5.0, 0, 1, 0.3f);
    IXC_CHECK(Near(static_cast<float>(k.IntervalMs(false, 33.3)), 333, 0.5f));
    k.OnDetection(60.0, 400, 1, 0.3f);  // but never beyond the budget
    IXC_CHECK(k.IntervalMs(false, 33.3) >= k.CostMs() / 0.10 - 1e-6);
}

IXC_TEST(Face_DownscaleConvertsColour) {
    // 64x36 NV12, video range, BT.709. Left half mid-grey, right half pure-ish red.
    const int w = 64, h = 36;
    std::vector<std::uint8_t> y(static_cast<size_t>(w) * h), uv(static_cast<size_t>(w) * h / 2);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) y[static_cast<size_t>(r) * w + c] = c < w / 2 ? 126 : 63;
    for (int r = 0; r < h / 2; ++r)
        for (int c = 0; c < w / 2; ++c) {
            uv[static_cast<size_t>(r) * w + 2 * c] = c < w / 4 ? 128 : 102;      // Cb
            uv[static_cast<size_t>(r) * w + 2 * c + 1] = c < w / 4 ? 128 : 240;  // Cr
        }
    ixc::processing::Nv12Planes p{y.data(), uv.data(), w, w, w, h};
    std::vector<std::uint8_t> bgr(16 * 9 * 3);
    IXC_CHECK(DownscaleNv12ToBgr(p, {ixc::processing::YuvMatrix::Bt709, false}, bgr.data(), 16, 9, false));
    const std::uint8_t* grey = &bgr[(4 * 16 + 2) * 3];
    IXC_CHECK(std::abs(grey[0] - grey[2]) <= 1 && std::abs(grey[1] - 128) <= 4);
    const std::uint8_t* red = &bgr[(4 * 16 + 13) * 3];
    IXC_CHECK(red[2] > 200 && red[1] < 40 && red[0] < 40);
    IXC_CHECK(!DownscaleNv12ToBgr(p, {}, bgr.data(), 128, 9));  // upscaling refused
}

IXC_TEST(Face_DetectorOnEmptyImages) {
    if (!DetectorAvailable()) return;
    std::vector<std::uint8_t> img(240 * 135 * 3, 90);
    Detection out[4];
    for (SimdPath p : {SimdPath::Portable, BestSimdPath()}) {
        Detector d(p);
        IXC_CHECK_EQ(d.Detect(img.data(), 240, 135, 240 * 3, 0.5f, out, 4), 0);  // flat grey: no face
        IXC_CHECK_EQ(d.Detect(nullptr, 240, 135, 240 * 3, 0.5f, out, 4), 0);
        IXC_CHECK_EQ(d.Detect(img.data(), 8, 8, 24, 0.5f, out, 4), 0);        // too small
    }
}

IXC_TEST(Face_EngineNeverBlocksTheFrameThread) {
    FaceEngine e;
    EngineConfig cfg;
    cfg.cpuBudget = 0.5;
    if (!e.Start(cfg)) {
        IXC_CHECK(!DetectorAvailable());
        IXC_CHECK(e.Status().state == EngineState::Unavailable);
        return;
    }
    const int w = 1280, h = 720;
    std::vector<std::uint8_t> y(static_cast<size_t>(w) * h, 100), uv(static_cast<size_t>(w) * h / 2, 128);
    ixc::processing::Nv12Planes p{y.data(), uv.data(), w, w, w, h};
    double maxCallMs = 0;
    const double start = FaceEngine::NowMs();
    int frames = 0;
    while (FaceEngine::NowMs() - start < 1500) {
        const double t0 = FaceEngine::NowMs();
        e.OnFrame(p, {}, t0, 33.3);
        maxCallMs = std::max(maxCallMs, FaceEngine::NowMs() - t0);
        ++frames;
        Sleep(5);
    }
    const EngineStatus s = e.Status();
    e.Stop();
    IXC_CHECK_EQ(s.framesSeen, static_cast<std::uint64_t>(frames));
    IXC_CHECK(s.detections > 0);
    IXC_CHECK(s.state == EngineState::Searching);
    IXC_CHECK(maxCallMs < 5.0);  // staging only; never waits for inference
    IXC_CHECK(e.Status().state == EngineState::Off);
    FaceSnapshot snap;
    e.Snapshot(FaceEngine::NowMs(), snap);
    IXC_CHECK_EQ(snap.count, 0);
}

IXC_TEST(Face_DownscaleNormalizesDarkFrames) {
    const int w = 64, h = 36;
    std::vector<std::uint8_t> y(static_cast<size_t>(w) * h, 30), uv(static_cast<size_t>(w) * h / 2, 128);
    ixc::processing::Nv12Planes p{y.data(), uv.data(), w, w, w, h};
    std::vector<std::uint8_t> bgr(16 * 9 * 3);
    float gain = 0;
    IXC_CHECK(DownscaleNv12ToBgr(p, {}, bgr.data(), 16, 9, true, &gain));
    IXC_CHECK_EQ(gain, 4.0f);  // capped
    std::vector<std::uint8_t> raw(bgr.size());
    IXC_CHECK(DownscaleNv12ToBgr(p, {}, raw.data(), 16, 9, false));
    IXC_CHECK(bgr[0] > raw[0] + 40);  // lifted
    std::fill(y.begin(), y.end(), static_cast<std::uint8_t>(150));
    IXC_CHECK(DownscaleNv12ToBgr(p, {}, bgr.data(), 16, 9, true, &gain));
    IXC_CHECK_EQ(gain, 1.0f);  // bright frames are never darkened
}

IXC_TEST(Face_CadenceNeverUsesLargeInputOnLowRam) {
    CadenceConfig c;
    c.allowLarge = false;
    Cadence k(c);
    k.Reset(0);
    for (int i = 0; i < 6; ++i) k.OnDetection(4.0, i * 100.0, 1, 0.08f);  // small face, fast CPU
    IXC_CHECK(k.Size() == InputSize::Medium);
}
