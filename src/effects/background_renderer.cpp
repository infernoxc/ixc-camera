#include "effects/background_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ixc::effects {

namespace {

std::uint8_t Clamp8(float v) { return static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); }

float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

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
float MaskAt(const std::vector<std::uint8_t>& m, float sx, float sy) {
    const float mx = std::clamp(sx * seg::kMaskW - 0.5f, 0.0f, static_cast<float>(seg::kMaskW - 1) - 0.001f);
    const float my = std::clamp(sy * seg::kMaskH - 0.5f, 0.0f, static_cast<float>(seg::kMaskH - 1) - 0.001f);
    const int x0 = static_cast<int>(mx), y0 = static_cast<int>(my);
    const float fx = mx - static_cast<float>(x0), fy = my - static_cast<float>(y0);
    const std::uint8_t* r0 = m.data() + static_cast<size_t>(y0) * seg::kMaskW + x0;
    const std::uint8_t* r1 = r0 + seg::kMaskW;
    const float top = r0[0] + (r0[1] - r0[0]) * fx, bottom = r1[0] + (r1[1] - r1[0]) * fx;
    return (top + (bottom - top) * fy) * (1.0f / 255.0f);
}

struct Yuv {
    float y, u, v;
};
Yuv RgbToYuv(std::uint32_t rgb, bool fullRange) {
    const float r = static_cast<float>((rgb >> 16) & 0xFF), g = static_cast<float>((rgb >> 8) & 0xFF), b = static_cast<float>(rgb & 0xFF);
    if (fullRange) return {0.2126f * r + 0.7152f * g + 0.0722f * b, 128 - 0.1146f * r - 0.3854f * g + 0.5f * b, 128 + 0.5f * r - 0.4542f * g - 0.0458f * b};
    return {16 + 0.1826f * r + 0.6142f * g + 0.0620f * b, 128 - 0.1006f * r - 0.3386f * g + 0.4392f * b, 128 + 0.4392f * r - 0.3989f * g - 0.0403f * b};
}

// Bilinear sample of a single-channel plane (stride = width * channels), channel c.
float Sample(const std::uint8_t* plane, int w, int h, int channels, int c, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1) - 0.001f);
    y = std::clamp(y, 0.0f, static_cast<float>(h - 1) - 0.001f);
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const std::uint8_t* r0 = plane + (static_cast<size_t>(y0) * w + x0) * channels + c;
    const std::uint8_t* r1 = r0 + static_cast<size_t>(w) * channels;
    const float top = r0[0] + (r0[channels] - r0[0]) * fx, bottom = r1[0] + (r1[channels] - r1[0]) * fx;
    return top + (bottom - top) * fy;
}

}  // namespace

void BackgroundRenderer::BuildMask(const BackgroundContext& ctx, const BackgroundConfig& cfg) {
    const auto& src = ctx.mask->value;
    if (mask_.size() != src.size()) mask_.resize(src.size());  // first use only
    // Edge feather: a contrast curve around the mask's 50% line. Low = crisp cut-out (less
    // background leaking through half-certain edges), high = soft transition.
    if (cfg.feather != lutFeather_) {
        lutFeather_ = cfg.feather;
        const float width = 0.12f + 0.6f * std::clamp(cfg.feather, 0.0f, 1.0f);
        for (int v = 0; v < 256; ++v) featherLut_[v] = Clamp8(255.0f * Smoothstep(0.5f - width / 2, 0.5f + width / 2, static_cast<float>(v) / 255.0f));
    }
    for (size_t k = 0; k < src.size(); ++k) mask_[k] = featherLut_[src[k]];
    const float protect = std::clamp(cfg.protection * 2.0f, 0.0f, 1.0f);  // >= 50%: full guard; below it fades out
    if (!ctx.faces || protect <= 0.0f) return;
    // Face guard: head (with ears and hairline) and neck are always foreground.
    for (int i = 0; i < ctx.faces->count && i < face::kMaxFaces; ++i) {
        const face::TrackedFace& fc = ctx.faces->faces[static_cast<size_t>(i)];
        if (fc.box.w <= 0 || fc.box.h <= 0 || fc.confidence < 0.3f) continue;
        const float cx = (fc.box.x + fc.box.w / 2) * seg::kMaskW, by = fc.box.y * seg::kMaskH;
        const float fw = fc.box.w * seg::kMaskW, fh = fc.box.h * seg::kMaskH;
        struct Ell {
            float cx, cy, rx, ry;
        };
        const Ell shapes[2] = {{cx, by + fh * 0.42f, fw * 0.62f, fh * 0.78f},   // head, ears, hairline
                               {cx, by + fh * 1.10f, fw * 0.34f, fh * 0.40f}};  // neck
        for (const Ell& e : shapes) {
            const int x0 = std::max(0, static_cast<int>(e.cx - e.rx)), x1 = std::min(seg::kMaskW - 1, static_cast<int>(e.cx + e.rx));
            const int y0 = std::max(0, static_cast<int>(e.cy - e.ry)), y1 = std::min(seg::kMaskH - 1, static_cast<int>(e.cy + e.ry));
            for (int y = y0; y <= y1; ++y) {
                const float dy = (static_cast<float>(y) + 0.5f - e.cy) / e.ry;
                std::uint8_t* row = mask_.data() + static_cast<size_t>(y) * seg::kMaskW;
                for (int x = x0; x <= x1; ++x) {
                    const float dx = (static_cast<float>(x) + 0.5f - e.cx) / e.rx;
                    const float v = 255.0f * protect * Smoothstep(1.0f, 0.8f, dx * dx + dy * dy);
                    if (v > static_cast<float>(row[x])) row[x] = static_cast<std::uint8_t>(v);
                }
            }
        }
    }
}

void BackgroundRenderer::BuildBlurSource(const processing::Nv12Frame& f, const BackgroundConfig& cfg, const BackgroundContext& ctx) {
    // Radii from the strength (in output pixels at 1080p, scaled to the frame width). The source
    // scale follows the radius: 1/8 once the far blur spans at least 2 cells there (smooth enough),
    // otherwise 1/4 so light blur keeps its detail.
    const float s = std::clamp(cfg.strength, 0.0f, 1.0f), scaleW = static_cast<float>(f.width) / 1920.0f;
    const float farPx = (6.0f + 90.0f * s) * scaleW, nearPx = farPx * (cfg.bokeh ? 0.3f : 0.5f);
    cell_ = farPx / 8.0f / 1.7f >= 2.0f ? 8 : 4;
    const int cell = cell_;
    lw_ = std::max(2, f.width / cell);
    lh_ = std::max(2, f.height / cell);
    const size_t ln = static_cast<size_t>(lw_) * lh_;
    if (lowY_.size() < ln) lowY_.resize(ln);
    if (lowUV_.size() < ln * 2) lowUV_.resize(ln * 2);
    if (work_.size() < ln * 10) work_.resize(ln * 10);
    // Planes: weighted base (w, wY, wU, wV), near copies (4), scratch, distance.
    float* base[4] = {work_.data(), work_.data() + ln, work_.data() + 2 * ln, work_.data() + 3 * ln};
    float* nearP[4] = {work_.data() + 4 * ln, work_.data() + 5 * ln, work_.data() + 6 * ln, work_.data() + 7 * ln};
    float* tmp = work_.data() + 8 * ln;
    float* dist = work_.data() + 9 * ln;

    const face::OutputMapping& m = ctx.map;
    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    for (int by = 0; by < lh_; ++by) {
        // 4 luma samples and 4 chroma samples per cell.
        const int q = cell / 4, q3 = 3 * cell / 4, cq = cell / 8, cq3 = 3 * cell / 8, half = cell / 2;
        const std::uint8_t* r0 = f.y + static_cast<std::ptrdiff_t>(std::min(by * cell + q, f.height - 1)) * f.yStride;
        const std::uint8_t* r1 = f.y + static_cast<std::ptrdiff_t>(std::min(by * cell + q3, f.height - 1)) * f.yStride;
        const std::uint8_t* c0 = f.uv + static_cast<std::ptrdiff_t>(std::min(by * half + cq, f.height / 2 - 1)) * f.uvStride;
        const std::uint8_t* c1 = f.uv + static_cast<std::ptrdiff_t>(std::min(by * half + cq3, f.height / 2 - 1)) * f.uvStride;
        const float oy = (static_cast<float>(by) + 0.5f) * static_cast<float>(cell) / H;
        const float sy = m.y + oy * m.h;
        for (int bx = 0; bx < lw_; ++bx) {
            const size_t i = static_cast<size_t>(by) * lw_ + bx;
            const int xa = std::min(bx * cell + q, f.width - 1), xb = std::min(bx * cell + q3, f.width - 1);
            const int ca = std::min(bx * half + cq, f.width / 2 - 1) * 2, cb = std::min(bx * half + cq3, f.width / 2 - 1) * 2;
            const float ox = (static_cast<float>(bx) + 0.5f) * static_cast<float>(cell) / W;
            const float sx = m.x + (m.mirror ? 1 - ox : ox) * m.w;
            const float person = MaskAt(mask_, sx, sy);
            const float luma = static_cast<float>(r0[xa] + r0[xb] + r1[xa] + r1[xb]) * 0.25f;
            float w = 1.0f - person + 1e-3f;
            if (cfg.bokeh) {
                // Highlights dominate their neighbourhood, like out-of-focus lights through a lens.
                const float hl = std::max(0.0f, (luma - 160.0f) / 95.0f);
                w *= 1.0f + 5.0f * hl * hl;
            }
            base[0][i] = w;
            base[1][i] = w * luma;
            base[2][i] = w * static_cast<float>(c0[ca] + c0[cb] + c1[ca] + c1[cb]) * 0.25f;
            base[3][i] = w * static_cast<float>(c0[ca + 1] + c0[cb + 1] + c1[ca + 1] + c1[cb + 1]) * 0.25f;
            dist[i] = person > 0.5f ? 0.0f : 1e6f;
        }
    }
    // Distance (in cells) from the person: two-pass chamfer (1 / 1.4).
    for (int y = 0; y < lh_; ++y)
        for (int x = 0; x < lw_; ++x) {
            float& d = dist[static_cast<size_t>(y) * lw_ + x];
            if (x > 0) d = std::min(d, dist[static_cast<size_t>(y) * lw_ + x - 1] + 1.0f);
            if (y > 0) {
                d = std::min(d, dist[static_cast<size_t>(y - 1) * lw_ + x] + 1.0f);
                if (x > 0) d = std::min(d, dist[static_cast<size_t>(y - 1) * lw_ + x - 1] + 1.4f);
                if (x + 1 < lw_) d = std::min(d, dist[static_cast<size_t>(y - 1) * lw_ + x + 1] + 1.4f);
            }
        }
    for (int y = lh_ - 1; y >= 0; --y)
        for (int x = lw_ - 1; x >= 0; --x) {
            float& d = dist[static_cast<size_t>(y) * lw_ + x];
            if (x + 1 < lw_) d = std::min(d, dist[static_cast<size_t>(y) * lw_ + x + 1] + 1.0f);
            if (y + 1 < lh_) {
                d = std::min(d, dist[static_cast<size_t>(y + 1) * lw_ + x] + 1.0f);
                if (x + 1 < lw_) d = std::min(d, dist[static_cast<size_t>(y + 1) * lw_ + x + 1] + 1.4f);
                if (x > 0) d = std::min(d, dist[static_cast<size_t>(y + 1) * lw_ + x - 1] + 1.4f);
            }
        }
    // Three box passes approximate a Gaussian (smooth, no blocky rings). Bokeh keeps the near
    // level sharper so the focus falloff reads as depth.
    const int farR = std::max(1, static_cast<int>(farPx / static_cast<float>(cell) / 1.7f + 0.5f));
    const int nearR = std::max(0, static_cast<int>(nearPx / static_cast<float>(cell) / 1.7f + 0.5f));
    for (int k = 0; k < 4; ++k) {
        std::copy(base[k], base[k] + ln, nearP[k]);
        for (int pass = 0; pass < 3 && nearR > 0; ++pass) BoxBlur(nearP[k], lw_, lh_, nearR, tmp);
        for (int pass = 0; pass < 3; ++pass) BoxBlur(base[k], lw_, lh_, farR, tmp);  // base becomes the far level
    }
    // Depth-like mix: near blur next to the person, far blur further away (focus falloff).
    const float reach = std::max(2.0f, static_cast<float>(lw_) * (0.04f + 0.36f * std::clamp(cfg.falloff, 0.0f, 1.0f)));
    for (size_t i = 0; i < ln; ++i) {
        const float t = Smoothstep(0.0f, reach, dist[i]);
        const float invN = 1.0f / nearP[0][i], invF = 1.0f / base[0][i];
        lowY_[i] = Clamp8(nearP[1][i] * invN + (base[1][i] * invF - nearP[1][i] * invN) * t);
        lowUV_[i * 2] = Clamp8(nearP[2][i] * invN + (base[2][i] * invF - nearP[2][i] * invN) * t);
        lowUV_[i * 2 + 1] = Clamp8(nearP[3][i] * invN + (base[3][i] * invF - nearP[3][i] * invN) * t);
    }
}

void BackgroundRenderer::BuildPlate(const processing::Nv12Frame& f, const BackgroundConfig& cfg, bool fullRange) {
    const BackgroundImage* img = cfg.image.get();
    const float key[3] = {cfg.posX, cfg.posY, cfg.scale};
    if (img == plateSource_ && plateW_ == f.width && plateH_ == f.height && plateFit_ == cfg.fit && plateFull_ == fullRange &&
        std::equal(key, key + 3, platePos_)) {
        return;  // cached
    }
    const size_t ySize = static_cast<size_t>(f.width) * f.height;
    if (plateY_.size() != ySize) {
        plateY_.assign(ySize, 0);
        plateUV_.assign(ySize / 2, 128);
    }
    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    const float iw = static_cast<float>(img->width), ih = static_cast<float>(img->height);
    const float cover = std::max(W / iw, H / ih);
    const float s = (cfg.fit == BackgroundFit::Fill ? cover : std::min(W / iw, H / ih)) * cfg.scale;
    const float dw = iw * s, dh = ih * s;
    const float ox = (W - dw) * cfg.posX, oy = (H - dh) * cfg.posY;
    // Fit leaves borders: fill them with a very soft, darkened cover-scaled version of the picture
    // (sampled from its 1/32 reduction) instead of black bars.
    const int tw = std::max(2, img->width / 32), th = std::max(2, img->height / 32);
    std::vector<std::uint8_t> thumbY, thumbUV;
    const bool borders = dw < W - 0.5f || dh < H - 0.5f;
    if (borders) {
        thumbY.resize(static_cast<size_t>(tw) * th);
        thumbUV.resize(static_cast<size_t>(tw) * th * 2);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const float sx = (static_cast<float>(x) + 0.5f) * iw / static_cast<float>(tw), sy = (static_cast<float>(y) + 0.5f) * ih / static_cast<float>(th);
                thumbY[static_cast<size_t>(y) * tw + x] = Clamp8(0.8f * Sample(img->y.data(), img->width, img->height, 1, 0, sx, sy));
                for (int c = 0; c < 2; ++c)
                    thumbUV[(static_cast<size_t>(y) * tw + x) * 2 + c] = Clamp8(Sample(img->uv.data(), img->width / 2, img->height / 2, 2, c, sx / 2, sy / 2));
            }
    }
    const float coverOx = (W - iw * cover) * 0.5f, coverOy = (H - ih * cover) * 0.5f;
    auto toRangeY = [&](float v) { return fullRange ? (v - 16) * (255.0f / 219) : v; };
    auto toRangeC = [&](float v) { return fullRange ? 128 + (v - 128) * (255.0f / 224) : v; };
    for (int y = 0; y < f.height; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float sy = (py - oy) / s - 0.5f;
        const bool inY = py >= oy && py < oy + dh;
        std::uint8_t* out = plateY_.data() + static_cast<size_t>(y) * f.width;
        for (int x = 0; x < f.width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            float v;
            if (inY && px >= ox && px < ox + dw) {
                v = Sample(img->y.data(), img->width, img->height, 1, 0, (px - ox) / s - 0.5f, sy);
            } else {
                v = Sample(thumbY.data(), tw, th, 1, 0, ((px - coverOx) / cover) * static_cast<float>(tw) / iw - 0.5f,
                           ((py - coverOy) / cover) * static_cast<float>(th) / ih - 0.5f);
            }
            out[x] = Clamp8(toRangeY(v));
        }
    }
    for (int y = 0; y < f.height / 2; ++y) {
        const float py = static_cast<float>(y) * 2 + 1;
        const float sy = ((py - oy) / s) / 2 - 0.5f;
        const bool inY = py >= oy && py < oy + dh;
        std::uint8_t* out = plateUV_.data() + static_cast<size_t>(y) * f.width;
        for (int x = 0; x < f.width / 2; ++x) {
            const float px = static_cast<float>(x) * 2 + 1;
            for (int c = 0; c < 2; ++c) {
                float v;
                if (inY && px >= ox && px < ox + dw) {
                    v = Sample(img->uv.data(), img->width / 2, img->height / 2, 2, c, ((px - ox) / s) / 2 - 0.5f, sy);
                } else {
                    v = Sample(thumbUV.data(), tw, th, 2, c, ((px - coverOx) / cover) * static_cast<float>(tw) / iw - 0.5f,
                               ((py - coverOy) / cover) * static_cast<float>(th) / ih - 0.5f);
                }
                out[x * 2 + c] = Clamp8(toRangeC(v));
            }
        }
    }
    plateSource_ = img;
    plateHold_ = cfg.image;
    plateW_ = f.width;
    plateH_ = f.height;
    plateFit_ = cfg.fit;
    plateFull_ = fullRange;
    std::copy(key, key + 3, platePos_);
}

void BackgroundRenderer::ReleaseBlur() {
    std::vector<std::uint8_t>().swap(lowY_);
    std::vector<std::uint8_t>().swap(lowUV_);
    std::vector<float>().swap(work_);
    std::vector<std::int16_t>().swap(lowRow_);
    lw_ = lh_ = 0;
}

void BackgroundRenderer::ReleasePlate() {
    std::vector<std::uint8_t>().swap(plateY_);
    std::vector<std::uint8_t>().swap(plateUV_);
    plateSource_ = nullptr;
    plateHold_.reset();
    plateW_ = plateH_ = 0;
}

void BackgroundRenderer::Composite(const processing::Nv12Frame& f, const BackgroundConfig& cfg, const BackgroundContext& ctx, float mix) {
    const bool picture = (cfg.mode == BackgroundMode::Replace || cfg.mode == BackgroundMode::Custom) && cfg.image;
    const bool solid = cfg.mode == BackgroundMode::Color;
    const Yuv colour = RgbToYuv(cfg.color, ctx.fullRange);
    const face::OutputMapping& m = ctx.map;
    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    const int amount = static_cast<int>(mix * 256 + 0.5f);

    auto plane = [&](std::uint8_t* base, int stride, int width, int height, int channels, float scale, const std::uint8_t* low,
                     const std::uint8_t* plate) {
        const float pxPerLow = static_cast<float>(cell_) / scale;
        const bool blur = !picture && !solid;  // only the blur reads the 1/8-scale source
        for (auto* v : {&colIdx_, &colW_, &maskCol_, &maskColW_})
            if (v->size() < static_cast<size_t>(width)) v->resize(static_cast<size_t>(width));
        if (rowWeight_.size() < static_cast<size_t>(width)) rowWeight_.resize(static_cast<size_t>(width));
        if (blur && lowRow_.size() < static_cast<size_t>(lw_) * channels) lowRow_.resize(static_cast<size_t>(lw_) * channels);
        for (int x = 0; x < width; ++x) {
            if (blur) {
                const float fx = std::clamp((static_cast<float>(x) + 0.5f) / pxPerLow - 0.5f, 0.0f, static_cast<float>(lw_ - 1) - 0.001f);
                colIdx_[static_cast<size_t>(x)] = static_cast<int>(fx) * channels;
                colW_[static_cast<size_t>(x)] = static_cast<int>((fx - static_cast<float>(static_cast<int>(fx))) * 256);
            }
            const float o = (static_cast<float>(x) + 0.5f) * scale / W;
            const float mx = std::clamp((m.x + (m.mirror ? 1 - o : o) * m.w) * seg::kMaskW - 0.5f, 0.0f, static_cast<float>(seg::kMaskW - 1) - 0.001f);
            maskCol_[static_cast<size_t>(x)] = static_cast<int>(mx);
            maskColW_[static_cast<size_t>(x)] = static_cast<int>((mx - static_cast<float>(static_cast<int>(mx))) * 256);
        }
        const std::uint8_t solidV[2] = {Clamp8(channels == 1 ? colour.y : colour.u), Clamp8(colour.v)};
        for (int y = 0; y < height; ++y) {
            const float my = std::clamp((m.y + (static_cast<float>(y) + 0.5f) * scale / H * m.h) * seg::kMaskH - 0.5f, 0.0f,
                                        static_cast<float>(seg::kMaskH - 1) - 0.001f);
            const int my0 = static_cast<int>(my), wy = static_cast<int>((my - static_cast<float>(my0)) * 256);
            const std::uint8_t* m0 = mask_.data() + static_cast<size_t>(my0) * seg::kMaskW;
            const std::uint8_t* m1 = m0 + seg::kMaskW;
            bool any = false;
            for (int x = 0; x < width; ++x) {
                const int c = maskCol_[static_cast<size_t>(x)], wx = maskColW_[static_cast<size_t>(x)];
                const int top = m0[c] * (256 - wx) + m0[c + 1] * wx, bottom = m1[c] * (256 - wx) + m1[c + 1] * wx;
                const int person = (top * (256 - wy) + bottom * wy) >> 16;  // 0..255
                const int bg = ((255 - person) * amount) >> 8;
                rowWeight_[static_cast<size_t>(x)] = static_cast<std::int16_t>(bg + (bg >> 7));  // 0..256
                any |= bg != 0;
            }
            if (!any) continue;
            std::uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * stride;
            if (picture || solid) {
                const std::uint8_t* src = picture ? plate + static_cast<size_t>(y) * width * channels : nullptr;
                for (int x = 0; x < width; ++x) {
                    const int w = rowWeight_[static_cast<size_t>(x)];
                    if (w == 0) continue;
                    for (int k = 0; k < channels; ++k) {
                        const int target = src ? src[x * channels + k] : solidV[k];
                        int v = row[x * channels + k];
                        v += ((target - v) * w) >> 8;
                        row[x * channels + k] = static_cast<std::uint8_t>(v);
                    }
                }
                continue;
            }
            // Blur: vertically interpolated low-res row, then horizontal interpolation per pixel.
            const float fy = std::clamp((static_cast<float>(y) + 0.5f) / pxPerLow - 0.5f, 0.0f, static_cast<float>(lh_ - 1) - 0.001f);
            const int ly = static_cast<int>(fy), lwy = static_cast<int>((fy - static_cast<float>(ly)) * 256);
            const std::uint8_t* l0 = low + static_cast<size_t>(ly) * lw_ * channels;
            const std::uint8_t* l1 = l0 + static_cast<size_t>(lw_) * channels;
            for (int i = 0; i < lw_ * channels; ++i) lowRow_[static_cast<size_t>(i)] = static_cast<std::int16_t>((l0[i] * (256 - lwy) + l1[i] * lwy) >> 8);
            const std::int16_t* lr = lowRow_.data();
            for (int x = 0; x < width; ++x) {
                const int w = rowWeight_[static_cast<size_t>(x)];
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
    plane(f.y, f.yStride, f.width, f.height, 1, 1.0f, lowY_.data(), plateY_.data());
    plane(f.uv, f.uvStride, f.width / 2, f.height / 2, 2, 2.0f, lowUV_.data(), plateUV_.data());
}

void BackgroundRenderer::Apply(const processing::Nv12Frame& f, const BackgroundConfig& cfg, const BackgroundContext& ctx) {
    const bool haveMask = ctx.mask && ctx.mask->generation != 0 && ctx.mask->value.size() == static_cast<size_t>(seg::kMaskW) * seg::kMaskH;
    presence_ = haveMask ? presence_ + (1.0f - presence_) * 0.25f : 0.0f;  // fades in over ~8 frames
    if (!cfg.Active() || !haveMask || presence_ < 0.01f || !f.y || !f.uv || f.width < 16 || f.height < 16) return;
    BackgroundConfig effective = cfg;
    if (cfg.mode == BackgroundMode::Blur && cfg.strength <= 0.005f) {  // 0%: nothing to do
        ReleaseBlur();
        return;
    }
    BuildMask(ctx, cfg);
    const bool picture = cfg.mode == BackgroundMode::Replace || cfg.mode == BackgroundMode::Custom;
    if (picture && (!cfg.image || cfg.image->width < 16)) effective.mode = BackgroundMode::Blur;  // picture unavailable: blur instead
    const bool usePicture = picture && effective.mode != BackgroundMode::Blur;
    if (effective.mode == BackgroundMode::Blur) BuildBlurSource(f, effective, ctx);
    else ReleaseBlur();                                   // only the blur uses the 1/8-scale buffers
    if (usePicture) BuildPlate(f, effective, ctx.fullRange);
    else if (plateSource_ || !plateY_.empty()) ReleasePlate();  // only pictures use the plate
    Composite(f, effective, ctx, presence_);
}

size_t BackgroundRenderer::ScratchBytes() const {
    return mask_.capacity() + lowY_.capacity() + lowUV_.capacity() + work_.capacity() * sizeof(float) + plateY_.capacity() + plateUV_.capacity() +
           (colIdx_.capacity() + colW_.capacity() + maskCol_.capacity() + maskColW_.capacity()) * sizeof(int) +
           (rowWeight_.capacity() + lowRow_.capacity()) * sizeof(std::int16_t);
}

}  // namespace ixc::effects
