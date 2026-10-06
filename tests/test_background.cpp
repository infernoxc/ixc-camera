#include "effects/background_image.h"
#include "effects/background_renderer.h"
#include "effects/backgrounds.h"
#include "effects/effects.h"
#include "ixc_test.h"
#include "profiles/profile.h"
#include "segmentation/mask_refine.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    std::uint8_t& U(int x, int y) { return buf[static_cast<size_t>(w) * h + static_cast<size_t>(y / 2) * w + (x / 2) * 2]; }
    std::uint8_t& V(int x, int y) { return buf[static_cast<size_t>(w) * h + static_cast<size_t>(y / 2) * w + (x / 2) * 2 + 1]; }
};

Frame Textured(int w, int h, unsigned seed = 1) {
    std::srand(seed);
    Frame f(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) f.Y(x, y) = static_cast<std::uint8_t>(40 + std::rand() % 160);
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) f.U(x, y) = static_cast<std::uint8_t>(80 + std::rand() % 96);
    return f;
}

double Detail(Frame& fr, int x0, int y0) {
    double s = 0;
    for (int y = y0; y < y0 + 8; ++y)
        for (int x = x0; x < x0 + 8; ++x) s += std::abs(fr.Y(x + 1, y) - fr.Y(x, y));
    return s;
}

// Person: a centred column (x 0.4..0.6 of the source width), everything else background.
seg::SegMask ColumnMask(float x0 = 0.4f, float x1 = 0.6f) {
    seg::SegMask m;
    m.generation = 1;
    m.value.assign(static_cast<size_t>(seg::kMaskW) * seg::kMaskH, 0);
    for (int y = 0; y < seg::kMaskH; ++y)
        for (int x = static_cast<int>(static_cast<float>(seg::kMaskW) * x0); x < static_cast<int>(static_cast<float>(seg::kMaskW) * x1); ++x)
            m.value[static_cast<size_t>(y) * seg::kMaskW + x] = 255;
    return m;
}

BackgroundConfig Mode(BackgroundMode mode, BlurLevel level = BlurLevel::Medium) {
    BackgroundConfig c;
    c.mode = mode;
    c.blur = level;
    c.strength = level == BlurLevel::Low ? 0.3f : level == BlurLevel::High ? 0.85f : 0.55f;
    return c;
}

// Applies until the fade-in is complete; returns the result.
Frame Render(BackgroundRenderer& r, const Frame& src, const BackgroundConfig& cfg, const BackgroundContext& ctx) {
    Frame out = src;
    for (int i = 0; i < 24; ++i) {
        out = src;
        r.Apply(out.View(), cfg, ctx);
    }
    return out;
}

}  // namespace

// ---- profile -----------------------------------------------------------------------------------

IXC_TEST(Background_ProfileRoundTripAndValidation) {
    Profile p;
    p.background.mode = BackgroundMode::Custom;
    p.background.image = "my-room-2";
    p.background.blur = BlurLevel::High;
    p.background.color = 0x112233;
    p.background.fit = BackgroundFit::Fit;
    p.background.posX = 0.25;
    p.background.posY = 0.75;
    p.background.scale = 1.5;
    const ProfileLoadResult r = ProfileFromJson(ProfileToJson(p));
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.warnings.empty());
    IXC_CHECK(r.profile.background == p.background);

    // Paths and odd names are never accepted as picture names.
    for (const char* bad : {"..\\\\x", "C:/a", "a/b", "UPPER", "", "-x"}) IXC_CHECK(!IsValidBackgroundName(bad));
    const ProfileLoadResult bad = ProfileFromJson(R"({"schemaVersion":1,"background":{"mode":"custom","image":"../etc/passwd","posX":7,"mode2":1}})");
    IXC_REQUIRE(bad.ok);
    IXC_CHECK(bad.profile.background.image.empty());
    IXC_CHECK(bad.profile.background.mode == BackgroundMode::Blur);  // custom without a picture → blur (reported)
    IXC_CHECK_EQ(bad.profile.background.posX, 1.0);
    IXC_CHECK(bad.warnings.size() >= 3);
    const ProfileLoadResult unknown = ProfileFromJson(R"({"schemaVersion":1,"background":{"mode":"sparkles"}})");
    IXC_CHECK(unknown.ok && unknown.profile.background.mode == BackgroundMode::Original && !unknown.warnings.empty());
}

IXC_TEST(Background_MigratesRetiredEffects) {
    const ProfileLoadResult r = ProfileFromJson(
        R"({"schemaVersion":1,"effects":[{"id":"background.blur","strength":90},{"id":"sticker.puppy","strength":70},{"id":"blush.tone","strength":60}]})");
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.profile.background.mode == BackgroundMode::Blur);
    IXC_CHECK(r.profile.background.blur == BlurLevel::High);
    IXC_REQUIRE(r.profile.effects.size() == 1);
    IXC_CHECK_EQ(r.profile.effects[0].id, std::string("blush.tone"));  // kept
    IXC_CHECK(r.warnings.size() == 2);                                // both changes reported
    const ProfileLoadResult studio = ProfileFromJson(R"({"schemaVersion":1,"effects":[{"id":"background.studio","strength":70}]})");
    IXC_CHECK(studio.ok && studio.profile.background.mode == BackgroundMode::Replace && studio.profile.background.builtin == "studio-light");
    // A profile that already has a background keeps it.
    const ProfileLoadResult both = ProfileFromJson(
        R"({"schemaVersion":1,"background":{"mode":"color","color":255},"effects":[{"id":"background.blur","strength":90}]})");
    IXC_CHECK(both.ok && both.profile.background.mode == BackgroundMode::Color && both.profile.effects.empty());
}

IXC_TEST(Background_CompileSeparateFromEffectsMasterSwitch) {
    Profile p;
    p.background.mode = BackgroundMode::Blur;
    p.effectsEnabled = false;
    const auto cfg = CompileEffects(p, false);
    IXC_CHECK(cfg->Active() && cfg->background.Active());
    IXC_CHECK(cfg->needsSegmentation && cfg->needsFaces);  // mask + face guard
    p.background.mode = BackgroundMode::Original;
    const auto off = CompileEffects(p, false);
    IXC_CHECK(!off->Active() && !off->needsSegmentation && !off->needsFaces);
}

// ---- pictures ---------------------------------------------------------------------------------

IXC_TEST(Background_ImageDownscalesAndRoundTrips) {
    // 4000x3000 red-left / blue-right picture → fits within 1920x1080, aspect kept.
    const int w = 4000, h = 3000;
    std::vector<std::uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = rgb.data() + (static_cast<size_t>(y) * w + x) * 3;
            p[0] = x < w / 2 ? 220 : 20;
            p[1] = 30;
            p[2] = x < w / 2 ? 20 : 220;
        }
    const auto img = MakeBackgroundImage(rgb.data(), w, h, w * 3);
    IXC_REQUIRE(img != nullptr);
    IXC_CHECK_EQ(img->height, 1080);
    IXC_CHECK_EQ(img->width, 1440);
    IXC_CHECK(img->uv[1] > 180);                                       // left: red → high V
    IXC_CHECK(img->uv[static_cast<size_t>(img->width) - 2] > 180);      // right: blue → high U

    const auto dir = std::filesystem::temp_directory_path() / "ixc_bg_test";
    std::filesystem::create_directories(dir);
    const auto file = CustomBackgroundFile(dir, "test-pic");
    IXC_REQUIRE(SaveBackgroundImage(file, *img));
    const auto back = LoadBackgroundImage(file);
    IXC_REQUIRE(back != nullptr);
    IXC_CHECK(back->width == img->width && back->height == img->height && back->y == img->y && back->uv == img->uv);

    // Malformed files are rejected: truncated, bad magic, absurd sizes.
    {
        std::ofstream t(dir / "trunc.ixbg", std::ios::binary);
        t << "IXBG";
    }
    IXC_CHECK(LoadBackgroundImage(dir / "trunc.ixbg") == nullptr);
    {
        std::ofstream t(dir / "huge.ixbg", std::ios::binary);
        const unsigned char hdr[16] = {'I', 'X', 'B', 'G', 1, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0};
        t.write(reinterpret_cast<const char*>(hdr), 16);
    }
    IXC_CHECK(LoadBackgroundImage(dir / "huge.ixbg") == nullptr);
    IXC_CHECK(LoadBackgroundImage(dir / "missing.ixbg") == nullptr);
    std::filesystem::remove_all(dir);
}

IXC_TEST(Background_BuiltinsRenderAndSourceCaches) {
    for (const auto& b : BuiltinBackgrounds()) {
        IXC_CHECK(IsValidBackgroundName(b.id));
        const auto img = RenderBuiltinBackground(b.id);
        IXC_CHECK(img && img->width == 640 && img->height == 360);
    }
    IXC_CHECK(RenderBuiltinBackground("no-such") == nullptr);

    BackgroundSource src;
    BackgroundSettings s;
    s.mode = BackgroundMode::Replace;
    s.builtin = "office";
    const auto a = src.Resolve(s, {});
    IXC_CHECK(a != nullptr && src.Resolve(s, {}) == a);  // cached: same picture object
    s.posX = 0.1;
    IXC_CHECK(src.Resolve(s, {}) == a);                   // position doesn't need a new picture
    s.builtin = "nature";
    const auto b = src.Resolve(s, {});
    IXC_CHECK(b != nullptr && b != a);
    s.mode = BackgroundMode::Blur;
    IXC_CHECK(src.Resolve(s, {}) == nullptr);             // released when unused
    s.mode = BackgroundMode::Custom;
    s.image = "not-there";
    IXC_CHECK(src.Resolve(s, std::filesystem::temp_directory_path()) == nullptr);
}

// ---- mask refinement --------------------------------------------------------------------------

IXC_TEST(Background_RefinerSnapsEdgesToTheGuide) {
    seg::MaskRefiner r;
    IXC_REQUIRE(r.Init(seg::kNetW, seg::kNetH, seg::kMaskW, seg::kMaskH));
    // Coarse probabilities: person edge somewhere between x 0.45 and 0.55 (a soft ramp).
    std::vector<float> prob(static_cast<size_t>(seg::kNetW) * seg::kNetH);
    for (int y = 0; y < seg::kNetH; ++y)
        for (int x = 0; x < seg::kNetW; ++x) {
            const float t = (static_cast<float>(x) / seg::kNetW - 0.45f) / 0.1f;
            prob[static_cast<size_t>(y) * seg::kNetW + x] = std::clamp(t, 0.0f, 1.0f);
        }
    // Guide: a sharp image edge exactly at x = 0.5 (dark background, bright person).
    std::vector<std::uint8_t> guide(static_cast<size_t>(seg::kMaskW) * seg::kMaskH), mask(guide.size());
    for (int y = 0; y < seg::kMaskH; ++y)
        for (int x = 0; x < seg::kMaskW; ++x) guide[static_cast<size_t>(y) * seg::kMaskW + x] = x < seg::kMaskW / 2 ? 40 : 200;
    r.Refine(prob.data(), guide.data(), mask.data());
    const int row = seg::kMaskH / 2;
    auto at = [&](int x) { return static_cast<int>(mask[static_cast<size_t>(row) * seg::kMaskW + x]); };
    const int edge = seg::kMaskW / 2;
    // Without the guide the ramp is centred on the edge with no step (128 there, a smooth ~20 px
    // transition). With it, the mask steps sharply exactly at the real image edge and settles
    // within ~10 pixels on both sides.
    IXC_CHECK(at(edge) - at(edge - 1) > 60);
    IXC_CHECK(at(edge - 10) < 10);
    IXC_CHECK(at(edge + 10) > 245);
    IXC_CHECK(at(10) == 0 && at(seg::kMaskW - 10) == 255);
}

IXC_TEST(Background_TemporalSmoothingIsMotionAdaptive) {
    using seg::MaskRefiner;
    IXC_CHECK_EQ(MaskRefiner::Temporal(200, 210), (210 * 3 + 200 * 5 + 4) / 8);  // small change: mostly kept
    IXC_CHECK_EQ(MaskRefiner::Temporal(0, 255), 255);                            // big change: follow at once (no ghost)
    IXC_CHECK(MaskRefiner::Temporal(100, 200) > 175);
    // A flickering edge pixel (±16 around 128) settles instead of jumping.
    int v = 128;
    int maxStep = 0;
    for (int i = 0; i < 20; ++i) {
        const int next = MaskRefiner::Temporal(v, i % 2 ? 144 : 112);
        maxStep = std::max(maxStep, std::abs(next - v));
        v = next;
    }
    IXC_CHECK(maxStep <= 12);
}

// ---- rendering --------------------------------------------------------------------------------

IXC_TEST(Background_BlurKeepsPersonAndBlursDepthLike) {
    const Frame src = Textured(320, 180);
    const seg::SegMask mask = ColumnMask();
    BackgroundRenderer r;
    BackgroundContext ctx;
    ctx.mask = &mask;
    Frame out = Render(r, src, Mode(BackgroundMode::Blur, BlurLevel::High), ctx);
    Frame s = src;
    IXC_CHECK_EQ(out.Y(160, 90), s.Y(160, 90));  // person untouched
    IXC_CHECK_EQ(out.U(160, 90), s.U(160, 90));
    IXC_CHECK(Detail(out, 8, 80) < Detail(s, 8, 80) * 0.4);  // background blurred
    // Depth-like: right next to the person the blur is lighter than far away.
    BackgroundRenderer r2;
    Frame lo = Render(r2, src, Mode(BackgroundMode::Blur, BlurLevel::Low), ctx);
    IXC_CHECK(Detail(lo, 8, 80) > Detail(out, 8, 80));  // Low blurs less than High
    const size_t scratch = r.ScratchBytes();
    Frame again = src;
    r.Apply(again.View(), Mode(BackgroundMode::Blur, BlurLevel::High), ctx);
    IXC_CHECK_EQ(r.ScratchBytes(), scratch);  // no per-frame growth
}

IXC_TEST(Background_NoMaskLeavesFrameAndFadesIn) {
    const Frame src = Textured(320, 180);
    BackgroundRenderer r;
    Frame out = src;
    r.Apply(out.View(), Mode(BackgroundMode::Color), {});
    IXC_CHECK(out.buf == src.buf);
    // The first frame with a mask only partly applies the background (fade-in, no pop).
    const seg::SegMask mask = ColumnMask();
    BackgroundContext ctx;
    ctx.mask = &mask;
    BackgroundConfig c = Mode(BackgroundMode::Color);
    c.color = 0x000000;
    Frame first = src;
    r.Apply(first.View(), c, ctx);
    Frame s = src;
    IXC_CHECK(first.Y(8, 8) > 16 && first.Y(8, 8) < s.Y(8, 8) + 1);
}

IXC_TEST(Background_ColorAndPictureReplaceTheBackground) {
    const Frame src = Textured(320, 180);
    const seg::SegMask mask = ColumnMask();
    BackgroundContext ctx;
    ctx.mask = &mask;
    BackgroundConfig c = Mode(BackgroundMode::Color);
    c.color = 0x00FF00;  // green
    BackgroundRenderer r;
    Frame g = Render(r, src, c, ctx);
    Frame s = src;
    IXC_CHECK(std::abs(g.Y(10, 10) - 173) <= 2 && g.U(10, 10) < 50 && g.V(10, 10) < 50);
    IXC_CHECK_EQ(g.Y(160, 90), s.Y(160, 90));

    // Picture: 2:1 image with a red left half and blue right half, Fill (cover) mode, centred.
    std::vector<std::uint8_t> rgb(400 * 200 * 3);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 400; ++x) {
            std::uint8_t* p = rgb.data() + (static_cast<size_t>(y) * 400 + x) * 3;
            p[0] = x < 200 ? 230 : 10;
            p[2] = x < 200 ? 10 : 230;
        }
    BackgroundConfig pic = Mode(BackgroundMode::Custom);
    pic.image = MakeBackgroundImage(rgb.data(), 400, 200, 1200);
    BackgroundRenderer rp;
    Frame p = Render(rp, src, pic, ctx);
    IXC_CHECK(p.V(10, 10) > 200 && p.U(310, 10) > 200);  // red left, blue right, frame filled (cover)
    IXC_CHECK_EQ(p.Y(160, 90), s.Y(160, 90));           // person untouched
    // Fit mode: the whole picture is visible (letterboxed vertically for 2:1 in 16:9) and the
    // borders are filled with a soft version of it, never black.
    pic.fit = BackgroundFit::Fit;
    BackgroundRenderer rf;
    Frame fit = Render(rf, src, pic, ctx);
    IXC_CHECK(fit.V(10, 2) > 150 && fit.U(310, 177) > 150);  // borders carry the picture's colours, not black
    // Position: posX = 0 shows the image's left edge (red) on the right part of the frame too.
    pic.fit = BackgroundFit::Fill;
    pic.scale = 3.0f;
    pic.posX = 0.0f;
    BackgroundRenderer rs;
    Frame left = Render(rs, src, pic, ctx);
    IXC_CHECK(left.V(300, 10) > 200);
    // A missing picture falls back to blur instead of failing.
    pic.image.reset();
    BackgroundRenderer rb;
    Frame fb = Render(rb, src, pic, ctx);
    IXC_CHECK(Detail(fb, 8, 80) < Detail(s, 8, 80) * 0.6);
}

IXC_TEST(Background_FaceGuardKeepsTheHead) {
    // The mask misses the face completely (e.g. dark side light); the tracked face is protected.
    seg::SegMask mask = ColumnMask(0.0f, 0.0f);
    face::FaceSnapshot faces;
    faces.count = 1;
    faces.faces[0].box = {0.4f, 0.3f, 0.2f, 0.3f};
    faces.faces[0].confidence = 0.9f;
    BackgroundRenderer r;
    BackgroundContext ctx;
    ctx.mask = &mask;
    ctx.faces = &faces;
    const Frame src = Textured(320, 180);
    Frame out = Render(r, src, Mode(BackgroundMode::Color), ctx);
    Frame s = src;
    IXC_CHECK_EQ(out.Y(160, 72), s.Y(160, 72));               // face centre kept
    IXC_CHECK_EQ(out.Y(static_cast<int>(0.41f * 320), 72), s.Y(static_cast<int>(0.41f * 320), 72));  // ear region kept
    IXC_CHECK(out.Y(20, 150) != s.Y(20, 150) || out.Y(21, 150) != s.Y(21, 150));  // background replaced
    const auto& m = r.EffectiveMask();
    IXC_CHECK(m[static_cast<size_t>(seg::kMaskH * 45 / 100) * seg::kMaskW + seg::kMaskW / 2] == 255);
}

IXC_TEST(Background_FollowsMirrorAndZoom) {
    const seg::SegMask mask = ColumnMask(0.0f, 0.33f);  // person in the source's left third
    const Frame src = Textured(320, 180);
    BackgroundContext ctx;
    ctx.mask = &mask;
    ctx.map = {0, 0, 1, 1, true};  // mirrored output: person on the right
    BackgroundRenderer r;
    Frame out = Render(r, src, Mode(BackgroundMode::Blur), ctx);
    Frame s = src;
    IXC_CHECK_EQ(out.Y(300, 90), s.Y(300, 90));
    IXC_CHECK(Detail(out, 20, 80) < Detail(s, 20, 80) * 0.6);
    ctx.map = {0, 0, 0.5f, 1, false};  // zoomed into the left half: left two thirds are person
    BackgroundRenderer z;
    Frame zo = Render(z, src, Mode(BackgroundMode::Blur), ctx);
    IXC_CHECK_EQ(zo.Y(150, 90), s.Y(150, 90));
    IXC_CHECK(Detail(zo, 300, 80) < Detail(s, 300, 80) * 0.6);
}

IXC_TEST(Background_SwitchingModesReleasesMemory) {
    const Frame src = Textured(640, 360);
    const seg::SegMask mask = ColumnMask();
    BackgroundContext ctx;
    ctx.mask = &mask;
    EffectRenderer r;
    Profile p;
    p.background.mode = BackgroundMode::Replace;
    p.background.builtin = "office";
    BackgroundSource source;
    const auto withPicture = CompileEffects(p, false, source.Resolve(p.background, {}));
    effects::FrameContext fctx;
    fctx.mask = &mask;
    Frame f = src;
    r.Apply(f.View(), *withPicture, fctx);
    const size_t pictureBytes = r.ScratchBytes();
    IXC_CHECK(pictureBytes >= static_cast<size_t>(640) * 360 * 3 / 2);  // the plate
    p.background.mode = BackgroundMode::Blur;
    const auto blur = CompileEffects(p, false, source.Resolve(p.background, {}));
    for (int i = 0; i < 3; ++i) {
        f = src;
        r.Apply(f.View(), *blur, fctx);
    }
    IXC_CHECK_EQ(r.BackgroundPlateBytes(), static_cast<size_t>(0));  // plate released (only the blur's buffers now)
    p.background.mode = BackgroundMode::Original;
    const auto none = CompileEffects(p, false, source.Resolve(p.background, {}));
    f = src;
    r.Apply(f.View(), *none, fctx);
    IXC_CHECK_EQ(r.ScratchBytes(), static_cast<size_t>(0));  // everything released
    // Repeated switching doesn't grow memory.
    size_t peak = 0;
    for (int round = 0; round < 20; ++round) {
        p.background.mode = round % 3 == 0 ? BackgroundMode::Replace : round % 3 == 1 ? BackgroundMode::Blur : BackgroundMode::Color;
        const auto c = CompileEffects(p, false, source.Resolve(p.background, {}));
        f = src;
        r.Apply(f.View(), *c, fctx);
        if (round >= 3) IXC_CHECK(r.ScratchBytes() <= peak + 4096);
        peak = std::max(peak, r.ScratchBytes());
    }
}

IXC_TEST(Background_StrengthStyleAndEdgeControls) {
    const Frame src = Textured(320, 180);
    const seg::SegMask mask = ColumnMask();
    BackgroundContext ctx;
    ctx.mask = &mask;
    // 0%: the frame is left exactly as it is.
    {
        BackgroundRenderer r;
        BackgroundConfig c = Mode(BackgroundMode::Blur);
        c.strength = 0;
        IXC_CHECK(Render(r, src, c, ctx).buf == src.buf);
        IXC_CHECK_EQ(r.ScratchBytes() < 300000, true);
    }
    // Continuous strength: more strength, less background detail.
    double prev = 1e9;
    for (float s : {0.2f, 0.5f, 0.8f, 1.0f}) {
        BackgroundRenderer r;
        BackgroundConfig c = Mode(BackgroundMode::Blur);
        c.strength = s;
        Frame o = Render(r, src, c, ctx);
        const double d = Detail(o, 8, 80);
        IXC_CHECK(d <= prev + 1e-9);
        prev = d;
    }
    // Bokeh renders differently from the standard blur (highlight bloom, falloff) and still keeps the person.
    {
        BackgroundRenderer a, b;
        BackgroundConfig c = Mode(BackgroundMode::Blur);
        c.strength = 0.65f;
        const Frame std1 = Render(a, src, c, ctx);
        c.bokeh = true;
        Frame bok = Render(b, src, c, ctx);
        IXC_CHECK(std1.buf != bok.buf);
        Frame s2 = src;
        IXC_CHECK_EQ(bok.Y(160, 90), s2.Y(160, 90));
    }
    // Feather: a crisp setting gives a harder mask edge than a soft one.
    {
        seg::SegMask soft = ColumnMask();
        for (int y = 0; y < seg::kMaskH; ++y)
            for (int x = 0; x < seg::kMaskW; ++x) soft.value[static_cast<size_t>(y) * seg::kMaskW + x] = static_cast<std::uint8_t>(std::clamp(x - 128, 0, 255));
        BackgroundContext sc;
        sc.mask = &soft;
        BackgroundRenderer a, b;
        BackgroundConfig c = Mode(BackgroundMode::Color);
        c.feather = 0.0f;
        Render(a, src, c, sc);
        c.feather = 1.0f;
        Render(b, src, c, sc);
        const size_t at = static_cast<size_t>(10) * seg::kMaskW + 128 + 160;  // mask value 160 (above the 50% line)
        IXC_CHECK(a.EffectiveMask()[at] > b.EffectiveMask()[at]);
    }
}

IXC_TEST(Background_BlurControlsInProfile) {
    Profile p;
    p.background.mode = BackgroundMode::Blur;
    ApplyBlurPreset(p.background, BlurPreset::Dslr);
    IXC_CHECK(p.background.style == BlurStyle::Bokeh && p.background.strength == 65);
    p.background.feather = 20;
    p.background.preset = BlurPreset::Custom;
    const auto r = ProfileFromJson(ProfileToJson(p));
    IXC_REQUIRE(r.ok && r.warnings.empty());
    IXC_CHECK(r.profile.background == p.background);
    // A 0.12 profile ("blur": "high", no strength) keeps its look: strength 85.
    const auto old = ProfileFromJson(R"({"schemaVersion":1,"background":{"mode":"blur","blur":"high"}})");
    IXC_REQUIRE(old.ok);
    IXC_CHECK_EQ(old.profile.background.strength, 85.0);
    // Out of range is clamped with a warning, never silently.
    const auto bad = ProfileFromJson(R"({"schemaVersion":1,"background":{"strength":250}})");
    IXC_CHECK(bad.ok && bad.profile.background.strength == 100 && !bad.warnings.empty());
    // The compiled config carries the controls.
    const auto cfg = CompileEffects(p, false);
    IXC_CHECK(cfg->background.bokeh && std::abs(cfg->background.feather - 0.2f) < 1e-6f && std::abs(cfg->background.strength - 0.65f) < 1e-6f);
}
