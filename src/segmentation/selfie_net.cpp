#include "segmentation/selfie_net.h"

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace ixc::seg {

namespace {

#include "../../third_party/mediapipe_selfie_segmenter/selfie_segmenter_landscape.inc"

static_assert(kInputW == SelfieNet::kWidth && kInputH == SelfieNet::kHeight);

float HalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    const std::uint32_t man = h & 0x3FFu;
    float f;
    if (exp == 0) {
        f = std::ldexp(static_cast<float>(man), -24);  // zero or subnormal
        return sign ? -f : f;
    }
    std::uint32_t bits;
    if (exp == 31) bits = sign | 0x7F800000u | (man << 13);  // inf/NaN (not present in the model)
    else bits = sign | ((exp + 112) << 23) | (man << 13);
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

inline float Activate(float v, int act) {
    switch (static_cast<Act>(act)) {
        case Act::None: return v;
        case Act::Relu: return v > 0 ? v : 0;
        case Act::HardSwish: return v * std::clamp(v + 3.0f, 0.0f, 6.0f) * (1.0f / 6.0f);
        case Act::Logistic: return 1.0f / (1.0f + std::exp(-v));
    }
    return v;
}

void ActivateSpan(float* p, size_t n, int act) {
    switch (static_cast<Act>(act)) {  // one branch per span, then a plain (vectorizable) loop
        case Act::None: return;
        case Act::Relu:
            for (size_t i = 0; i < n; ++i) p[i] = p[i] > 0 ? p[i] : 0;
            return;
        case Act::HardSwish:
            for (size_t i = 0; i < n; ++i) p[i] = Activate(p[i], static_cast<int>(Act::HardSwish));
            return;
        case Act::Logistic:
            for (size_t i = 0; i < n; ++i) p[i] = Activate(p[i], static_cast<int>(Act::Logistic));
            return;
    }
}

// out[c] += v * w[c]. The pointers never alias (arena tensors are disjoint from the weights and
// from each other), which lets the compiler vectorize without runtime overlap checks.
// SSE2 (every x64 CPU) so the speed doesn't depend on the compiler's auto-vectorizer.
inline void Axpy(float* __restrict out, const float* __restrict w, float v, int n) {
    int c = 0;
    const __m128 vv = _mm_set1_ps(v);
    for (; c + 4 <= n; c += 4) _mm_storeu_ps(out + c, _mm_add_ps(_mm_loadu_ps(out + c), _mm_mul_ps(vv, _mm_loadu_ps(w + c))));
    for (; c < n; ++c) out[c] += v * w[c];
}
inline void MulAcc(float* __restrict out, const float* __restrict a, const float* __restrict b, int n) {
    int c = 0;
    for (; c + 4 <= n; c += 4)
        _mm_storeu_ps(out + c, _mm_add_ps(_mm_loadu_ps(out + c), _mm_mul_ps(_mm_loadu_ps(a + c), _mm_loadu_ps(b + c))));
    for (; c < n; ++c) out[c] += a[c] * b[c];
}

// Convolution with weights [k][k][I][O]: the inner loop runs over output channels (contiguous,
// no reduction), which compilers vectorize.
void Conv(const Op& op, const float* in, float* out, const float* w, const float* b) {
    const int I = op.c, O = op.oc, k = op.k, s = op.stride;
    for (int oy = 0; oy < op.oh; ++oy) {
        for (int ox = 0; ox < op.ow; ++ox) {
            float* o = out + (static_cast<size_t>(oy) * op.ow + ox) * O;
            std::copy(b, b + O, o);
            for (int ky = 0; ky < k; ++ky) {
                const int iy = oy * s - op.padT + ky;
                if (iy < 0 || iy >= op.h) continue;
                for (int kx = 0; kx < k; ++kx) {
                    const int ix = ox * s - op.padL + kx;
                    if (ix < 0 || ix >= op.w) continue;
                    const float* px = in + (static_cast<size_t>(iy) * op.w + ix) * I;
                    const float* wk = w + static_cast<size_t>(ky * k + kx) * I * O;
                    for (int i = 0; i < I; ++i) Axpy(o, wk + static_cast<size_t>(i) * O, px[i], O);
                }
            }
            ActivateSpan(o, static_cast<size_t>(O), op.act);
        }
    }
}

// Depthwise convolution with weights [k][k][C].
void DwConv(const Op& op, const float* in, float* out, const float* w, const float* b) {
    const int C = op.c, k = op.k, s = op.stride;
    for (int oy = 0; oy < op.oh; ++oy) {
        for (int ox = 0; ox < op.ow; ++ox) {
            float* o = out + (static_cast<size_t>(oy) * op.ow + ox) * C;
            std::copy(b, b + C, o);
            for (int ky = 0; ky < k; ++ky) {
                const int iy = oy * s - op.padT + ky;
                if (iy < 0 || iy >= op.h) continue;
                for (int kx = 0; kx < k; ++kx) {
                    const int ix = ox * s - op.padL + kx;
                    if (ix < 0 || ix >= op.w) continue;
                    const float* px = in + (static_cast<size_t>(iy) * op.w + ix) * C;
                    const float* wk = w + static_cast<size_t>(ky * k + kx) * C;
                    MulAcc(o, px, wk, C);
                }
            }
            ActivateSpan(o, static_cast<size_t>(C), op.act);
        }
    }
}

// 2x2, stride 2 transposed convolution (no overlap): each input pixel writes a 2x2 output block.
// Weights [2][2][I][O].
void Deconv2x(const Op& op, const float* in, float* out, const float* w, const float* b) {
    const int I = op.c, O = op.oc;
    for (int y = 0; y < op.h; ++y) {
        for (int x = 0; x < op.w; ++x) {
            const float* px = in + (static_cast<size_t>(y) * op.w + x) * I;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    float* o = out + (static_cast<size_t>(2 * y + dy) * op.ow + (2 * x + dx)) * O;
                    std::copy(b, b + O, o);
                    const float* wk = w + static_cast<size_t>(dy * 2 + dx) * I * O;
                    for (int i = 0; i < I; ++i) Axpy(o, wk + static_cast<size_t>(i) * O, px[i], O);
                    ActivateSpan(o, static_cast<size_t>(O), op.act);
                }
            }
        }
    }
}

// Bilinear 2x upsampling, half-pixel centres, edges clamped (TFLite RESIZE_BILINEAR semantics).
void Resize2x(const Op& op, const float* in, float* out) {
    const int C = op.c;
    for (int y = 0; y < op.oh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * 0.5f - 0.5f;
        const float y0f = std::floor(fy), wy = fy - y0f;
        const int y0 = std::max(static_cast<int>(y0f), 0), y1 = std::min(static_cast<int>(y0f) + 1, op.h - 1);
        for (int x = 0; x < op.ow; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * 0.5f - 0.5f;
            const float x0f = std::floor(fx), wx = fx - x0f;
            const int x0 = std::max(static_cast<int>(x0f), 0), x1 = std::min(static_cast<int>(x0f) + 1, op.w - 1);
            const float* a = in + (static_cast<size_t>(y0) * op.w + x0) * C;
            const float* bb = in + (static_cast<size_t>(y0) * op.w + x1) * C;
            const float* c0 = in + (static_cast<size_t>(y1) * op.w + x0) * C;
            const float* d = in + (static_cast<size_t>(y1) * op.w + x1) * C;
            float* o = out + (static_cast<size_t>(y) * op.ow + x) * C;
            for (int c = 0; c < C; ++c) {
                const float top = a[c] + (bb[c] - a[c]) * wx, bottom = c0[c] + (d[c] - c0[c]) * wx;
                o[c] = top + (bottom - top) * wy;
            }
        }
    }
}

void Mean(const Op& op, const float* in, float* out) {
    const int C = op.c;
    const size_t n = static_cast<size_t>(op.h) * op.w;
    std::fill(out, out + C, 0.0f);
    for (size_t p = 0; p < n; ++p) {
        const float* px = in + p * C;
        for (int c = 0; c < C; ++c) out[c] += px[c];
    }
    const float inv = 1.0f / static_cast<float>(n);
    for (int c = 0; c < C; ++c) out[c] *= inv;
}

void Binary(const Op& op, const float* a, const float* b, float* out, bool mul) {
    const int C = op.c;
    const size_t n = static_cast<size_t>(op.h) * op.w;
    for (size_t p = 0; p < n; ++p) {
        const float* x = a + p * C;
        const float* y = op.k == 1 ? b : b + p * C;  // per-channel vector or same shape
        float* o = out + p * C;
        if (mul) {
            for (int c = 0; c < C; ++c) o[c] = x[c] * y[c];
        } else {
            for (int c = 0; c < C; ++c) o[c] = x[c] + y[c];
        }
    }
}

}  // namespace

bool SelfieNet::Init() {
    if (Ready()) return true;
    try {
        weights_.resize(kWeightCount);
        for (std::uint32_t i = 0; i < kWeightCount; ++i) weights_[i] = HalfToFloat(kWeightsF16[i]);
        arena_.assign(kArenaFloats, 0.0f);
    } catch (const std::bad_alloc&) {
        weights_ = {};
        arena_ = {};
        return false;
    }
    // Validate the table once (it's compiled in, so this only guards against a bad conversion).
    for (const Op& op : kOps) {
        const auto fits = [&](int off, long long n) { return off >= 0 && off + n <= static_cast<long long>(kArenaFloats); };
        const long long inN = static_cast<long long>(op.h) * op.w * op.c, outN = static_cast<long long>(op.oh) * op.ow * op.oc;
        if (!fits(op.in0, inN) || !fits(op.out, outN) || (op.in1 >= 0 && !fits(op.in1, op.k == 1 ? op.c : inN)) ||
            (op.wOff >= 0 && op.wOff >= static_cast<int>(kWeightCount)) || (op.bOff >= 0 && op.bOff + op.oc > static_cast<int>(kWeightCount))) {
            weights_ = {};
            arena_ = {};
            return false;
        }
    }
    return true;
}

float* SelfieNet::Input() { return Ready() ? arena_.data() + kInputOffset : nullptr; }

const float* SelfieNet::Run() {
    if (!Ready()) return nullptr;
    float* a = arena_.data();
    const float* w = weights_.data();
    for (const Op& op : kOps) {
        const float* in = a + op.in0;
        float* out = a + op.out;
        const float* wt = op.wOff >= 0 ? w + op.wOff : nullptr;
        const float* bias = op.bOff >= 0 ? w + op.bOff : nullptr;
        const size_t n = static_cast<size_t>(op.oh) * op.ow * op.oc;
        switch (static_cast<OpType>(op.type)) {
            case OpType::Conv: Conv(op, in, out, wt, bias); break;
            case OpType::DwConv: DwConv(op, in, out, wt, bias); break;
            case OpType::Deconv2x: Deconv2x(op, in, out, wt, bias); break;
            case OpType::Resize2x: Resize2x(op, in, out); break;
            case OpType::Mean: Mean(op, in, out); break;
            case OpType::Mul: Binary(op, in, a + op.in1, out, true); break;
            case OpType::Add: Binary(op, in, a + op.in1, out, false); break;
            case OpType::Logistic:
                std::copy(in, in + n, out);
                ActivateSpan(out, n, static_cast<int>(Act::Logistic));
                break;
            case OpType::Relu:
                std::copy(in, in + n, out);
                ActivateSpan(out, n, static_cast<int>(Act::Relu));
                break;
            case OpType::HardSwish:
                std::copy(in, in + n, out);
                ActivateSpan(out, n, static_cast<int>(Act::HardSwish));
                break;
        }
    }
    return a + kOutputOffset;
}

}  // namespace ixc::seg
