#include "effects/effects.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ixc::effects {

namespace {

const std::vector<EffectInfo> kCatalog = {
    {"blush.tone", L"Blush Tone", "face", true, true, Cost::VeryLow,
     "fades out without a face; cheeks are estimated from the face box when landmarks are unreliable"},
    {"beauty.basic", L"Basic Beauty", "face", true, false, Cost::Low, "fades out without a face"},
    {"portrait.soft", L"Portrait", "portrait", true, false, Cost::Moderate, "uses a centred subject when no face is tracked"},
    {"color.warm", L"Warm Glow", "color", false, false, Cost::VeryLow, "none needed"},
    {"color.cool", L"Cool Breeze", "color", false, false, Cost::VeryLow, "none needed"},
    {"color.mono", L"Mono", "color", false, false, Cost::VeryLow, "none needed"},
    {"color.vivid", L"Vivid", "color", false, false, Cost::VeryLow, "none needed"},
    {"lighting.soft", L"Soft Light", "lighting", false, false, Cost::VeryLow, "none needed"},
};

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
    Kind order[3] = {Kind::Portrait, Kind::Beauty, Kind::Blush};  // background first, cheeks last
    float faceStrength[3] = {0, 0, 0};

    for (const EffectEntry& e : effects) {
        const float s = static_cast<float>(std::clamp(e.strength, 0.0, 100.0) / 100.0);
        if (s <= 0 || !Find(e.id)) continue;
        const std::string_view id = e.id;
        if (id == "portrait.soft") faceStrength[0] = s;
        else if (id == "beauty.basic") faceStrength[1] = s;
        else if (id == "blush.tone") faceStrength[2] = s;
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
    for (int i = 0; i < 3; ++i) {
        if (faceStrength[i] > 0) cfg->faceEffects.push_back({order[i], faceStrength[i]});
    }
    cfg->needsFaces = !cfg->faceEffects.empty();
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
    face_.cheekRx = 0.34f * eyeDist;
    face_.cheekRy = 0.24f * eyeDist;
    face_.valid = face_.w > 8 && face_.h > 8;
}

void EffectRenderer::Blush(const processing::Nv12Frame& f, float a) {
    a *= presence_;
    if (!face_.valid || a < 0.01f || face_.cheekRx < 2) return;
    for (const face::PointF& c : {face_.cheekL, face_.cheekR}) {
        // Chroma (half resolution): a soft rose tint; luma: slightly deeper.
        const float cx = c.x / 2, cy = c.y / 2, rx = face_.cheekRx / 2, ry = face_.cheekRy / 2;
        const int x0 = std::max(0, static_cast<int>(cx - rx)), x1 = std::min(f.width / 2 - 1, static_cast<int>(cx + rx));
        const int y0 = std::max(0, static_cast<int>(cy - ry)), y1 = std::min(f.height / 2 - 1, static_cast<int>(cy + ry));
        for (int y = y0; y <= y1; ++y) {
            std::uint8_t* row = f.uv + static_cast<std::ptrdiff_t>(y) * f.uvStride;
            const float dy = (static_cast<float>(y) - cy) / ry;
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) - cx) / rx, d = dx * dx + dy * dy;
                if (d >= 1) continue;
                const float m = (1 - d) * (1 - d) * a;
                row[2 * x] = Clamp8(row[2 * x] - 8 * m);          // U: a little less blue
                row[2 * x + 1] = Clamp8(row[2 * x + 1] + 26 * m);  // V: more red
            }
        }
        const int Y0 = std::max(0, y0 * 2), Y1 = std::min(f.height - 1, y1 * 2 + 1);
        const int X0 = std::max(0, x0 * 2), X1 = std::min(f.width - 1, x1 * 2 + 1);
        for (int y = Y0; y <= Y1; ++y) {
            std::uint8_t* row = f.y + static_cast<std::ptrdiff_t>(y) * f.yStride;
            const float dy = (static_cast<float>(y) - c.y) / face_.cheekRy;
            for (int x = X0; x <= X1; ++x) {
                const float dx = (static_cast<float>(x) - c.x) / face_.cheekRx, d = dx * dx + dy * dy;
                if (d < 1) row[x] = Clamp8(row[x] - 5 * (1 - d) * (1 - d) * a);
            }
        }
    }
}

void EffectRenderer::Beauty(const processing::Nv12Frame& f, float a) {
    a *= presence_;
    if (!face_.valid || a < 0.01f) return;
    // Region: the face box widened for cheeks, raised for the forehead.
    const float cx = face_.x + face_.w / 2, cy = face_.y + face_.h * 0.48f;
    const float hw = face_.w * 0.62f, hh = face_.h * 0.68f;
    const int x0 = std::max(0, static_cast<int>(cx - hw)), x1 = std::min(f.width, static_cast<int>(cx + hw));
    const int y0 = std::max(0, static_cast<int>(cy - hh)), y1 = std::min(f.height, static_cast<int>(cy + hh));
    const int rw = x1 - x0, rh = y1 - y0;
    if (rw < 8 || rh < 8) return;
    const int r = std::clamp(static_cast<int>(face_.w / 30), 2, 10);  // blur radius scales with the face
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
    constexpr float kEdge = 16;
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
    // Low-resolution background (1/8 luma, same grid for chroma), 4 samples per block.
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
    for (const auto& e : cfg.faceEffects) {
        switch (e.kind) {
            case Kind::Portrait: Portrait(frame, e.strength); break;
            case Kind::Beauty: Beauty(frame, e.strength); break;
            case Kind::Blush: Blush(frame, e.strength); break;
            case Kind::Grade: break;
        }
    }
    if (cfg.grade) Grade(frame, cfg);
}

size_t EffectRenderer::ScratchBytes() const {
    return tmp_.capacity() * sizeof(std::uint16_t) + blur_.capacity() + lowY_.capacity() + lowUV_.capacity() +
           colDx2_.capacity() * sizeof(float) + upRow_.capacity() * 2 + lowRow_.capacity() * 2;
}

}  // namespace ixc::effects

namespace ixc::effects {
std::shared_ptr<const EffectConfig> CompileEffects(const Profile& profile, bool fullRange) {
    return CompileEffects(profile.effectsEnabled ? profile.effects : std::vector<EffectEntry>{}, fullRange);
}
}  // namespace ixc::effects
