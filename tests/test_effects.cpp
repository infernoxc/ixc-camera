#include "effects/effects.h"
#include "ixc_test.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace ixc;
using namespace ixc::effects;

namespace {
struct Frame {
    int w, h;
    std::vector<std::uint8_t> buf;
    Frame(int w_, int h_, std::uint8_t y = 120, std::uint8_t c = 128) : w(w_), h(h_), buf(static_cast<size_t>(w_) * h_ * 3 / 2, c) {
        std::fill(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(w) * h, y);
    }
    processing::Nv12Frame View() { return {buf.data(), buf.data() + static_cast<size_t>(w) * h, w, w, w, h}; }
    std::uint8_t& Y(int x, int y) { return buf[static_cast<size_t>(y) * w + x]; }
    std::uint8_t& V(int x, int y) { return buf[static_cast<size_t>(w) * h + static_cast<size_t>(y / 2) * w + (x / 2) * 2 + 1]; }
    std::uint8_t& U(int x, int y) { return buf[static_cast<size_t>(w) * h + static_cast<size_t>(y / 2) * w + (x / 2) * 2]; }
};
std::shared_ptr<const EffectConfig> One(const char* id, double strength = 100) {
    return CompileEffects({EffectEntry{id, strength}}, false);
}
face::FaceSnapshot FaceAt(float x, float y, float s) {
    face::FaceSnapshot snap;
    snap.count = 1;
    auto& f = snap.faces[0];
    f.id = 1;
    f.box = {x, y, s, s * 1.2f};
    f.lm.leftEye = {x + s * 0.3f, y + s * 0.4f};
    f.lm.rightEye = {x + s * 0.7f, y + s * 0.4f};
    f.lm.nose = {x + s * 0.5f, y + s * 0.62f};
    f.lm.mouthLeft = {x + s * 0.35f, y + s * 0.85f};
    f.lm.mouthRight = {x + s * 0.65f, y + s * 0.85f};
    f.landmarksValid = true;
    f.confidence = 0.9f;
    return snap;
}
}  // namespace

IXC_TEST(Effects_CatalogIdsAreValidProfileIds) {
    for (const auto& e : Catalog()) {
        IXC_CHECK(IsValidEffectId(e.id));
        IXC_CHECK(Find(e.id) == &e);
    }
    IXC_CHECK(Find("nope") == nullptr);
    IXC_CHECK(!CompileEffects({EffectEntry{"nope", 100}}, false)->Active());      // unknown: ignored
    IXC_CHECK(!CompileEffects({EffectEntry{"color.mono", 0}}, false)->Active());  // strength 0: off
}

IXC_TEST(Effects_ColorGrades) {
    Frame f(64, 36, 120, 150);
    auto mono = One("color.mono");
    EffectRenderer r;
    r.Apply(f.View(), *mono, {});
    IXC_CHECK_EQ(f.V(10, 10), 128);  // no colour left
    Frame g(64, 36, 120, 128);
    r.Apply(g.View(), *One("color.warm", 50), {});
    IXC_CHECK(g.V(10, 10) > 128);    // redder
    IXC_CHECK(!One("color.warm")->needsFaces);
}

IXC_TEST(Effects_BlushToneGradesFrameWarmsFaceAndLeavesLipsAlone) {
    EffectRenderer r;
    auto cfg = One("blush.tone");
    IXC_CHECK(cfg->needsFaces && cfg->grade);
    // Grade alone (no face): rosy chroma scaling applies everywhere, identically.
    Frame plain(320, 180, 120, 140);
    face::FaceSnapshot none;
    r.Apply(plain.View(), *cfg, {&none, {}, false});
    const int gradedV = plain.V(10, 10);
    IXC_CHECK(gradedV > 140);                 // red axis boosted (1.25x around 128)
    IXC_CHECK(plain.V(160, 76) == gradedV);   // no face: no local treatment
    const int gradedU = plain.U(10, 10);
    // With a face: the skin gets its peach warmth (less blue) on top of the grade; the lips get
    // no colour of their own (no lip effect); the background is untouched.
    const face::FaceSnapshot snap = FaceAt(0.4f, 0.2f, 0.25f);
    Frame f(320, 180, 120, 140);
    for (int i = 0; i < 20; ++i) {  // presence fade-in
        Frame g(320, 180, 120, 140);
        r.Apply(g.View(), *cfg, {&snap, {}, false});
        f = g;
    }
    const int faceX = static_cast<int>((0.4f + 0.125f) * 320), faceY = static_cast<int>((0.2f + 0.15f) * 180);
    IXC_CHECK(f.U(faceX, faceY) < gradedU - 3);  // warmth
    const int lipX = faceX, lipY = static_cast<int>((0.2f + 0.2125f) * 180) + 2;
    IXC_CHECK(f.V(lipX, lipY) <= gradedV + 2);   // no lip colour
    IXC_CHECK_EQ(f.V(10, 10), gradedV);
    IXC_CHECK_EQ(f.U(10, 10), gradedU);
    // Face gone: the local treatment fades out, the grade stays.
    for (int i = 0; i < 40; ++i) {
        Frame g(320, 180, 120, 140);
        r.Apply(g.View(), *cfg, {&none, {}, false});
        f = g;
    }
    IXC_CHECK(f.U(faceX, faceY) >= gradedU - 1);
}

IXC_TEST(Effects_BeautySmoothsSkinKeepsEdgesAndBackground) {
    Frame f(320, 180);
    std::srand(7);
    for (auto it = f.buf.begin(); it != f.buf.begin() + 320 * 180; ++it) *it = static_cast<std::uint8_t>(120 + std::rand() % 9 - 4);
    const int ex = 160, ey = 60;  // a strong edge inside the face (an "eye")
    for (int x = ex - 6; x < ex + 6; ++x) f.Y(x, ey) = 30;
    Frame before = f;
    EffectRenderer r;
    const face::FaceSnapshot snap = FaceAt(0.35f, 0.15f, 0.3f);
    auto cfg = One("beauty.basic");
    for (int i = 0; i < 20; ++i) {  // presence fade-in
        Frame g = before;
        r.Apply(g.View(), *cfg, {&snap, {}, false});
        f = g;
    }
    auto noise = [](Frame& fr, int x0, int y0) {
        double s = 0;
        for (int y = y0; y < y0 + 10; ++y)
            for (int x = x0; x < x0 + 10; ++x) s += std::abs(fr.Y(x + 1, y) - fr.Y(x, y));
        return s;
    };
    IXC_CHECK(noise(f, 150, 80) < noise(before, 150, 80) * 0.6);  // cheek texture smoothed
    IXC_CHECK(f.Y(ex, ey) < 50);                                     // eye edge kept
    IXC_CHECK(noise(f, 5, 5) == noise(before, 5, 5));                // background untouched
    const size_t scratch = r.ScratchBytes();
    Frame g = before;
    r.Apply(g.View(), *cfg, {&snap, {}, false});
    IXC_CHECK_EQ(r.ScratchBytes(), scratch);  // no per-frame growth
}

IXC_TEST(Effects_PortraitSoftensBackgroundNotSubject) {
    Frame f(320, 180);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) f.Y(x, y) = static_cast<std::uint8_t>(40 + std::rand() % 160);  // detailed texture
    Frame before = f;
    EffectRenderer r;
    r.Apply(f.View(), *One("portrait.soft"), {nullptr, {}, false});  // no face: centred subject
    IXC_CHECK_EQ(f.Y(160, 112), before.Y(160, 112));                 // centre unchanged
    auto detail = [](Frame& fr) { double s = 0; for (int y = 2; y < 12; ++y) for (int x = 2; x < 12; ++x) s += std::abs(fr.Y(x + 1, y) - fr.Y(x, y)); return s; };
    IXC_CHECK(detail(f) < detail(before) * 0.5);                     // corner blurred
}

IXC_TEST(Effects_FaceCoordinatesFollowZoomAndMirror) {
    // Face at the source's left; mirrored output puts the face warmth on the right half.
    EffectRenderer r;
    const face::FaceSnapshot snap = FaceAt(0.1f, 0.2f, 0.25f);
    auto cfg = One("blush.tone");
    const face::OutputMapping mirror{0, 0, 1, 1, true};
    Frame f(320, 180, 120, 128);
    for (int i = 0; i < 20; ++i) {
        Frame g(320, 180, 120, 128);
        r.Apply(g.View(), *cfg, {&snap, mirror, false});
        f = g;
    }
    int left = 0, right = 0;
    for (int y = 0; y < 180; y += 2)
        for (int x = 0; x < 320; x += 2) (x < 160 ? left : right) += f.U(x, y) < 124;  // warmth lowers U
    IXC_CHECK(right > 0 && left == 0);
}

IXC_TEST(Effects_MasterSwitchDisablesAll) {
    Profile p;
    p.effects = {{"color.mono", 100}, {"blush.tone", 80}};
    IXC_CHECK(CompileEffects(p, false)->Active());
    p.effectsEnabled = false;
    IXC_CHECK(!CompileEffects(p, false)->Active());
    IXC_CHECK(!CompileEffects(p, false)->needsFaces);  // no tracker either
    const ProfileLoadResult r = ProfileFromJson(ProfileToJson(p));
    IXC_CHECK(r.ok && !r.profile.effectsEnabled && r.profile.effects.size() == 2);  // list kept
}
