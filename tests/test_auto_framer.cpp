#include "face/auto_framer.h"
#include "face/cadence.h"
#include "ixc_test.h"

#include <cmath>

using namespace ixc::face;

namespace {
constexpr ViewRect kFull{0, 0, 1, 1};
// Runs n frames at 30 fps with the same face; returns the final view.
ViewRect Run(AutoFramer& f, const ViewRect& base, const RectF* face, int n, double& t) {
    ViewRect v;
    for (int i = 0; i < n; ++i) {
        t += 33.3;
        v = f.Update(base, face, t);
    }
    return v;
}
bool Inside(const ViewRect& v, const ViewRect& b) {
    return v.x >= b.x - 1e-9 && v.y >= b.y - 1e-9 && v.x + v.w <= b.x + b.w + 1e-9 && v.y + v.h <= b.y + b.h + 1e-9;
}
}  // namespace

IXC_TEST(AutoFramer_FramesHeadAndShoulders) {
    AutoFramer f;
    double t = 0;
    const RectF face{0.60f, 0.30f, 0.10f, 0.15f};  // small face, right of centre
    const ViewRect v = Run(f, kFull, &face, 150, t);  // 5 s: settled
    IXC_CHECK(f.Framing());
    IXC_CHECK(std::abs(v.h - 0.15 / AutoFramer::kFaceHeightFraction) < 0.01);  // face = 30% of the view height
    IXC_CHECK(std::abs(v.w / v.h - 1.0) < 1e-6);                               // base aspect kept
    IXC_CHECK(std::abs((v.x + v.w / 2) - 0.65) < 0.01);                        // face centred horizontally
    IXC_CHECK(std::abs((0.375 - v.y) / v.h - AutoFramer::kFaceCentreFromTop) < 0.01);
    IXC_CHECK(Inside(v, kFull));
}

IXC_TEST(AutoFramer_EasesNeverJumps) {
    AutoFramer f;
    double t = 0;
    const RectF face{0.45f, 0.40f, 0.10f, 0.12f};
    ViewRect prev = f.Update(kFull, &face, t);
    IXC_CHECK(prev == kFull);  // first frame shows the base view
    for (int i = 0; i < 90; ++i) {
        t += 33.3;
        const ViewRect v = f.Update(kFull, &face, t);
        IXC_CHECK(std::abs(v.w - prev.w) < 0.05 && std::abs(v.x - prev.x) < 0.05);  // small steps every frame
        prev = v;
    }
    // A stall (e.g. a dropped second of frames) is treated as at most 250 ms of motion.
    const ViewRect before = f.Current();
    const RectF moved{0.05f, 0.40f, 0.10f, 0.12f};
    const ViewRect after = f.Update(kFull, &moved, t + 5000);
    IXC_CHECK(std::abs(after.x - before.x) < 0.2);
}

IXC_TEST(AutoFramer_DeadbandIgnoresSmallMovements) {
    AutoFramer f;
    double t = 0;
    RectF face{0.45f, 0.35f, 0.10f, 0.12f};
    const ViewRect settled = Run(f, kFull, &face, 300, t);  // 10 s: settled exactly
    face.x += 0.01f;  // a small head movement
    face.y += 0.005f;
    const ViewRect v = Run(f, kFull, &face, 60, t);
    IXC_CHECK(v == settled);  // the view doesn't wander
    face.x = 0.70f;           // the person moves well to the side: follow
    const ViewRect moved = Run(f, kFull, &face, 150, t);
    IXC_CHECK(moved.x > settled.x + 0.1);
}

IXC_TEST(AutoFramer_LimitsZoomAndStaysInsideBase) {
    AutoFramer f;
    double t = 0;
    const ViewRect base{0.1, 0.1, 0.8, 0.8};  // the user's own crop/zoom
    const RectF tiny{0.85f, 0.80f, 0.02f, 0.03f};  // far away, near the base edge
    const ViewRect v = Run(f, base, &tiny, 200, t);
    IXC_CHECK(v.h >= base.h / AutoFramer::kMaxZoom - 1e-6);  // never more than 2.5x
    IXC_CHECK(Inside(v, base));
    const RectF huge{0.0f, 0.0f, 0.9f, 0.9f};  // face fills the frame: base view
    const ViewRect w = Run(f, base, &huge, 200, t);
    IXC_CHECK(std::abs(w.w - base.w) < 1e-3 && std::abs(w.h - base.h) < 1e-3);
}

IXC_TEST(AutoFramer_ZoomsOutWhenFaceIsLost) {
    AutoFramer f;
    double t = 0;
    const RectF face{0.45f, 0.35f, 0.10f, 0.12f};
    Run(f, kFull, &face, 150, t);
    IXC_CHECK(f.Framing());
    const ViewRect held = Run(f, kFull, nullptr, 30, t);  // 1 s without a face: hold
    IXC_CHECK(held.w < 0.9);
    const ViewRect out = Run(f, kFull, nullptr, 200, t);  // then ease back to the base view
    IXC_CHECK(out == kFull);
    IXC_CHECK(!f.Framing());
}

IXC_TEST(AutoFramer_BaseChangeRestartsFromUserView) {
    AutoFramer f;
    double t = 0;
    const RectF face{0.45f, 0.35f, 0.10f, 0.12f};
    Run(f, kFull, &face, 150, t);
    const ViewRect base{0.25, 0.25, 0.5, 0.5};  // user zoomed in 2x meanwhile
    t += 33.3;
    const ViewRect v = f.Update(base, &face, t);
    IXC_CHECK(Inside(v, base));
}

IXC_TEST(AutoFramer_LargestFace) {
    FaceSnapshot s;
    IXC_CHECK(LargestFace(s) == nullptr);
    s.count = 2;
    s.faces[0].box = {0.1f, 0.1f, 0.1f, 0.1f};
    s.faces[1].box = {0.5f, 0.5f, 0.2f, 0.2f};
    IXC_CHECK(LargestFace(s) == &s.faces[1].box);
}

// 0.15 framing settings: tightness, speed and max zoom change the framing predictably, and the
// default settings keep the 0.10 behaviour.
IXC_TEST(AutoFramer_SettingsChangeTightnessSpeedAndZoom) {
    const FramerParams def;
    const FramerParams mid = FramerParamsFor(50, 50, 2.5);
    IXC_CHECK(std::abs(mid.faceHeightFraction - def.faceHeightFraction) < 1e-9);
    IXC_CHECK(std::abs(mid.faceCentreFromTop - def.faceCentreFromTop) < 1e-9);
    // Tighter = the face fills more of the view (monotonic over the whole range).
    double prev = 0;
    for (int t = 0; t <= 100; t += 10) {
        const FramerParams p = FramerParamsFor(t, 50, 2.5);
        IXC_CHECK(p.faceHeightFraction > prev);
        prev = p.faceHeightFraction;
    }
    // Faster = shorter time constant (monotonic), and a smaller deadband.
    IXC_CHECK(FramerParamsFor(50, 100, 2.5).timeConstantMs < FramerParamsFor(50, 60, 2.5).timeConstantMs);
    IXC_CHECK(FramerParamsFor(50, 60, 2.5).timeConstantMs < FramerParamsFor(50, 0, 2.5).timeConstantMs);
    IXC_CHECK(FramerParamsFor(50, 100, 2.5).timeConstantMs <= 125);
    IXC_CHECK(FramerParamsFor(50, 100, 2.5).deadband < FramerParamsFor(50, 0, 2.5).deadband);
    IXC_CHECK_EQ(FramerParamsFor(50, 50, 9).maxZoom, 4.0);
    IXC_CHECK_EQ(FramerParamsFor(50, 50, 0).maxZoom, 1.0);

    // Applied: a tight setting gives a smaller view than a wide one, both inside the frame.
    const RectF face{0.45f, 0.30f, 0.06f, 0.10f};
    const ViewRect frame{0.1, 0.1, 0.8, 0.8};
    AutoFramer wide, tight;
    wide.SetParams(FramerParamsFor(0, 100, 4));
    tight.SetParams(FramerParamsFor(100, 100, 4));
    double t1 = 0, t2 = 0;
    const ViewRect vw = Run(wide, frame, &face, 120, t1);
    const ViewRect vt = Run(tight, frame, &face, 120, t2);
    IXC_CHECK(vt.h < vw.h);
    IXC_CHECK(Inside(vw, frame) && Inside(vt, frame));  // never leaves the user's frame
    // Fast framing reaches its target sooner than slow framing.
    AutoFramer fast, slow;
    fast.SetParams(FramerParamsFor(50, 100, 2.5));
    slow.SetParams(FramerParamsFor(50, 0, 2.5));
    double a = 0, b = 0;
    const ViewRect target = Run(fast, kFull, &face, 300, a);
    AutoFramer fast2;
    fast2.SetParams(FramerParamsFor(50, 100, 2.5));
    double c = 0;
    const ViewRect vf = Run(fast2, kFull, &face, 10, c);
    const ViewRect vs = Run(slow, kFull, &face, 10, b);
    IXC_CHECK(std::abs(vf.h - target.h) < std::abs(vs.h - target.h));
    // Reset keeps the settings.
    fast2.Reset();
    IXC_CHECK(fast2.Params().timeConstantMs < 200);
}

IXC_TEST(Cadence_PreciseModeDetectsMoreOftenAndOnLargerInput) {
    CadenceConfig base;
    const CadenceConfig p = PreciseCadence(base);
    IXC_CHECK(p.movingHz >= 2 * base.movingHz);
    IXC_CHECK(p.stableHz >= 3 * base.stableHz);
    IXC_CHECK(p.budget > base.budget);
    Cadence light(base), precise(p);
    light.Reset(0);
    precise.Reset(0);
    // A fast detector (3 ms at 240x135): precise asks for the larger input, light keeps the medium.
    for (int i = 1; i <= 4; ++i) {
        light.OnDetection(3, i * 100.0, 1, 0.3f);
        precise.OnDetection(3, i * 100.0, 1, 0.3f);
    }
    IXC_CHECK(light.Size() == InputSize::Medium);
    IXC_CHECK(precise.Size() == InputSize::Large);
    IXC_CHECK(precise.IntervalMs(false, 33.3) < light.IntervalMs(false, 33.3) / 2);
    // Never above the budget: an expensive detector still slows down.
    Cadence slowCpu(p);
    slowCpu.Reset(0);
    for (int i = 1; i <= 4; ++i) slowCpu.OnDetection(40, i * 100.0, 1, 0.3f);
    IXC_CHECK(slowCpu.IntervalMs(false, 33.3) >= 40 / p.budget - 1e-6);
    // Without the large input allowed (low-RAM PC), precise stays on the medium input.
    CadenceConfig lowRam;
    lowRam.allowLarge = false;
    IXC_CHECK(!PreciseCadence(lowRam).preferLarge);
}
