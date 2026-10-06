// Direct3D 11 compute kernels for the segmentation network (segmentation/selfie_net.cpp).
//
// Same operations, same weight layout and the same summation order as the CPU kernels, one
// thread per output value. All tensors live in one float arena (NHWC, N = 1), exactly as on the
// CPU, so the op table's offsets are used unchanged. Results match the CPU within float rounding
// (test_gpu_segmentation.cpp).
//
// Compiled with fxc (cs_5_0): unsigned index arithmetic only (no signed division).

RWStructuredBuffer<float> arena : register(u0);
StructuredBuffer<float> weights : register(t0);

cbuffer OpParams : register(b0) {
    uint in0, in1, outOff, act;      // arena offsets (in1 unused = 0), fused activation
    uint h, w, c, k;                 // input shape, kernel size (Mul/Add: k == 1 = per-channel in1)
    uint oh, ow, oc, stride;         // output shape, stride
    int padT, padL;                  // SAME padding before the first row/column
    uint wOff, bOff;                 // weight/bias offsets
    uint total, mode, pad0, pad1;    // number of output values; Binary: 0 = Mul, 1 = Add
};

// Activations (Act in selfie_net.h): 0 None, 1 Relu, 2 HardSwish, 3 Logistic.
float Activate(float v, uint a) {
    if (a == 1) return v > 0 ? v : 0;
    if (a == 2) return v * clamp(v + 3.0f, 0.0f, 6.0f) * (1.0f / 6.0f);
    if (a == 3) return 1.0f / (1.0f + exp(-v));
    return v;
}

[numthreads(64, 1, 1)]
void CSConv(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= total) return;
    const uint o = i % oc, p = i / oc, ox = p % ow, oy = p / ow;
    float sum = weights[bOff + o];
    for (uint ky = 0; ky < k; ++ky) {
        const int iy = int(oy * stride) - padT + int(ky);
        if (iy < 0 || iy >= int(h)) continue;
        for (uint kx = 0; kx < k; ++kx) {
            const int ix = int(ox * stride) - padL + int(kx);
            if (ix < 0 || ix >= int(w)) continue;
            const uint px = in0 + (uint(iy) * w + uint(ix)) * c;
            const uint wk = wOff + (ky * k + kx) * c * oc + o;
            for (uint ci = 0; ci < c; ++ci) sum += arena[px + ci] * weights[wk + ci * oc];
        }
    }
    arena[outOff + i] = Activate(sum, act);
}

[numthreads(64, 1, 1)]
void CSDwConv(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= total) return;
    const uint ch = i % c, p = i / c, ox = p % ow, oy = p / ow;
    float sum = weights[bOff + ch];
    for (uint ky = 0; ky < k; ++ky) {
        const int iy = int(oy * stride) - padT + int(ky);
        if (iy < 0 || iy >= int(h)) continue;
        for (uint kx = 0; kx < k; ++kx) {
            const int ix = int(ox * stride) - padL + int(kx);
            if (ix < 0 || ix >= int(w)) continue;
            sum += arena[in0 + (uint(iy) * w + uint(ix)) * c + ch] * weights[wOff + (ky * k + kx) * c + ch];
        }
    }
    arena[outOff + i] = Activate(sum, act);
}

// 2x2 stride-2 transposed convolution: each output pixel comes from exactly one input pixel.
[numthreads(64, 1, 1)]
void CSDeconv2x(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= total) return;
    const uint o = i % oc, p = i / oc, ox = p % ow, oy = p / ow;
    const uint x = ox / 2, y = oy / 2, dx = ox % 2, dy = oy % 2;
    float sum = weights[bOff + o];
    const uint px = in0 + (y * w + x) * c;
    const uint wk = wOff + (dy * 2 + dx) * c * oc + o;
    for (uint ci = 0; ci < c; ++ci) sum += arena[px + ci] * weights[wk + ci * oc];
    arena[outOff + i] = Activate(sum, act);
}

// Bilinear 2x, half-pixel centres, edges clamped.
[numthreads(64, 1, 1)]
void CSResize2x(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= total) return;
    const uint ch = i % c, p = i / c, x = p % ow, y = p / ow;
    const float fy = (float(y) + 0.5f) * 0.5f - 0.5f, fx = (float(x) + 0.5f) * 0.5f - 0.5f;
    const float y0f = floor(fy), x0f = floor(fx);
    const float wy = fy - y0f, wx = fx - x0f;
    const uint y0 = uint(max(int(y0f), 0)), y1 = uint(min(int(y0f) + 1, int(h) - 1));
    const uint x0 = uint(max(int(x0f), 0)), x1 = uint(min(int(x0f) + 1, int(w) - 1));
    const float a = arena[in0 + (y0 * w + x0) * c + ch], b = arena[in0 + (y0 * w + x1) * c + ch];
    const float cc = arena[in0 + (y1 * w + x0) * c + ch], d = arena[in0 + (y1 * w + x1) * c + ch];
    const float top = a + (b - a) * wx, bottom = cc + (d - cc) * wx;
    arena[outOff + i] = top + (bottom - top) * wy;
}

// Global average over the spatial dimensions: one thread per channel.
[numthreads(64, 1, 1)]
void CSMean(uint3 id : SV_DispatchThreadID) {
    const uint ch = id.x;
    if (ch >= c) return;
    const uint n = h * w;
    float sum = 0;
    for (uint p = 0; p < n; ++p) sum += arena[in0 + p * c + ch];
    arena[outOff + ch] = sum * (1.0f / float(n));
}

// Mul / Add (mode), with in1 either the same shape or one value per channel (k == 1).
[numthreads(64, 1, 1)]
void CSBinary(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= total) return;
    const float x = arena[in0 + i];
    const float y = arena[in1 + (k == 1 ? i % c : i)];
    arena[outOff + i] = mode == 0 ? x * y : x + y;
}

// Standalone activation ops (Logistic, Relu, HardSwish): act selects which.
[numthreads(64, 1, 1)]
void CSActivate(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= total) return;
    arena[outOff + i] = Activate(arena[in0 + i], act);
}
