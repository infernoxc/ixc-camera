#include "processing/image_pipeline.h"

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ixc::processing {

namespace {

std::uint8_t Clamp8(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

void IdentityLut(std::array<std::uint8_t, 256>& t) {
    for (int i = 0; i < 256; ++i) t[static_cast<size_t>(i)] = static_cast<std::uint8_t>(i);
}

// Tone curve on normalized luma. Order: exposure, brightness, contrast, shadows/highlights,
// low-light lift, gamma. Weights keep black at black (lift, don't wash out).
double Tone(double t, const ImageSettings& im) {
    t *= std::exp2(im.exposureEv);
    t += im.brightness / 100.0 * 0.25;
    t = (t - 0.5) * std::exp2(im.contrast / 100.0) + 0.5;
    const double c = std::clamp(t, 0.0, 1.0);
    const double shadowW = 6.75 * c * (1 - c) * (1 - c);   // peaks at 1/3
    const double highlightW = 6.75 * c * c * (1 - c);      // peaks at 2/3
    t += im.shadows / 100.0 * 0.15 * shadowW + im.highlights / 100.0 * 0.15 * highlightW;
    t += im.lowLight / 100.0 * 0.20 * shadowW;
    t = std::clamp(t, 0.0, 1.0);
    if (im.gamma != 1.0) t = std::pow(t, 1.0 / im.gamma);
    return t;
}

// Output-aspect rectangle inside the crop, then shrunk by the zoom factor around its centre.
void FitGeometry(const Profile& p, std::uint32_t w, std::uint32_t h, PipelineParams& out) {
    const double aspect = static_cast<double>(w) / h;
    double cx = p.crop.x * w, cy = p.crop.y * h, cw = p.crop.width * w, ch = p.crop.height * h;
    if (cw / ch > aspect) {
        const double nw = ch * aspect;
        cx += (cw - nw) / 2;
        cw = nw;
    } else {
        const double nh = cw / aspect;
        cy += (ch - nh) / 2;
        ch = nh;
    }
    const double zoom = std::max(1.0, p.zoom);
    const double zw = cw / zoom, zh = ch / zoom;
    cx += (cw - zw) / 2;
    cy += (ch - zh) / 2;
    out.srcX = cx / w;
    out.srcY = cy / h;
    out.srcW = zw / w;
    out.srcH = zh / h;
    out.geometryIdentity = std::fabs(out.srcX) < 1e-6 && std::fabs(out.srcY) < 1e-6 && std::fabs(out.srcW - 1) < 1e-6 &&
                           std::fabs(out.srcH - 1) < 1e-6;
}

}  // namespace

PipelineParams CompileParams(const Profile& profile, std::uint32_t width, std::uint32_t height, bool fullRange) {
    PipelineParams p;
    const ImageSettings& im = profile.image;

    const bool toneNeutral = im.brightness == 0 && im.contrast == 0 && im.gamma == 1.0 && im.highlights == 0 && im.shadows == 0 &&
                             im.exposureEv == 0 && im.lowLight == 0;
    if (toneNeutral) {
        IdentityLut(p.yLut);
    } else {
        const double lo = fullRange ? 0 : 16, hi = fullRange ? 255 : 235;
        for (int v = 0; v < 256; ++v) {
            const double t = (v - lo) / (hi - lo);
            // Values outside the nominal range (super-black/white) keep their offset from it.
            double outT = Tone(std::clamp(t, 0.0, 1.0), im);
            if (t < 0) outT += t;
            if (t > 1) outT += t - 1;
            p.yLut[static_cast<size_t>(v)] = Clamp8(lo + outT * (hi - lo));
        }
    }

    const bool chromaNeutral = im.saturation == 0 && im.temperature == 0 && im.tint == 0;
    if (chromaNeutral) {
        IdentityLut(p.uLut);
        IdentityLut(p.vLut);
    } else {
        const double s = 1.0 + im.saturation / 100.0;
        // Warm = more red (Cr up) and less blue (Cb down); tint + = magenta (both up).
        const double dCr = im.temperature / 100.0 * 12.0 + im.tint / 100.0 * 8.0;
        const double dCb = -im.temperature / 100.0 * 12.0 + im.tint / 100.0 * 8.0;
        for (int v = 0; v < 256; ++v) {
            p.uLut[static_cast<size_t>(v)] = Clamp8(128 + (v - 128) * s + dCb);
            p.vLut[static_cast<size_t>(v)] = Clamp8(128 + (v - 128) * s + dCr);
        }
    }
    p.lutIdentity = toneNeutral && chromaNeutral;

    p.sharpenAmount = static_cast<int>(std::lround(std::clamp(im.sharpness, 0.0, 100.0) / 100.0 * 1.5 * 256));
    p.mirror = profile.mirror;
    FitGeometry(profile, width, height, p);
    p.identity = p.lutIdentity && p.sharpenAmount == 0 && !p.mirror && p.geometryIdentity;
    p.gpuAllowed = profile.gpu == GpuMode::Auto;
    return p;
}

namespace detail {

// Reference implementation. Unsharp mask with a 3x3 binomial blur, a noise threshold and halo
// clamping. The multiply is floor(detail * amount / 256), matching the SSE2 _mm_mulhi_epi16.
void SharpenRowScalar(const std::uint8_t* a, const std::uint8_t* c, const std::uint8_t* b, std::uint8_t* d, int w, int amount,
                      int threshold, int halo) {
    for (int x = 1; x < w - 1; ++x) {
        const int blur = (a[x - 1] + 2 * a[x] + a[x + 1] + 2 * c[x - 1] + 4 * c[x] + 2 * c[x + 1] + b[x - 1] + 2 * b[x] + b[x + 1] + 8) >> 4;
        const int detail = c[x] - blur;
        if (detail <= threshold && detail >= -threshold) {
            d[x] = c[x];  // flat area / noise: unchanged
            continue;
        }
        // Halo control: no overshoot beyond the cross neighbourhood's range plus a margin.
        int mn = std::min(a[x], b[x]);
        mn = std::min(mn, static_cast<int>(std::min(c[x - 1], c[x + 1])));
        mn = std::min(mn, static_cast<int>(c[x]));
        int mx = std::max(a[x], b[x]);
        mx = std::max(mx, static_cast<int>(std::max(c[x - 1], c[x + 1])));
        mx = std::max(mx, static_cast<int>(c[x]));
        const int delta = (detail * 16 * (amount * 16)) >> 16;  // == floor(detail*amount/256)
        const int v = std::clamp(c[x] + delta, mn - halo, mx + halo);
        d[x] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    }
}

// SSE2 (baseline on every x64 CPU): 8 pixels per iteration in 16-bit lanes; the scalar
// reference handles the tail. Bit-identical to SharpenRowScalar (unit-tested).
void SharpenRowSse2(const std::uint8_t* a, const std::uint8_t* c, const std::uint8_t* b, std::uint8_t* d, int w, int amount,
                    int threshold, int halo) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i eight = _mm_set1_epi16(8);
    const __m128i thr = _mm_set1_epi16(static_cast<short>(threshold));
    const __m128i amt = _mm_set1_epi16(static_cast<short>(amount * 16));  // <= 6144
    const __m128i haloV = _mm_set1_epi16(static_cast<short>(halo));
    auto load8 = [&](const std::uint8_t* p) {
        return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)), zero);
    };

    int x = 1;
    for (; x + 8 <= w - 1; x += 8) {
        const __m128i al = load8(a + x - 1), am = load8(a + x), ar = load8(a + x + 1);
        const __m128i cl = load8(c + x - 1), cm = load8(c + x), cr = load8(c + x + 1);
        const __m128i bl = load8(b + x - 1), bm = load8(b + x), br = load8(b + x + 1);
        // Separable [1 2 1] x [1 2 1]: vertical sums first (max 1020 per lane).
        const __m128i vl = _mm_add_epi16(_mm_add_epi16(al, bl), _mm_slli_epi16(cl, 1));
        const __m128i vm = _mm_add_epi16(_mm_add_epi16(am, bm), _mm_slli_epi16(cm, 1));
        const __m128i vr = _mm_add_epi16(_mm_add_epi16(ar, br), _mm_slli_epi16(cr, 1));
        const __m128i sum = _mm_add_epi16(_mm_add_epi16(vl, vr), _mm_slli_epi16(vm, 1));  // max 4080
        const __m128i blur = _mm_srli_epi16(_mm_add_epi16(sum, eight), 4);
        const __m128i detail = _mm_sub_epi16(cm, blur);

        const __m128i absDetail = _mm_max_epi16(detail, _mm_sub_epi16(zero, detail));
        const __m128i active = _mm_cmpgt_epi16(absDetail, thr);

        const __m128i delta = _mm_mulhi_epi16(_mm_slli_epi16(detail, 4), amt);
        __m128i v = _mm_add_epi16(cm, delta);

        __m128i mn = _mm_min_epi16(_mm_min_epi16(am, bm), _mm_min_epi16(cl, cr));
        mn = _mm_min_epi16(mn, cm);
        __m128i mx = _mm_max_epi16(_mm_max_epi16(am, bm), _mm_max_epi16(cl, cr));
        mx = _mm_max_epi16(mx, cm);
        v = _mm_max_epi16(v, _mm_sub_epi16(mn, haloV));
        v = _mm_min_epi16(v, _mm_add_epi16(mx, haloV));

        const __m128i result = _mm_or_si128(_mm_and_si128(active, v), _mm_andnot_si128(active, cm));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(d + x), _mm_packus_epi16(result, result));  // saturates to 0..255
    }
    if (x < w - 1) {
        // Tail: run the reference on the remaining pixels (it needs x-1 .. x+1 context).
        SharpenRowScalar(a + x - 1, c + x - 1, b + x - 1, d + x - 1, w - 1 - x + 2, amount, threshold, halo);
    }
}

}  // namespace detail

size_t Nv12Processor::ScratchBytes() const {
    return (geo_.yX.capacity() + geo_.yY.capacity() + geo_.uvX.capacity() + geo_.uvY.capacity()) * sizeof(std::int32_t) + rows_.capacity() +
           blend_.capacity() * sizeof(std::uint16_t);
}

bool Nv12Processor::Process(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& p) {
    if (!src.y || !src.uv || !dst.y || !dst.uv) return false;
    if (src.width != dst.width || src.height != dst.height || src.width < 2 || src.height < 2) return false;
    if ((src.width & 1) || (src.height & 1)) return false;
    if (src.yStride < src.width || src.uvStride < src.width || dst.yStride < dst.width || dst.uvStride < dst.width) return false;

    if (p.geometryIdentity) ColorPass(src, dst, p);
    else ScalePass(src, dst, p);
    if (p.sharpenAmount > 0) SharpenPass(dst, p);
    return true;
}

void Nv12Processor::ColorPass(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& p) {
    const int w = src.width, h = src.height, cw = w / 2;
    const std::uint8_t* yl = p.yLut.data();
    const std::uint8_t* ul = p.uLut.data();
    const std::uint8_t* vl = p.vLut.data();

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* s = src.y + static_cast<std::ptrdiff_t>(y) * src.yStride;
        std::uint8_t* d = dst.y + static_cast<std::ptrdiff_t>(y) * dst.yStride;
        if (!p.mirror) {
            if (p.lutIdentity) std::memcpy(d, s, static_cast<size_t>(w));
            else for (int x = 0; x < w; ++x) d[x] = yl[s[x]];
        } else {
            for (int x = 0; x < w; ++x) d[x] = yl[s[w - 1 - x]];
        }
    }
    for (int y = 0; y < h / 2; ++y) {
        const std::uint8_t* s = src.uv + static_cast<std::ptrdiff_t>(y) * src.uvStride;
        std::uint8_t* d = dst.uv + static_cast<std::ptrdiff_t>(y) * dst.uvStride;
        if (!p.mirror && p.lutIdentity) {
            std::memcpy(d, s, static_cast<size_t>(w));
            continue;
        }
        for (int i = 0; i < cw; ++i) {
            const int j = p.mirror ? cw - 1 - i : i;
            d[2 * i] = ul[s[2 * j]];
            d[2 * i + 1] = vl[s[2 * j + 1]];
        }
    }
}

void BuildGeometryTables(int w, int h, const PipelineParams& p, GeometryTables& t) {
    // Output pixel centre → source coordinate, 16.16 fixed point, clamped to the plane.
    auto build = [](std::vector<std::int32_t>& table, int outN, int planeN, double start, double span, bool flip) {
        table.resize(static_cast<size_t>(outN));
        for (int i = 0; i < outN; ++i) {
            const int o = flip ? outN - 1 - i : i;
            double sPos = start * planeN + (o + 0.5) * span * planeN / outN - 0.5;
            sPos = std::clamp(sPos, 0.0, static_cast<double>(planeN - 1));
            table[static_cast<size_t>(i)] = static_cast<std::int32_t>(std::lround(sPos * 65536.0));
        }
    };
    build(t.yX, w, w, p.srcX, p.srcW, p.mirror);
    build(t.yY, h, h, p.srcY, p.srcH, false);
    build(t.uvX, w / 2, w / 2, p.srcX, p.srcW, p.mirror);
    build(t.uvY, h / 2, h / 2, p.srcY, p.srcH, false);
}

void Nv12Processor::PrepareGeometry(int w, int h, const PipelineParams& p) {
    const double key[5] = {p.srcX, p.srcY, p.srcW, p.srcH, p.mirror ? 1.0 : 0.0};
    if (geoW_ == w && geoH_ == h && std::equal(std::begin(key), std::end(key), std::begin(geoKey_))) return;
    BuildGeometryTables(w, h, p, geo_);
    geoW_ = w;
    geoH_ = h;
    std::copy(std::begin(key), std::end(key), std::begin(geoKey_));
}

void Nv12Processor::ScalePass(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& p) {
    const int w = src.width, h = src.height;
    PrepareGeometry(w, h, p);
    const std::uint8_t* yl = p.yLut.data();
    const std::uint8_t* ul = p.uLut.data();
    const std::uint8_t* vl = p.vLut.data();

    // Bilinear, 8-bit weights. Vertical blend first into a 16-bit row (a contiguous pass the
    // compiler vectorizes), then horizontal interpolation from it: 2 loads per output pixel
    // instead of 4. Only the source columns the rectangle touches are blended.
    // The x table is monotonic (reversed when mirrored), so its ends bound the columns used.
    const int colFirst = std::min(geo_.yX.front(), geo_.yX.back()) >> 16;
    const int colLast = std::min(w - 1, (std::max(geo_.yX.front(), geo_.yX.back()) >> 16) + 1);
    blend_.resize(static_cast<size_t>(w));
    std::uint16_t* vb = blend_.data();
    for (int y = 0; y < h; ++y) {
        const std::int32_t fy = geo_.yY[static_cast<size_t>(y)];
        const int y0 = fy >> 16, y1 = std::min(y0 + 1, h - 1), wy = (fy >> 8) & 0xFF;
        const std::uint8_t* r0 = src.y + static_cast<std::ptrdiff_t>(y0) * src.yStride;
        const std::uint8_t* r1 = src.y + static_cast<std::ptrdiff_t>(y1) * src.yStride;
        const int w0 = 256 - wy;
        for (int x = colFirst; x <= colLast; ++x) {
            vb[x] = static_cast<std::uint16_t>(r0[x] * w0 + r1[x] * wy);  // <= 65280
        }
        std::uint8_t* d = dst.y + static_cast<std::ptrdiff_t>(y) * dst.yStride;
        for (int x = 0; x < w; ++x) {
            const std::int32_t fx = geo_.yX[static_cast<size_t>(x)];
            const int x0 = fx >> 16, x1 = std::min(x0 + 1, w - 1), wx = (fx >> 8) & 0xFF;
            const std::uint32_t v = static_cast<std::uint32_t>(vb[x0]) * static_cast<std::uint32_t>(256 - wx) +
                                    static_cast<std::uint32_t>(vb[x1]) * static_cast<std::uint32_t>(wx);
            d[x] = yl[(v + 32768) >> 16];
        }
    }
    const int cw = w / 2, ch = h / 2;
    for (int y = 0; y < ch; ++y) {
        const std::int32_t fy = geo_.uvY[static_cast<size_t>(y)];
        const int y0 = fy >> 16, y1 = std::min(y0 + 1, ch - 1), wy = (fy >> 8) & 0xFF;
        const std::uint8_t* r0 = src.uv + static_cast<std::ptrdiff_t>(y0) * src.uvStride;
        const std::uint8_t* r1 = src.uv + static_cast<std::ptrdiff_t>(y1) * src.uvStride;
        std::uint8_t* d = dst.uv + static_cast<std::ptrdiff_t>(y) * dst.uvStride;
        for (int x = 0; x < cw; ++x) {
            const std::int32_t fx = geo_.uvX[static_cast<size_t>(x)];
            const int x0 = fx >> 16, x1 = std::min(x0 + 1, cw - 1), wx = (fx >> 8) & 0xFF;
            for (int c = 0; c < 2; ++c) {
                const int top = r0[2 * x0 + c] * (256 - wx) + r0[2 * x1 + c] * wx;
                const int bot = r1[2 * x0 + c] * (256 - wx) + r1[2 * x1 + c] * wx;
                const int v = (top * (256 - wy) + bot * wy + 32768) >> 16;
                d[2 * x + c] = c == 0 ? ul[v] : vl[v];
            }
        }
    }
}

void Nv12Processor::SharpenPass(const Nv12Frame& f, const PipelineParams& p) {
    const int w = f.width, h = f.height;
    if (w < 3 || h < 3) return;
    rows_.resize(static_cast<size_t>(w) * 3);
    std::uint8_t* ring[3] = {rows_.data(), rows_.data() + w, rows_.data() + 2 * static_cast<ptrdiff_t>(w)};
    auto row = [&](int y) { return f.y + static_cast<std::ptrdiff_t>(y) * f.yStride; };

    // Keep original values of rows y-1, y, y+1 because row y is rewritten in place.
    std::memcpy(ring[0], row(0), static_cast<size_t>(w));
    std::memcpy(ring[1], row(1), static_cast<size_t>(w));
    const int amount = p.sharpenAmount, thr = p.sharpenThreshold, halo = p.sharpenHaloLimit;
    for (int y = 1; y < h - 1; ++y) {
        std::memcpy(ring[2], row(y + 1), static_cast<size_t>(w));
        detail::SharpenRowSse2(ring[0], ring[1], ring[2], row(y), w, amount, thr, halo);
        std::uint8_t* t = ring[0];
        ring[0] = ring[1];
        ring[1] = ring[2];
        ring[2] = t;
    }
}

}  // namespace ixc::processing
