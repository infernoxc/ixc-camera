#include "processing/temporal_denoise.h"

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ixc::processing {

double TemporalDenoiser::AutoStrengthForGain(double ev) {
    if (!(ev > 0.1)) return 0;
    return 25 + 35 * std::min(ev / 1.5, 1.0);
}

void TemporalDenoiser::Configure(double strength, double noise) {
    const double s = std::clamp(strength, 0.0, 100.0) / 100.0;
    // Threshold in 8-bit levels of the smoothed difference: at least a few levels, and a
    // multiple of the measured noise; anything well above it is treated as real change. Capped
    // so a frame-wide change (a pan) is never mistaken for noise.
    const double thr = std::clamp(std::max(3 + 13 * s, noise * (2.0 + 1.5 * s)), 3.0, 32.0);
    thr4_ = static_cast<int>(std::lround(thr * 4));                          // 12..128
    kmax_ = static_cast<int>(std::lround(16 * (0.35 + 0.45 * s)));           // 6/16 .. 13/16
    slope_ = static_cast<int>(std::lround(kmax_ * 2.0 * 4096.0 / thr4_));    // <= 8875: fits 16 bits
}

// One row (luma: step 1; interleaved chroma: step 2), in place. The scalar loop is the
// reference; the SSE2 loop computes exactly the same integers 8 pixels at a time.
void TemporalDenoiser::Row(std::uint8_t* row, std::uint16_t* hist, int n, int step, const std::uint8_t* kLimit, std::uint8_t* kOut,
                           bool sample) {
    std::uint8_t* cur = cur_.data();
    std::uint8_t* prev = prev_.data();
    std::uint16_t* dRow = dRow_.data();
    std::memcpy(cur, row, static_cast<size_t>(n));
    const int thr4 = thr4_, slope = slope_, kmax = kmax_;
    auto pixel = [&](int i) {
        const int il = i >= step ? i - step : i;
        const int ir = i + step < n ? i + step : i;
        const int c = cur[il] + 2 * cur[i] + cur[ir];
        const int p = prev[il] + 2 * prev[i] + prev[ir];
        const int d4 = std::abs(c - p);
        dRow[i] = static_cast<std::uint16_t>(d4);
        int k = std::min(kmax, (std::max(0, thr4 - d4) * 16 * slope) >> 16);
        if (kLimit) k = std::min<int>(k, kLimit[i]);
        if (kOut) kOut[i] = static_cast<std::uint8_t>(k);
        const int c16 = cur[i] << 4;
        const int h = c16 + (((hist[i] - c16) * k) >> 4);  // arithmetic shift: floor, as SSE2 mulhi
        hist[i] = static_cast<std::uint16_t>(h);
        row[i] = static_cast<std::uint8_t>(std::min(255, (h + 8) >> 4));
    };

    int i = 0;
    if (scalar_) {
        for (int x = 0; x < n; ++x) prev[x] = static_cast<std::uint8_t>(std::min(255, (hist[x] + 8) >> 4));
        for (; i < n; ++i) pixel(i);
    } else {
        int x = 0;
        const __m128i eight = _mm_set1_epi16(8);
        for (; x + 16 <= n; x += 16) {
            const __m128i a = _mm_srli_epi16(_mm_add_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hist + x)), eight), 4);
            const __m128i b = _mm_srli_epi16(_mm_add_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hist + x + 8)), eight), 4);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(prev + x), _mm_packus_epi16(a, b));
        }
        for (; x < n; ++x) prev[x] = static_cast<std::uint8_t>(std::min(255, (hist[x] + 8) >> 4));

        for (; i < step; ++i) pixel(i);  // left edge
        const __m128i zero = _mm_setzero_si128();
        const __m128i thrV = _mm_set1_epi16(static_cast<short>(thr4)), slopeV = _mm_set1_epi16(static_cast<short>(slope));
        const __m128i kmaxV = _mm_set1_epi16(static_cast<short>(kmax));
        auto load8 = [&](const std::uint8_t* q) { return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(q)), zero); };
        for (; i + 8 + step <= n; i += 8) {
            const __m128i cm = load8(cur + i);
            const __m128i c = _mm_add_epi16(_mm_add_epi16(load8(cur + i - step), load8(cur + i + step)), _mm_slli_epi16(cm, 1));
            const __m128i p = _mm_add_epi16(_mm_add_epi16(load8(prev + i - step), load8(prev + i + step)), _mm_slli_epi16(load8(prev + i), 1));
            const __m128i d4 = _mm_or_si128(_mm_subs_epu16(c, p), _mm_subs_epu16(p, c));
            if (sample) _mm_storeu_si128(reinterpret_cast<__m128i*>(dRow + i), d4);
            const __m128i t = _mm_slli_epi16(_mm_subs_epu16(thrV, d4), 4);
            __m128i k = _mm_min_epi16(_mm_mulhi_epu16(t, slopeV), kmaxV);
            if (kLimit) k = _mm_min_epi16(k, load8(kLimit + i));
            if (kOut) _mm_storel_epi64(reinterpret_cast<__m128i*>(kOut + i), _mm_packus_epi16(k, k));
            const __m128i c16 = _mm_slli_epi16(cm, 4);
            const __m128i hv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(hist + i));
            const __m128i diff = _mm_slli_epi16(_mm_sub_epi16(hv, c16), 3);              // |.| <= 32640
            const __m128i h = _mm_add_epi16(c16, _mm_mulhi_epi16(diff, _mm_slli_epi16(k, 9)));  // (diff*k) >> 4
            _mm_storeu_si128(reinterpret_cast<__m128i*>(hist + i), h);
            const __m128i o = _mm_srli_epi16(_mm_add_epi16(h, eight), 4);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(row + i), _mm_packus_epi16(o, o));
        }
        for (; i < n; ++i) pixel(i);  // right edge
    }
    if (sample)
        for (int x = 0; x < n; ++x) ++hist_[std::min<size_t>(dRow[x], 1023)];
}

void TemporalDenoiser::Apply(const Nv12Frame& f, double strength) {
    if (!(strength > 0)) {
        Release();
        return;
    }
    if (!f.y || !f.uv || f.width < 4 || f.height < 2 || (f.width & 1) || (f.height & 1)) return;
    const int w = f.width, h = f.height;
    const size_t ySize = static_cast<size_t>(w) * static_cast<size_t>(h), uvSize = ySize / 2;
    if (histW_ != w || histH_ != h) {
        // First frame (or a new size): it becomes the history as is.
        if (histY_.size() != ySize) {
            histY_.assign(ySize, 0);
            histUV_.assign(uvSize, 0);
            cur_.assign(static_cast<size_t>(w), 0);
            prev_.assign(static_cast<size_t>(w), 0);
            kRow_.assign(static_cast<size_t>(w), 0);
            kRow2_.assign(static_cast<size_t>(w), 0);
            dRow_.assign(static_cast<size_t>(w), 0);
        }
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* s = f.y + static_cast<size_t>(y) * f.yStride;
            std::uint16_t* d = histY_.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) d[x] = static_cast<std::uint16_t>(s[x] << 4);
        }
        for (int y = 0; y < h / 2; ++y) {
            const std::uint8_t* s = f.uv + static_cast<size_t>(y) * f.uvStride;
            std::uint16_t* d = histUV_.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) d[x] = static_cast<std::uint16_t>(s[x] << 4);
        }
        histW_ = w;
        histH_ = h;
        noise_ = 0;
        return;
    }
    Configure(strength, noise_);
    hist_.fill(0);
    std::uint64_t sampled = 0;
    for (int j = 0; j < h / 2; ++j) {
        const int y0 = 2 * j;
        const bool sample = (j & 1) == 0;  // every 4th luma row feeds the noise estimate
        Row(f.y + static_cast<size_t>(y0) * f.yStride, histY_.data() + static_cast<size_t>(y0) * w, w, 1, nullptr, kRow_.data(), sample);
        Row(f.y + static_cast<size_t>(y0 + 1) * f.yStride, histY_.data() + static_cast<size_t>(y0 + 1) * w, w, 1, nullptr, kRow2_.data(), false);
        if (sample) sampled += static_cast<std::uint64_t>(w);
        // Chroma sample (U,V at bytes 2i, 2i+1) covers luma columns 2i, 2i+1 of both rows.
        for (int x = 0; x < w; ++x) kRow_[static_cast<size_t>(x)] = std::min(kRow_[static_cast<size_t>(x)], kRow2_[static_cast<size_t>(x)]);
        for (int x = 0; x < w; x += 2) {
            const std::uint8_t k = std::min(kRow_[static_cast<size_t>(x)], kRow_[static_cast<size_t>(x) + 1]);
            kRow2_[static_cast<size_t>(x)] = kRow2_[static_cast<size_t>(x) + 1] = k;
        }
        Row(f.uv + static_cast<size_t>(j) * f.uvStride, histUV_.data() + static_cast<size_t>(j) * w, w, 2, kRow2_.data(), nullptr, false);
    }
    // Noise estimate for the next frame: for Gaussian noise the 25th percentile of |difference|
    // is 0.32 sigma. Smoothed over frames so one busy frame doesn't swing the threshold.
    const std::uint64_t target = sampled / 4;
    std::uint64_t acc = 0;
    int p25 = 0;
    while (p25 < 1023 && acc + hist_[static_cast<size_t>(p25)] < target) acc += hist_[static_cast<size_t>(p25++)];
    const double sigma = p25 / 4.0 / 0.32;
    noise_ = noise_ > 0 ? noise_ * 0.8 + sigma * 0.2 : sigma;
}

void TemporalDenoiser::Release() {
    if (histY_.capacity() == 0) return;
    std::vector<std::uint16_t>().swap(histY_);
    std::vector<std::uint16_t>().swap(histUV_);
    std::vector<std::uint8_t>().swap(cur_);
    std::vector<std::uint8_t>().swap(prev_);
    std::vector<std::uint8_t>().swap(kRow_);
    std::vector<std::uint8_t>().swap(kRow2_);
    std::vector<std::uint16_t>().swap(dRow_);
    histW_ = histH_ = 0;
}

size_t TemporalDenoiser::MemoryBytes() const {
    return (histY_.capacity() + histUV_.capacity() + dRow_.capacity()) * sizeof(std::uint16_t) + cur_.capacity() + prev_.capacity() +
           kRow_.capacity() + kRow2_.capacity();
}

}  // namespace ixc::processing
