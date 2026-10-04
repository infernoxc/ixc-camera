#include "face/auto_framer.h"
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
