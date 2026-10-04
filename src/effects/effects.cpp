#include "effects/effects.h"

#include "effects/stickers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ixc::effects {

namespace {

const std::vector<EffectInfo> kCatalog = {
    {"blush.tone", L"Blush Tone", "face", true, false, true, Cost::Low,
     "rosy grade always applies; the face treatment fades out without a face and uses box estimates without landmarks"},
    {"beauty.basic", L"Basic Beauty", "face", true, false, false, Cost::Low, "fades out without a face"},
    {"background.blur", L"Background Blur", "background", false, true, false, Cost::Moderate,
     "fades in with the first person mask (~0.1 s); off if the CPU is too slow for segmentation"},
    {"background.studio", L"Studio Backdrop", "background", false, true, false, Cost::Moderate,
     "fades in with the first person mask (~0.1 s); off if the CPU is too slow for segmentation"},
    {"sticker.shades", L"Shades", "sticker", true, false, true, Cost::Low, "fades out without a face; box estimates without landmarks"},
    {"sticker.hearts", L"Heart Eyes", "sticker", true, false, true, Cost::Low, "fades out without a face; box estimates without landmarks"},
    {"sticker.crown", L"Crown", "sticker", true, false, true, Cost::Low, "fades out without a face; box estimates without landmarks"},
    {"sticker.puppy", L"Puppy", "sticker", true, false, true, Cost::Low, "fades out without a face; box estimates without landmarks"},
    {"portrait.soft", L"Portrait", "portrait", true, false, false, Cost::Moderate, "uses a centred subject when no face is tracked"},
    {"color.warm", L"Warm Glow", "color", false, false, false, Cost::VeryLow, "none needed"},
    {"color.cool", L"Cool Breeze", "color", false, false, false, Cost::VeryLow, "none needed"},
    {"color.mono", L"Mono", "color", false, false, false, Cost::VeryLow, "none needed"},
    {"color.vivid", L"Vivid", "color", false, false, false, Cost::VeryLow, "none needed"},
    {"lighting.soft", L"Soft Light", "lighting", false, false, false, Cost::VeryLow, "none needed"},
};

// ---- Blush Tone parameters (measured; see docs/blush-tone.md) -----------------------------------------
// Global luma curve: shadows deeper, mid-tones lifted, highlights rolled off (video-range levels).
constexpr float kBlushCurveIn[] = {16, 28, 36, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124, 132, 140, 148, 156, 164, 172, 180, 188, 196, 204, 212, 220, 228, 235};
constexpr float kBlushCurveOut[] = {16, 24, 29, 38.7f, 47.1f, 60, 70, 78.3f, 87.9f, 97.2f, 107.3f, 116.8f, 122.3f, 130.5f, 138.2f, 146.8f, 155.4f,
                                    162.1f, 168.7f, 175.5f, 182.3f, 189.9f, 196.8f, 202.8f, 207.2f, 210.7f, 211.7f, 214};
constexpr float kBlushUScale = 0.779f;  // blue-yellow axis damped
constexpr float kBlushVScale = 1.254f;  // red-green axis boosted: the rosy look
constexpr float kBlushVOffset = -2.0f;
struct TintSpec {
    float drop, rx, ry, dY, dU, dV;  // centre offset/radii in eye distances
    int lumaFloor = 60;
};
struct SkinSpec {
    float smoothing, radiusDiv, edge;
};
constexpr SkinSpec kBlushSkin{1.0f, 20, 24};  // softer than Basic Beauty (reference is very smooth)
constexpr TintSpec kBlushFace{0, 0, 0, 2, -10, 1};  // whole-face peach warmth: radii from the face box
constexpr TintSpec kBlushEyes{0.22f, 0.40f, 0.22f, 0, -4, 2};
constexpr TintSpec kBlushNose{0.02f, 0.22f, 0.18f, 0, -3, 2};

float BlushCurve(float v) {
    if (v <= kBlushCurveIn[0]) return v;
    for (size_t i = 1; i < std::size(kBlushCurveIn); ++i) {
        if (v <= kBlushCurveIn[i]) {
            const float t = (v - kBlushCurveIn[i - 1]) / (kBlushCurveIn[i] - kBlushCurveIn[i - 1]);
            return kBlushCurveOut[i - 1] + t * (kBlushCurveOut[i] - kBlushCurveOut[i - 1]);
        }
    }
    return kBlushCurveOut[std::size(kBlushCurveOut) - 1] + (v - 235) * 0.5f;
}

std::uint8_t Clamp8(float v) { return static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); }
float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}


}  // namespace

const std::vector<EffectInfo>& Catalog() { return kCatalog; }

const EffectInfo* Find(std::string_view id) {
    for (const auto& e : kCatalog) {
        if (id == e.id) return &e;
    }
    return nullptr;
}

std::string NextLens(std::string_view current, int dir, const std::vector<EffectEntry>& effects) {
    // Positions 0..n-1 are catalog entries, n is "no lens".
    const int n = static_cast<int>(kCatalog.size());
    int pos = n;
    for (int i = 0; i < n; ++i) {
        if (current == kCatalog[static_cast<size_t>(i)].id) pos = i;
    }
    const int step = dir < 0 ? n : 1;  // n == -1 modulo n + 1
    for (int k = 0; k <= n; ++k) {
        pos = (pos + step) % (n + 1);
        if (pos == n) return {};
        const std::string_view id = kCatalog[static_cast<size_t>(pos)].id;
        const bool userOwned = id != current && std::any_of(effects.begin(), effects.end(), [&](const EffectEntry& e) { return e.id == id; });
        if (!userOwned) return std::string(id);
    }
    return {};
}

std::shared_ptr<const EffectConfig> CompileEffects(const std::vector<EffectEntry>& effects, bool fullRange) {
    auto cfg = std::make_shared<EffectConfig>();
    for (int i = 0; i < 256; ++i) {
        cfg->yLut[static_cast<size_t>(i)] = cfg->uLut[static_cast<size_t>(i)] = cfg->vLut[static_cast<size_t>(i)] =
            static_cast<std::uint8_t>(i);
    }
    const float lo = fullRange ? 0.0f : 16.0f, hi = fullRange ? 255.0f : 235.0f;
    auto mapY = [&](auto&& f) {  // compose a tone curve t → f(t) (normalized) onto the Y LUT
        for (auto& v : cfg->yLut) {
            const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
            v = Clamp8(lo + f(t) * (hi - lo));
        }
    };
    auto mapC = [&](std::array<std::uint8_t, 256>& lut, auto&& f) {
        for (auto& v : lut) v = Clamp8(std::clamp(f(static_cast<float>(v)), lo, fullRange ? 255.0f : 240.0f));
    };
    // Background first, cheeks last.
    constexpr Kind order[5] = {Kind::BackgroundStudio, Kind::BackgroundBlur, Kind::Portrait, Kind::Beauty, Kind::Blush};
    float faceStrength[5] = {0, 0, 0, 0, 0};

    for (const EffectEntry& e : effects) {
        const float s = static_cast<float>(std::clamp(e.strength, 0.0, 100.0) / 100.0);
        if (s <= 0 || !Find(e.id)) continue;
        const std::string_view id = e.id;
        if (id == "background.studio") faceStrength[0] = s;
        else if (id == "background.blur") faceStrength[1] = s;
        else if (id == "portrait.soft") faceStrength[2] = s;
        else if (id == "beauty.basic") faceStrength[3] = s;
        else if (id.starts_with("sticker.")) continue;  // added below, in drawing order
        else if (id == "blush.tone") {
            faceStrength[4] = s;
            // Global grade of the look (measured curve, defined on video-range levels).
            cfg->grade = true;
            for (auto& v : cfg->yLut) {
                const float video = fullRange ? 16 + v * (219.0f / 255) : static_cast<float>(v);
                float out = video + (BlushCurve(video) - video) * s;
                if (fullRange) out = (out - 16) * (255.0f / 219);
                v = Clamp8(out);
            }
            mapC(cfg->uLut, [&](float u) { return 128 + (u - 128) * (1 + (kBlushUScale - 1) * s); });
            mapC(cfg->vLut, [&](float v) { return 128 + (v - 128) * (1 + (kBlushVScale - 1) * s) + kBlushVOffset * s; });
        }
        else {
            cfg->grade = true;
            if (id == "color.warm") {
                mapC(cfg->uLut, [&](float u) { return u - 14 * s; });
                mapC(cfg->vLut, [&](float v) { return v + 12 * s; });
            } else if (id == "color.cool") {
                mapC(cfg->uLut, [&](float u) { return u + 14 * s; });
                mapC(cfg->vLut, [&](float v) { return v - 10 * s; });
            } else if (id == "color.mono") {
                mapC(cfg->uLut, [&](float u) { return 128 + (u - 128) * (1 - s); });
                mapC(cfg->vLut, [&](float v) { return 128 + (v - 128) * (1 - s); });
            } else if (id == "color.vivid") {
                mapC(cfg->uLut, [&](float u) { return 128 + (u - 128) * (1 + 0.5f * s); });
                mapC(cfg->vLut, [&](float v) { return 128 + (v - 128) * (1 + 0.5f * s); });
                mapY([&](float t) { return t + (t * t * (3 - 2 * t) - t) * 0.6f * s; });
            } else if (id == "lighting.soft") {
                mapY([&](float t) { return t + (std::pow(t, 0.75f) - t) * s - (t > 0.9f ? (t - 0.9f) * 0.3f * s : 0); });
            }
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (faceStrength[i] > 0) cfg->faceEffects.push_back({order[i], faceStrength[i]});
    }
    // Stickers last, on top of everything: head pieces first, then eyewear.
    constexpr struct {
        const char* id;
        stickers::Id sticker;
    } kStickers[] = {{"sticker.puppy", stickers::Id::Puppy},
                     {"sticker.crown", stickers::Id::Crown},
                     {"sticker.shades", stickers::Id::Shades},
                     {"sticker.hearts", stickers::Id::HeartEyes}};
    for (const auto& k : kStickers) {
        for (const EffectEntry& e : effects) {
            const float st = static_cast<float>(std::clamp(e.strength, 0.0, 100.0) / 100.0);
            if (e.id == k.id && st > 0) cfg->faceEffects.push_back({Kind::Sticker, st, static_cast<int>(k.sticker)});
        }
    }
    for (const auto& e : cfg->faceEffects) {
        const bool background = e.kind == Kind::BackgroundBlur || e.kind == Kind::BackgroundStudio;
        cfg->needsSegmentation |= background;
        cfg->needsFaces |= !background;
    }
    return cfg;
}

// ---- rendering --------------------------------------------------------------------------------------

void EffectRenderer::UpdateFace(const processing::Nv12Frame& f, const FrameContext& ctx) {
    const face::TrackedFace* best = nullptr;
    if (ctx.faces) {
        for (int i = 0; i < ctx.faces->count; ++i) {
            const auto& c = ctx.faces->faces[static_cast<size_t>(i)];
            if (!best || c.box.w > best->box.w) best = &c;
        }
    }
    presence_ += ((best ? 1.0f : 0.0f) - presence_) * 0.2f;  // ~5 frames to appear / fade
    if (!best) return;

    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    auto px = [&](face::PointF p) {
        const face::PointF o = face::MapToOutput(p, ctx.map);
        return face::PointF{o.x * W, o.y * H};
    };
    const face::RectF b = face::MapToOutput(best->box, ctx.map);
    face_.x = b.x * W;
    face_.y = b.y * H;
    face_.w = b.w * W;
    face_.h = b.h * H;

    face::PointF cl, cr;
    float eyeDist;
    const face::Landmarks& l = best->lm;
    if (best->landmarksValid) {
        const float ex = l.rightEye.x - l.leftEye.x, ey = l.rightEye.y - l.leftEye.y;
        const float d = std::sqrt(ex * ex + ey * ey);
        cl = {l.leftEye.x + (l.mouthLeft.x - l.leftEye.x) * 0.55f - 0.15f * d, l.leftEye.y + (l.mouthLeft.y - l.leftEye.y) * 0.55f};
        cr = {l.rightEye.x + (l.mouthRight.x - l.rightEye.x) * 0.55f + 0.15f * d, l.rightEye.y + (l.mouthRight.y - l.rightEye.y) * 0.55f};
        const face::PointF a = px(l.leftEye), c = px(l.rightEye);
        eyeDist = std::hypot(c.x - a.x, c.y - a.y);
    } else {  // box-only estimate
        const face::RectF& s = best->box;
        cl = {s.x + 0.25f * s.w, s.y + 0.64f * s.h};
        cr = {s.x + 0.75f * s.w, s.y + 0.64f * s.h};
        eyeDist = 0.4f * face_.w;
    }
    face_.cheekL = px(cl);
    face_.cheekR = px(cr);
    if (best->landmarksValid) {
        face_.eyeL = px(l.leftEye);
        face_.eyeR = px(l.rightEye);
        face_.nose = px(l.nose);
    } else {  // typical frontal proportions within the box
        const face::RectF& s = best->box;
        face_.eyeL = px({s.x + 0.32f * s.w, s.y + 0.40f * s.h});
        face_.eyeR = px({s.x + 0.68f * s.w, s.y + 0.40f * s.h});
        face_.nose = px({s.x + 0.50f * s.w, s.y + 0.60f * s.h});
    }
    face_.eyeDist = eyeDist;
    face_.cheekRx = 0.34f * eyeDist;
    face_.cheekRy = 0.24f * eyeDist;
    face_.valid = face_.w > 8 && face_.h > 8;
}

// Soft elliptical tint (luma delta dY, chroma deltas dU/dV) with a (1-d)^2 falloff. lumaFloor > 0
// fades the tint out on dark pixels (beard, nostrils, brows) so only skin tones change.
void EffectRenderer::Tint(const processing::Nv12Frame& f, face::PointF c, float rx, float ry, float dY, float dU, float dV, float a,
                          int lumaFloor) {
    if (rx < 1 || ry < 1 || a < 0.005f) return;
    auto gate = [&](int luma) { return lumaFloor > 0 ? Smoothstep(static_cast<float>(lumaFloor), static_cast<float>(lumaFloor + 35), static_cast<float>(luma)) : 1.0f; };
    const int x0 = std::max(0, static_cast<int>(c.x - rx)), x1 = std::min(f.width - 1, static_cast<int>(c.x + rx));
    const int y0 = std::max(0, static_cast<int>(c.y - ry)), y1 = std::min(f.height - 1, static_cast<int>(c.y + ry));
    if (dU != 0 || dV != 0) {
        for (int y = y0 / 2; y <= y1 / 2; ++y) {
            std::uint8_t* row = f.uv + static_cast<std::ptrdiff_t>(y) * f.uvStride;
            const std::uint8_t* lumaRow = f.y + static_cast<std::ptrdiff_t>(y * 2) * f.yStride;
            const float dy = (static_cast<float>(y * 2) - c.y) / ry;
            for (int x = x0 / 2; x <= x1 / 2; ++x) {
                const float dx = (static_cast<float>(x * 2) - c.x) / rx, d = dx * dx + dy * dy;
                if (d >= 1) continue;
                const float m = (1 - d) * (1 - d) * a * gate(lumaRow[x * 2]);
                row[2 * x] = Clamp8(row[2 * x] + dU * m);
                row[2 * x + 1] = Clamp8(row[2 * x + 1] + dV * m);
            }
        }
    }
    if (dY != 0) {
        for (int y = y0; y <= y1; ++y) {
            std::uint8_t* row = f.y + static_cast<std::ptrdiff_t>(y) * f.yStride;
            const float dy = (static_cast<float>(y) - c.y) / ry;
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) - c.x) / rx, d = dx * dx + dy * dy;
                if (d < 1) row[x] = Clamp8(row[x] + dY * (1 - d) * (1 - d) * a * gate(row[x]));
            }
        }
    }
}

// "Blush Tone": an independent reimplementation of the look of a reference lens the user supplied,
// matched against lens-on/lens-off frames (docs/blush-tone.md). The global grade comes from
// EffectConfig; this adds the face treatment: soft glowing skin and rosy under-eyes and nose tip.
void EffectRenderer::Blush(const processing::Nv12Frame& f, float a) {
    if (!face_.valid || a * presence_ < 0.01f || face_.eyeDist < 4) return;
    Beauty(f, a * kBlushSkin.smoothing, kBlushSkin.radiusDiv, kBlushSkin.edge);  // applies presence_ itself
    a *= presence_;
    Tint(f, {face_.x + face_.w / 2, face_.y + face_.h * 0.5f}, face_.w * 0.62f, face_.h * 0.72f, kBlushFace.dY, kBlushFace.dU, kBlushFace.dV, a, 60);
    const float ed = face_.eyeDist;
    for (const face::PointF& e : {face_.eyeL, face_.eyeR}) {
        Tint(f, {e.x, e.y + kBlushEyes.drop * ed}, kBlushEyes.rx * ed, kBlushEyes.ry * ed, kBlushEyes.dY, kBlushEyes.dU, kBlushEyes.dV, a, 60);
    }
    Tint(f, {face_.nose.x, face_.nose.y + kBlushNose.drop * ed}, kBlushNose.rx * ed, kBlushNose.ry * ed, kBlushNose.dY, kBlushNose.dU,
         kBlushNose.dV, a, 60);
}

void EffectRenderer::Beauty(const processing::Nv12Frame& f, float a, float radiusDiv, float edge) {
    a *= presence_;
    if (!face_.valid || a < 0.01f) return;
    // Region: the face box widened for cheeks, raised for the forehead.
    const float cx = face_.x + face_.w / 2, cy = face_.y + face_.h * 0.48f;
    const float hw = face_.w * 0.62f, hh = face_.h * 0.68f;
    const int x0 = std::max(0, static_cast<int>(cx - hw)), x1 = std::min(f.width, static_cast<int>(cx + hw));
    const int y0 = std::max(0, static_cast<int>(cy - hh)), y1 = std::min(f.height, static_cast<int>(cy + hh));
    const int rw = x1 - x0, rh = y1 - y0;
    if (rw < 8 || rh < 8) return;
    const int r = std::clamp(static_cast<int>(face_.w / radiusDiv), 2, 14);  // blur radius scales with the face
    const size_t n = static_cast<size_t>(rw) * rh;
    if (tmp_.size() < n) tmp_.resize(n);    // grows only for a bigger face; reused afterwards
    if (blur_.size() < n) blur_.resize(n);

    // Separable box blur of the region (edges clamp to the region).
    for (int y = 0; y < rh; ++y) {
        const std::uint8_t* src = f.y + static_cast<std::ptrdiff_t>(y0 + y) * f.yStride + x0;
        std::uint16_t* dst = tmp_.data() + static_cast<size_t>(y) * rw;
        int sum = 0;
        for (int k = -r; k <= r; ++k) sum += src[std::clamp(k, 0, rw - 1)];
        for (int x = 0; x < rw; ++x) {
            dst[x] = static_cast<std::uint16_t>(sum);
            sum += src[std::min(x + r + 1, rw - 1)] - src[std::max(x - r, 0)];
        }
    }
    const int area = (2 * r + 1) * (2 * r + 1);
    for (int x = 0; x < rw; ++x) {
        int sum = 0;
        for (int k = -r; k <= r; ++k) sum += tmp_[static_cast<size_t>(std::clamp(k, 0, rh - 1)) * rw + x];
        for (int y = 0; y < rh; ++y) {
            blur_[static_cast<size_t>(y) * rw + x] = static_cast<std::uint8_t>((sum + area / 2) / area);
            sum += tmp_[static_cast<size_t>(std::min(y + r + 1, rh - 1)) * rw + x] - tmp_[static_cast<size_t>(std::max(y - r, 0)) * rw + x];
        }
    }
    // Edge-preserving blend inside a soft ellipse: small differences (skin texture) are smoothed,
    // large ones (eyes, brows, mouth, outline) are kept.
    const float kEdge = edge;
    for (int y = 0; y < rh; ++y) {
        std::uint8_t* row = f.y + static_cast<std::ptrdiff_t>(y0 + y) * f.yStride + x0;
        const std::uint8_t* b = blur_.data() + static_cast<size_t>(y) * rw;
        const float dy = (static_cast<float>(y0 + y) - cy) / hh;
        for (int x = 0; x < rw; ++x) {
            const float dx = (static_cast<float>(x0 + x) - cx) / hw, e = dx * dx + dy * dy;
            if (e >= 1) continue;
            const float m = std::min(1.0f, (1 - e) * 3) * a;
            const float diff = static_cast<float>(b[x]) - static_cast<float>(row[x]);
            const float keep = std::max(0.0f, 1 - std::abs(diff) / kEdge);
            row[x] = Clamp8(row[x] + diff * 0.85f * keep * m + 3 * m);  // smooth + a touch of glow
        }
    }
}

// Stickers: anchored to the eyes and nose of the tracked face, fading with its presence. Opacity
// follows the strength slider (fully opaque from 70%).
void EffectRenderer::Sticker(const processing::Nv12Frame& f, int variant, float a, bool fullRange) {
    const float opacity = std::min(1.0f, a / 0.7f) * presence_;
    if (!face_.valid || opacity < 0.01f || face_.eyeDist < 6) return;
    stickers::Anchor anchor;
    const bool swap = face_.eyeL.x > face_.eyeR.x;  // mirrored output: keep the sticker upright
    anchor.eyeLeft = swap ? face_.eyeR : face_.eyeL;
    anchor.eyeRight = swap ? face_.eyeL : face_.eyeR;
    anchor.nose = face_.nose;
    stickers::Draw(f, static_cast<stickers::Id>(variant), anchor, opacity, fullRange);
}

// Low-resolution copy of the frame (1/8 luma, same grid for chroma), 4 samples per block.
void EffectRenderer::BuildLowRes(const processing::Nv12Frame& f) {
    const int lw = std::max(2, f.width / 8), lh = std::max(2, f.height / 8);
    const size_t ln = static_cast<size_t>(lw) * lh;
    if (lowY_.size() < ln) lowY_.resize(ln);
    if (lowUV_.size() < ln * 2) lowUV_.resize(ln * 2);
    for (int by = 0; by < lh; ++by) {
        const std::uint8_t* r0 = f.y + static_cast<std::ptrdiff_t>(std::min(by * 8 + 2, f.height - 1)) * f.yStride;
        const std::uint8_t* r1 = f.y + static_cast<std::ptrdiff_t>(std::min(by * 8 + 6, f.height - 1)) * f.yStride;
        const std::uint8_t* c0 = f.uv + static_cast<std::ptrdiff_t>(std::min(by * 4 + 1, f.height / 2 - 1)) * f.uvStride;
        const std::uint8_t* c1 = f.uv + static_cast<std::ptrdiff_t>(std::min(by * 4 + 3, f.height / 2 - 1)) * f.uvStride;
        for (int bx = 0; bx < lw; ++bx) {
            const int xa = std::min(bx * 8 + 2, f.width - 1), xb = std::min(bx * 8 + 6, f.width - 1);
            lowY_[static_cast<size_t>(by) * lw + bx] = static_cast<std::uint8_t>((r0[xa] + r0[xb] + r1[xa] + r1[xb] + 2) / 4);
            const int ca = std::min(bx * 4 + 1, f.width / 2 - 1) * 2, cb = std::min(bx * 4 + 3, f.width / 2 - 1) * 2;
            for (int k = 0; k < 2; ++k) {
                lowUV_[(static_cast<size_t>(by) * lw + bx) * 2 + k] =
                    static_cast<std::uint8_t>((c0[ca + k] + c0[cb + k] + c1[ca + k] + c1[cb + k] + 2) / 4);
            }
        }
    }
}

void EffectRenderer::Portrait(const processing::Nv12Frame& f, float a) {
    if (a < 0.01f) return;
    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    // Subject ellipse: head and shoulders from the tracked face, else centred.
    float cx = W / 2, cy = H * 0.62f, rx = W * 0.3f, ry = H * 0.62f;
    if (face_.valid && presence_ > 0.01f) {
        const float p = presence_;
        cx += (face_.x + face_.w / 2 - cx) * p;
        cy += (face_.y + face_.h * 1.1f - cy) * p;
        rx += (face_.w * 1.7f - rx) * p;
        ry += (face_.h * 2.3f - ry) * p;
    }
    BuildLowRes(f);
    const int lw = std::max(2, f.width / 8), lh = std::max(2, f.height / 8);
    // Background weight (0..256) as a function of the ellipse distance q in [1, 1.8]: a LUT.
    int weight[256];
    for (int i = 0; i < 256; ++i) weight[i] = static_cast<int>(Smoothstep(0, 1, i / 255.0f) * a * 256 + 0.5f);

    // Per plane and row: a vertically interpolated low-res row is upsampled once (a simple,
    // vectorizable loop), then each row splits into spans: full background (constant weight),
    // the feather band (per-pixel weight from the ellipse distance) and the untouched subject.
    auto plane = [&](std::uint8_t* base, int stride, int width, int height, int channels, float scale, const std::uint8_t* low) {
        const size_t rowLen = static_cast<size_t>(width) * channels;
        if (colDx2_.size() < static_cast<size_t>(width)) colDx2_.resize(static_cast<size_t>(width));
        if (lowRow_.size() < static_cast<size_t>(lw) * channels) lowRow_.resize(static_cast<size_t>(lw) * channels);
        if (upRow_.size() < rowLen) upRow_.resize(rowLen);
        const float pxPerLow = 8.0f / scale;  // plane pixels per low-res cell
        if (colIdx_.size() < static_cast<size_t>(width)) {
            colIdx_.resize(static_cast<size_t>(width));
            colW_.resize(static_cast<size_t>(width));
        }
        for (int x = 0; x < width; ++x) {
            const float dx = (static_cast<float>(x) * scale - cx) / rx;
            colDx2_[static_cast<size_t>(x)] = dx * dx;
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) / pxPerLow - 0.5f, 0.0f, static_cast<float>(lw - 1) - 0.001f);
            colIdx_[static_cast<size_t>(x)] = static_cast<int>(fx) * channels;
            colW_[static_cast<size_t>(x)] = static_cast<int>((fx - static_cast<float>(static_cast<int>(fx))) * 256);
        }
        const int full = weight[255];
        auto blend = [&](std::uint8_t* row, const std::int16_t* up, int x, int w) {
            for (int k = 0; k < channels; ++k) {
                int v = row[x * channels + k];
                v += ((up[x * channels + k] - v) * w) >> 8;
                if (channels == 1) v = (v * (256 - ((41 * w) >> 8))) >> 8;        // soft vignette
                else v = 128 + (((v - 128) * (256 - ((38 * w) >> 8))) >> 8);     // muted colour
                row[x * channels + k] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
            }
        };
        // Constant-weight span: branch-free integer loop the compiler vectorizes. The result can't
        // leave 0..255 (a blend of two 0..255 values, then scaled toward black or grey).
        const int kY = 256 - ((41 * full) >> 8), kC = 256 - ((38 * full) >> 8);
        auto fullSpan = [&](std::uint8_t* row, const std::int16_t* up, int x0, int x1) {
            if (x1 <= x0) return;
            std::uint8_t* r = row + static_cast<size_t>(x0) * channels;
            const std::int16_t* u = up + static_cast<size_t>(x0) * channels;
            const int n = (x1 - x0) * channels;
            if (channels == 1) {
                for (int i = 0; i < n; ++i) {
                    const int v = r[i] + (((u[i] - r[i]) * full) >> 8);
                    r[i] = static_cast<std::uint8_t>((v * kY) >> 8);
                }
            } else {
                for (int i = 0; i < n; ++i) {
                    const int v = r[i] + (((u[i] - r[i]) * full) >> 8);
                    r[i] = static_cast<std::uint8_t>(128 + (((v - 128) * kC) >> 8));
                }
            }
        };
        for (int y = 0; y < height; ++y) {
            const float dy = (static_cast<float>(y) * scale - cy) / ry, dy2 = dy * dy;
            const float fy = std::clamp((static_cast<float>(y) + 0.5f) / pxPerLow - 0.5f, 0.0f, static_cast<float>(lh - 1) - 0.001f);
            const int y0 = static_cast<int>(fy), wy = static_cast<int>((fy - static_cast<float>(y0)) * 256);
            const std::uint8_t* l0 = low + static_cast<size_t>(y0) * lw * channels;
            const std::uint8_t* l1 = l0 + static_cast<size_t>(lw) * channels;
            for (int i = 0; i < lw * channels; ++i) lowRow_[static_cast<size_t>(i)] = static_cast<std::int16_t>((l0[i] * (256 - wy) + l1[i] * wy) >> 8);
            // Horizontal upsample: linear between low-res cell centres.
            std::int16_t* up = upRow_.data();
            const std::int16_t* lr = lowRow_.data();
            for (int x = 0; x < width; ++x) {
                const int i = colIdx_[static_cast<size_t>(x)], xw = colW_[static_cast<size_t>(x)];
                for (int k = 0; k < channels; ++k) up[x * channels + k] = static_cast<std::int16_t>((lr[i + k] * (256 - xw) + lr[i + channels + k] * xw) >> 8);
            }
            std::uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * stride;
            // Span bounds in plane pixels: outer = q 1.8 (full weight beyond), inner = q 1 (subject).
            const float cxp = cx / scale, rxp = rx / scale;
            int outL = width, outR = -1, inL = width, inR = -1;
            if (dy2 < 1.8f) {
                const float h = rxp * std::sqrt(1.8f - dy2);
                outL = std::max(0, static_cast<int>(cxp - h));
                outR = std::min(width - 1, static_cast<int>(cxp + h) + 1);
            }
            if (dy2 < 1.0f) {
                const float h = rxp * std::sqrt(1.0f - dy2);
                inL = static_cast<int>(cxp - h) + 1;
                inR = static_cast<int>(cxp + h) - 1;
            }
            if (outR < 0) {  // whole row is background
                fullSpan(row, up, 0, width);
                continue;
            }
            fullSpan(row, up, 0, outL);
            fullSpan(row, up, outR + 1, width);
            for (int x = outL; x <= outR; ++x) {
                if (x >= inL && x <= inR) {
                    x = inR;
                    continue;
                }
                const float q = colDx2_[static_cast<size_t>(x)] + dy2;
                if (q <= 1) continue;
                const int w = weight[std::min(255, static_cast<int>((q - 1) * (255 / 0.8f)))];
                if (w) blend(row, up, x, w);
            }
        }
    };
    plane(f.y, f.yStride, f.width, f.height, 1, 1.0f, lowY_.data());
    plane(f.uv, f.uvStride, f.width / 2, f.height / 2, 2, 2.0f, lowUV_.data());  // UV pairs: half resolution
}

namespace {

// Separable box blur (edges clamped) of a w x h float plane, in place, using tmp (w*h floats).
void BoxBlur(float* data, int w, int h, int r, float* tmp) {
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* src = data + static_cast<size_t>(y) * w;
        float* dst = tmp + static_cast<size_t>(y) * w;
        float sum = 0;
        for (int k = -r; k <= r; ++k) sum += src[std::clamp(k, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            dst[x] = sum * inv;
            sum += src[std::min(x + r + 1, w - 1)] - src[std::max(x - r, 0)];
        }
    }
    for (int x = 0; x < w; ++x) {
        float sum = 0;
        for (int k = -r; k <= r; ++k) sum += tmp[static_cast<size_t>(std::clamp(k, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            data[static_cast<size_t>(y) * w + x] = sum * inv;
            sum += tmp[static_cast<size_t>(std::min(y + r + 1, h - 1)) * w + x] - tmp[static_cast<size_t>(std::max(y - r, 0)) * w + x];
        }
    }
}

// Person weight (0..1) at a source-normalized position, bilinear on the mask grid.
float MaskAt(const seg::SegMask& m, float sx, float sy) {
    const float mx = std::clamp(sx * seg::kMaskW - 0.5f, 0.0f, static_cast<float>(seg::kMaskW - 1) - 0.001f);
    const float my = std::clamp(sy * seg::kMaskH - 0.5f, 0.0f, static_cast<float>(seg::kMaskH - 1) - 0.001f);
    const int x0 = static_cast<int>(mx), y0 = static_cast<int>(my);
    const float fx = mx - static_cast<float>(x0), fy = my - static_cast<float>(y0);
    const std::uint8_t* r0 = m.value.data() + static_cast<size_t>(y0) * seg::kMaskW + x0;
    const std::uint8_t* r1 = r0 + seg::kMaskW;
    const float top = r0[0] + (r0[1] - r0[0]) * fx, bottom = r1[0] + (r1[1] - r1[0]) * fx;
    return (top + (bottom - top) * fy) * (1.0f / 255.0f);
}

}  // namespace

// Background effects driven by the person mask. The background is built at 1/8 scale (cheap), then
// blended back per pixel with the mask, upsampled bilinearly through the crop/zoom/mirror mapping.
//   Blur: a mask-weighted ("normalized") blur, so the person's colours don't bleed into the blurred
//         background as a halo. Strength sets the blur radius.
//   Studio: a soft, neutral studio gradient replaces the background. Opaque from strength 60%;
//         lower strengths let the real background show through.
void EffectRenderer::Background(const processing::Nv12Frame& f, const FrameContext& ctx, Kind kind, float a) {
    if (!ctx.mask || ctx.mask->generation == 0 || ctx.mask->value.size() != static_cast<size_t>(seg::kMaskW) * seg::kMaskH) return;
    const float mix = (kind == Kind::BackgroundBlur ? 1.0f : std::min(1.0f, 0.4f + a)) * maskPresence_;
    if (mix < 0.01f) return;
    const seg::SegMask& mask = *ctx.mask;
    const face::OutputMapping& map = ctx.map;
    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    auto srcX = [&](float outX) { const float o = outX / W; return map.x + (map.mirror ? 1 - o : o) * map.w; };
    auto srcY = [&](float outY) { return map.y + outY / H * map.h; };

    const int lw = std::max(2, f.width / 8), lh = std::max(2, f.height / 8);
    const size_t ln = static_cast<size_t>(lw) * lh;
    if (lowY_.size() < ln) lowY_.resize(ln);
    if (lowUV_.size() < ln * 2) lowUV_.resize(ln * 2);

    if (kind == Kind::BackgroundBlur) {
        BuildLowRes(f);
        // Planes: background weight, then weight * Y, U, V; plus one plane of scratch.
        if (blurTmp_.size() < ln * 5) blurTmp_.resize(ln * 5);
        float* bw = blurTmp_.data();
        float* by = bw + ln;
        float* bu = by + ln;
        float* bv = bu + ln;
        float* tmp = bv + ln;
        for (int y = 0; y < lh; ++y) {
            const float sy = srcY((static_cast<float>(y) + 0.5f) * 8);
            for (int x = 0; x < lw; ++x) {
                const size_t i = static_cast<size_t>(y) * lw + x;
                const float w = 1.0f - MaskAt(mask, srcX((static_cast<float>(x) + 0.5f) * 8), sy) + 1e-3f;
                bw[i] = w;
                by[i] = w * lowY_[i];
                bu[i] = w * lowUV_[i * 2];
                bv[i] = w * lowUV_[i * 2 + 1];
            }
        }
        const int r = 1 + static_cast<int>(a * 3.0f + 0.5f);  // 1..4 cells: 8..32 px boxes at 1/8 scale
        for (float* plane : {bw, by, bu, bv}) {
            BoxBlur(plane, lw, lh, r, tmp);
            BoxBlur(plane, lw, lh, r, tmp);  // two passes: close to a Gaussian
        }
        for (size_t i = 0; i < ln; ++i) {
            const float inv = 1.0f / bw[i];
            lowY_[i] = Clamp8(by[i] * inv);
            lowUV_[i * 2] = Clamp8(bu[i] * inv);
            lowUV_[i * 2 + 1] = Clamp8(bv[i] * inv);
        }
    } else {
        // Studio backdrop: light grey at the top to a slightly darker, warm grey at the bottom,
        // with a gentle vignette. Defined in video range, converted for full-range frames.
        auto toRangeY = [&](float v) { return ctx.fullRange ? (v - 16) * (255.0f / 219) : v; };
        auto toRangeC = [&](float v) { return ctx.fullRange ? 128 + (v - 128) * (255.0f / 224) : v; };
        for (int y = 0; y < lh; ++y) {
            const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(lh);
            for (int x = 0; x < lw; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(lw) - 0.5f, dy = t - 0.45f;
                const float vignette = 1.0f - 0.35f * (dx * dx + dy * dy);
                const size_t i = static_cast<size_t>(y) * lw + x;
                lowY_[i] = Clamp8(toRangeY(16 + (150 - 58 * t - 16) * vignette));
                lowUV_[i * 2] = Clamp8(toRangeC(126 - 2 * t));
                lowUV_[i * 2 + 1] = Clamp8(toRangeC(130 + 2 * t));
            }
        }
    }

    // Per plane: upsample the low-res background along each row and blend it in where the mask
    // says background. Rows (and spans) that are all person are skipped.
    const int amount = static_cast<int>(mix * 256 + 0.5f);
    auto plane = [&](std::uint8_t* base, int stride, int width, int height, int channels, float scale, const std::uint8_t* low) {
        const float pxPerLow = 8.0f / scale;
        if (colIdx_.size() < static_cast<size_t>(width)) {
            colIdx_.resize(static_cast<size_t>(width));
            colW_.resize(static_cast<size_t>(width));
        }
        if (maskCol_.size() < static_cast<size_t>(width)) {
            maskCol_.resize(static_cast<size_t>(width));
            maskColW_.resize(static_cast<size_t>(width));
        }
        if (maskRow_.size() < static_cast<size_t>(width)) maskRow_.resize(static_cast<size_t>(width));
        if (lowRow_.size() < static_cast<size_t>(lw) * channels) lowRow_.resize(static_cast<size_t>(lw) * channels);
        if (upRow_.size() < static_cast<size_t>(width) * channels) upRow_.resize(static_cast<size_t>(width) * channels);
        for (int x = 0; x < width; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) / pxPerLow - 0.5f, 0.0f, static_cast<float>(lw - 1) - 0.001f);
            colIdx_[static_cast<size_t>(x)] = static_cast<int>(fx) * channels;
            colW_[static_cast<size_t>(x)] = static_cast<int>((fx - static_cast<float>(static_cast<int>(fx))) * 256);
            const float mx = std::clamp(srcX((static_cast<float>(x) + 0.5f) * scale) * seg::kMaskW - 0.5f, 0.0f,
                                        static_cast<float>(seg::kMaskW - 1) - 0.001f);
            maskCol_[static_cast<size_t>(x)] = static_cast<int>(mx);
            maskColW_[static_cast<size_t>(x)] = static_cast<int>((mx - static_cast<float>(static_cast<int>(mx))) * 256);
        }
        for (int y = 0; y < height; ++y) {
            // Mask row: background weight 0..256 per pixel, scaled by the effect amount.
            const float my = std::clamp(srcY((static_cast<float>(y) + 0.5f) * scale) * seg::kMaskH - 0.5f, 0.0f,
                                        static_cast<float>(seg::kMaskH - 1) - 0.001f);
            const int my0 = static_cast<int>(my), wy = static_cast<int>((my - static_cast<float>(my0)) * 256);
            const std::uint8_t* m0 = mask.value.data() + static_cast<size_t>(my0) * seg::kMaskW;
            const std::uint8_t* m1 = m0 + seg::kMaskW;
            bool any = false;
            for (int x = 0; x < width; ++x) {
                const int c = maskCol_[static_cast<size_t>(x)], wx = maskColW_[static_cast<size_t>(x)];
                const int top = m0[c] * (256 - wx) + m0[c + 1] * wx, bottom = m1[c] * (256 - wx) + m1[c + 1] * wx;
                const int person = (top * (256 - wy) + bottom * wy) >> 16;  // 0..255
                const int bg = ((255 - person) * amount) >> 8;              // 0..255
                maskRow_[static_cast<size_t>(x)] = static_cast<std::int16_t>(bg + (bg >> 7));  // 0..256
                any |= bg != 0;
            }
            if (!any) continue;
            const float fy = std::clamp((static_cast<float>(y) + 0.5f) / pxPerLow - 0.5f, 0.0f, static_cast<float>(lh - 1) - 0.001f);
            const int ly = static_cast<int>(fy), lwy = static_cast<int>((fy - static_cast<float>(ly)) * 256);
            const std::uint8_t* l0 = low + static_cast<size_t>(ly) * lw * channels;
            const std::uint8_t* l1 = l0 + static_cast<size_t>(lw) * channels;
            for (int i = 0; i < lw * channels; ++i) lowRow_[static_cast<size_t>(i)] = static_cast<std::int16_t>((l0[i] * (256 - lwy) + l1[i] * lwy) >> 8);
            std::uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * stride;
            const std::int16_t* lr = lowRow_.data();
            for (int x = 0; x < width; ++x) {
                const int w = maskRow_[static_cast<size_t>(x)];
                if (w == 0) continue;
                const int i = colIdx_[static_cast<size_t>(x)], xw = colW_[static_cast<size_t>(x)];
                for (int k = 0; k < channels; ++k) {
                    const int up = (lr[i + k] * (256 - xw) + lr[i + channels + k] * xw) >> 8;
                    int v = row[x * channels + k];
                    v += ((up - v) * w) >> 8;
                    row[x * channels + k] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
                }
            }
        }
    };
    plane(f.y, f.yStride, f.width, f.height, 1, 1.0f, lowY_.data());
    plane(f.uv, f.uvStride, f.width / 2, f.height / 2, 2, 2.0f, lowUV_.data());
}

void EffectRenderer::Grade(const processing::Nv12Frame& f, const EffectConfig& cfg) {
    for (int y = 0; y < f.height; ++y) {
        std::uint8_t* row = f.y + static_cast<std::ptrdiff_t>(y) * f.yStride;
        for (int x = 0; x < f.width; ++x) row[x] = cfg.yLut[row[x]];
    }
    for (int y = 0; y < f.height / 2; ++y) {
        std::uint8_t* row = f.uv + static_cast<std::ptrdiff_t>(y) * f.uvStride;
        for (int x = 0; x < f.width; x += 2) {
            row[x] = cfg.uLut[row[x]];
            row[x + 1] = cfg.vLut[row[x + 1]];
        }
    }
}

void EffectRenderer::Apply(const processing::Nv12Frame& frame, const EffectConfig& cfg, const FrameContext& ctx) {
    if (!frame.y || !frame.uv || frame.width < 16 || frame.height < 16 || !cfg.Active()) return;
    if (cfg.needsFaces) UpdateFace(frame, ctx);
    if (cfg.needsSegmentation) {
        const bool haveMask = ctx.mask && ctx.mask->generation != 0;
        maskPresence_ = haveMask ? maskPresence_ + (1.0f - maskPresence_) * 0.25f : 0.0f;  // fades in over ~8 frames
    }
    for (const auto& e : cfg.faceEffects) {
        switch (e.kind) {
            case Kind::Portrait: Portrait(frame, e.strength); break;
            case Kind::Beauty: Beauty(frame, e.strength); break;
            case Kind::Blush: Blush(frame, e.strength); break;
            case Kind::BackgroundBlur:
            case Kind::BackgroundStudio: Background(frame, ctx, e.kind, e.strength); break;
            case Kind::Sticker: Sticker(frame, e.variant, e.strength, ctx.fullRange); break;
            case Kind::Grade: break;
        }
    }
    if (cfg.grade) Grade(frame, cfg);
}

size_t EffectRenderer::ScratchBytes() const {
    return tmp_.capacity() * sizeof(std::uint16_t) + blur_.capacity() + lowY_.capacity() + lowUV_.capacity() +
           colDx2_.capacity() * sizeof(float) + upRow_.capacity() * 2 + lowRow_.capacity() * 2 + blurTmp_.capacity() * sizeof(float) +
           (maskCol_.capacity() + maskColW_.capacity() + colIdx_.capacity() + colW_.capacity()) * sizeof(int) + maskRow_.capacity() * 2;
}

}  // namespace ixc::effects

namespace ixc::effects {
std::shared_ptr<const EffectConfig> CompileEffects(const Profile& profile, bool fullRange) {
    return CompileEffects(profile.effectsEnabled ? profile.effects : std::vector<EffectEntry>{}, fullRange);
}
}  // namespace ixc::effects
