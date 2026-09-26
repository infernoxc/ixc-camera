// IXC image pipeline, Direct3D 11 compute shaders (cs_5_0).
//
// Every kernel reproduces the CPU reference (processing/image_pipeline.cpp) with the same
// integer arithmetic, so GPU and CPU output are byte-identical (verified by unit test).
// Planes: Y as R8_UINT, interleaved UV as R8G8_UINT.

cbuffer Params : register(b0) {
    uint gWidth;      // luma width
    uint gHeight;     // luma height
    uint gMirror;     // 1 = flip left-right (colour pass)
    int gAmount;      // sharpen amount (0..384, /256)
    int gThreshold;   // sharpen noise threshold
    int gHalo;        // sharpen halo limit
    uint2 gPad;
};

Buffer<uint> gLut : register(t0);   // [0..255] Y, [256..511] U, [512..767] V
Texture2D<uint> gSrcY : register(t1);
Texture2D<uint2> gSrcUV : register(t2);
Buffer<int> gYX : register(t3);      // 16.16 source positions (scale pass)
Buffer<int> gYY : register(t4);
Buffer<int> gUVX : register(t5);
Buffer<int> gUVY : register(t6);

RWTexture2D<uint> gDstY : register(u0);
RWTexture2D<uint2> gDstUV : register(u1);

// ---- colour/tone lookup, optional mirror (no geometry change) ----------------------------------

[numthreads(16, 16, 1)]
void CSColorY(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gWidth || id.y >= gHeight) return;
    const uint sx = gMirror ? gWidth - 1 - id.x : id.x;
    gDstY[id.xy] = gLut[gSrcY[uint2(sx, id.y)]];
}

[numthreads(16, 16, 1)]
void CSColorUV(uint3 id : SV_DispatchThreadID) {
    const uint cw = gWidth / 2, ch = gHeight / 2;
    if (id.x >= cw || id.y >= ch) return;
    const uint sx = gMirror ? cw - 1 - id.x : id.x;
    const uint2 c = gSrcUV[uint2(sx, id.y)];
    gDstUV[id.xy] = uint2(gLut[256 + c.x], gLut[512 + c.y]);
}

// ---- bilinear rescale (crop / digital zoom / mirror) + lookup -----------------------------------

[numthreads(16, 16, 1)]
void CSScaleY(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gWidth || id.y >= gHeight) return;
    const int fy = gYY[id.y];
    const uint y0 = (uint)(fy >> 16), y1 = min(y0 + 1, gHeight - 1), wy = (uint)((fy >> 8) & 0xFF);
    const int fx = gYX[id.x];
    const uint x0 = (uint)(fx >> 16), x1 = min(x0 + 1, gWidth - 1), wx = (uint)((fx >> 8) & 0xFF);
    // Same order as the CPU: vertical blend (16-bit), then horizontal.
    const uint w0 = 256 - wy;
    const uint vb0 = gSrcY[uint2(x0, y0)] * w0 + gSrcY[uint2(x0, y1)] * wy;
    const uint vb1 = gSrcY[uint2(x1, y0)] * w0 + gSrcY[uint2(x1, y1)] * wy;
    const uint v = vb0 * (256 - wx) + vb1 * wx;
    gDstY[id.xy] = gLut[(v + 32768) >> 16];
}

[numthreads(16, 16, 1)]
void CSScaleUV(uint3 id : SV_DispatchThreadID) {
    const uint cw = gWidth / 2, ch = gHeight / 2;
    if (id.x >= cw || id.y >= ch) return;
    const int fy = gUVY[id.y];
    const uint y0 = (uint)(fy >> 16), y1 = min(y0 + 1, ch - 1), wy = (uint)((fy >> 8) & 0xFF);
    const int fx = gUVX[id.x];
    const uint x0 = (uint)(fx >> 16), x1 = min(x0 + 1, cw - 1), wx = (uint)((fx >> 8) & 0xFF);
    // Same order as the CPU: horizontal per row, then vertical.
    const uint2 a0 = gSrcUV[uint2(x0, y0)], a1 = gSrcUV[uint2(x1, y0)];
    const uint2 b0 = gSrcUV[uint2(x0, y1)], b1 = gSrcUV[uint2(x1, y1)];
    const uint2 top = a0 * (256 - wx) + a1 * wx;
    const uint2 bot = b0 * (256 - wx) + b1 * wx;
    const uint2 v = (top * (256 - wy) + bot * wy + 32768) >> 16;
    gDstUV[id.xy] = uint2(gLut[256 + v.x], gLut[512 + v.y]);
}

// ---- unsharp mask on luma (reads the colour/scale output, writes the final plane) ---------------

[numthreads(16, 16, 1)]
void CSSharpen(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gWidth || id.y >= gHeight) return;
    const int2 p = int2(id.xy);
    const int c = (int)gSrcY[p];
    if (id.x == 0 || id.y == 0 || id.x == gWidth - 1 || id.y == gHeight - 1) {
        gDstY[id.xy] = (uint)c;  // borders untouched, as on the CPU
        return;
    }
    const int al = (int)gSrcY[p + int2(-1, -1)], am = (int)gSrcY[p + int2(0, -1)], ar = (int)gSrcY[p + int2(1, -1)];
    const int cl = (int)gSrcY[p + int2(-1, 0)], cr = (int)gSrcY[p + int2(1, 0)];
    const int bl = (int)gSrcY[p + int2(-1, 1)], bm = (int)gSrcY[p + int2(0, 1)], br = (int)gSrcY[p + int2(1, 1)];
    const int blur = (al + 2 * am + ar + 2 * cl + 4 * c + 2 * cr + bl + 2 * bm + br + 8) >> 4;
    const int detail = c - blur;
    if (detail <= gThreshold && detail >= -gThreshold) {
        gDstY[id.xy] = (uint)c;
        return;
    }
    const int mn = min(min(min(am, bm), min(cl, cr)), c);
    const int mx = max(max(max(am, bm), max(cl, cr)), c);
    const int delta = (detail * 16 * (gAmount * 16)) >> 16;  // floor(detail*amount/256), as on the CPU
    const int v = clamp(c + delta, mn - gHalo, mx + gHalo);
    gDstY[id.xy] = (uint)clamp(v, 0, 255);
}
