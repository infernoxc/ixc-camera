#include "effects/stickers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ixc::effects::stickers {

namespace {

enum class Shape : std::uint8_t { Circle, Ellipse, RoundRect, Capsule, Heart, Triangle };

// One shape in face-local units. Parameters by kind:
//   Circle    cx cy r
//   Ellipse   cx cy rx ry angleDeg
//   RoundRect cx cy halfW halfH radius
//   Capsule   x0 y0 x1 y1 r
//   Heart     cx cy size           (centre of the heart's bounding box)
//   Triangle  tipX tipY halfW h    (isosceles, apex up, base h below the tip)
// nose: the shape's position is relative to the nose instead of the eye midpoint.
struct Part {
    Shape shape;
    float p[5];
    std::uint8_t r, g, b;
    float alpha;
    bool nose = false;
};

// Shades: dark rounded lenses with a thin rim, bridge, arms and a soft highlight.
constexpr Part kShades[] = {
    {Shape::Capsule, {-0.86f, -0.06f, -1.08f, -0.02f, 0.035f}, 20, 20, 24, 1.0f},
    {Shape::Capsule, {0.86f, -0.06f, 1.08f, -0.02f, 0.035f}, 20, 20, 24, 1.0f},
    {Shape::RoundRect, {-0.5f, 0.03f, 0.40f, 0.27f, 0.14f}, 15, 15, 18, 1.0f},
    {Shape::RoundRect, {0.5f, 0.03f, 0.40f, 0.27f, 0.14f}, 15, 15, 18, 1.0f},
    {Shape::RoundRect, {-0.5f, 0.03f, 0.355f, 0.225f, 0.11f}, 30, 34, 44, 0.96f},
    {Shape::RoundRect, {0.5f, 0.03f, 0.355f, 0.225f, 0.11f}, 30, 34, 44, 0.96f},
    {Shape::Capsule, {-0.13f, -0.07f, 0.13f, -0.07f, 0.045f}, 15, 15, 18, 1.0f},
    {Shape::Capsule, {-0.72f, -0.10f, -0.56f, -0.10f, 0.035f}, 255, 255, 255, 0.45f},
    {Shape::Capsule, {0.28f, -0.10f, 0.44f, -0.10f, 0.035f}, 255, 255, 255, 0.45f},
};

// Heart Eyes: glossy red hearts over the eyes.
constexpr Part kHeartEyes[] = {
    {Shape::Heart, {-0.52f, 0.0f, 0.62f}, 225, 30, 70, 0.97f},
    {Shape::Heart, {0.52f, 0.0f, 0.62f}, 225, 30, 70, 0.97f},
    {Shape::Ellipse, {-0.64f, -0.12f, 0.07f, 0.045f, -30}, 255, 235, 240, 0.8f},
    {Shape::Ellipse, {0.40f, -0.12f, 0.07f, 0.045f, -30}, 255, 235, 240, 0.8f},
};

// Crown: three gold points on a band, with jewels, resting on the head.
constexpr Part kCrown[] = {
    {Shape::Triangle, {-0.62f, -2.15f, 0.30f, 0.62f}, 235, 180, 35, 1.0f},
    {Shape::Triangle, {0.0f, -2.35f, 0.34f, 0.82f}, 235, 180, 35, 1.0f},
    {Shape::Triangle, {0.62f, -2.15f, 0.30f, 0.62f}, 235, 180, 35, 1.0f},
    {Shape::RoundRect, {0.0f, -1.43f, 0.92f, 0.16f, 0.05f}, 220, 160, 25, 1.0f},
    {Shape::Circle, {-0.62f, -2.12f, 0.07f}, 250, 230, 140, 1.0f},
    {Shape::Circle, {0.0f, -2.32f, 0.08f}, 250, 230, 140, 1.0f},
    {Shape::Circle, {0.62f, -2.12f, 0.07f}, 250, 230, 140, 1.0f},
    {Shape::Circle, {-0.45f, -1.43f, 0.075f}, 200, 30, 60, 1.0f},
    {Shape::Circle, {0.0f, -1.43f, 0.09f}, 40, 90, 210, 1.0f},
    {Shape::Circle, {0.45f, -1.43f, 0.075f}, 200, 30, 60, 1.0f},
};

// Puppy: floppy ears on the head, a shiny nose on the nose.
constexpr Part kPuppy[] = {
    {Shape::Ellipse, {-1.05f, -1.35f, 0.36f, 0.66f, 22}, 125, 80, 45, 1.0f},
    {Shape::Ellipse, {1.05f, -1.35f, 0.36f, 0.66f, -22}, 125, 80, 45, 1.0f},
    {Shape::Ellipse, {-1.02f, -1.28f, 0.20f, 0.44f, 22}, 235, 160, 165, 1.0f},
    {Shape::Ellipse, {1.02f, -1.28f, 0.20f, 0.44f, -22}, 235, 160, 165, 1.0f},
    {Shape::Ellipse, {0.0f, -0.02f, 0.25f, 0.17f, 0}, 30, 22, 22, 1.0f, true},
    {Shape::Ellipse, {-0.07f, -0.08f, 0.07f, 0.035f, -15}, 255, 255, 255, 0.55f, true},
};

struct Art {
    const Part* parts;
    int count;
};

Art ArtFor(Id id) {
    switch (id) {
        case Id::Shades: return {kShades, static_cast<int>(std::size(kShades))};
        case Id::HeartEyes: return {kHeartEyes, static_cast<int>(std::size(kHeartEyes))};
        case Id::Crown: return {kCrown, static_cast<int>(std::size(kCrown))};
        case Id::Puppy: return {kPuppy, static_cast<int>(std::size(kPuppy))};
    }
    return {kShades, 0};
}

float Len(float x, float y) { return std::sqrt(x * x + y * y); }

// Signed distance (local units, negative inside) from (x, y) to the part (positioned already).
float Sdf(const Part& s, float x, float y) {
    const float* p = s.p;
    switch (s.shape) {
        case Shape::Circle: return Len(x - p[0], y - p[1]) - p[2];
        case Shape::Ellipse: {
            const float a = p[4] * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
            const float dx = x - p[0], dy = y - p[1];
            const float lx = (c * dx + sn * dy) / p[2], ly = (-sn * dx + c * dy) / p[3];
            const float k0 = Len(lx, ly), k1 = Len(lx / p[2], ly / p[3]);
            return k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -std::min(p[2], p[3]);
        }
        case Shape::RoundRect: {
            const float qx = std::abs(x - p[0]) - p[2] + p[4], qy = std::abs(y - p[1]) - p[3] + p[4];
            return Len(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - p[4];
        }
        case Shape::Capsule: {
            const float pax = x - p[0], pay = y - p[1], bax = p[2] - p[0], bay = p[3] - p[1];
            const float h = std::clamp((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
            return Len(pax - bax * h, pay - bay * h) - p[4];
        }
        case Shape::Heart: {
            // Heart of height ~1 with its tip at the bottom, scaled to `size` and centred.
            const float sz = p[2];
            const float hx = std::abs(x - p[0]) / sz;
            const float hy = (p[1] - y) / sz + 0.5f;  // y up, tip at 0
            float d;
            if (hy + hx > 1.0f) {
                d = Len(hx - 0.25f, hy - 0.75f) - 0.35355339f;
            } else {
                const float m = 0.5f * std::max(hx + hy, 0.0f);
                d = std::sqrt(std::min((hx * hx + (hy - 1.0f) * (hy - 1.0f)), (hx - m) * (hx - m) + (hy - m) * (hy - m))) *
                    (hx - hy > 0 ? 1.0f : -1.0f);
            }
            return d * sz;
        }
        case Shape::Triangle: {
            // Isosceles, apex at the tip, base h below (y down).
            const float qx = p[2], qy = p[3];
            const float px = std::abs(x - p[0]), py = y - p[1];
            const float t = std::clamp((px * qx + py * qy) / (qx * qx + qy * qy), 0.0f, 1.0f);
            const float ax = px - qx * t, ay = py - qy * t;
            const float u = std::clamp(px / qx, 0.0f, 1.0f);
            const float bx = px - qx * u, by = py - qy;
            const float d0 = std::min(ax * ax + ay * ay, bx * bx + by * by);
            const float sgn = std::min(-(px * qy - py * qx), -(py - qy));  // qy > 0
            return -std::sqrt(d0) * (sgn > 0 ? 1.0f : -1.0f);
        }
    }
    return 1e9f;
}

// Bounding radius of a part around its centre (local units), for quick rejection.
void Bounds(const Part& s, float& cx, float& cy, float& rad) {
    const float* p = s.p;
    switch (s.shape) {
        case Shape::Circle: cx = p[0], cy = p[1], rad = p[2]; return;
        case Shape::Ellipse: cx = p[0], cy = p[1], rad = std::max(p[2], p[3]); return;
        case Shape::RoundRect: cx = p[0], cy = p[1], rad = Len(p[2], p[3]); return;
        case Shape::Capsule: cx = (p[0] + p[2]) / 2, cy = (p[1] + p[3]) / 2, rad = Len(p[2] - p[0], p[3] - p[1]) / 2 + p[4]; return;
        case Shape::Heart: cx = p[0], cy = p[1], rad = p[2] * 0.8f; return;
        case Shape::Triangle: cx = p[0], cy = p[1] + p[3] / 2, rad = Len(p[2], p[3] / 2); return;
    }
    cx = cy = rad = 0;
}

struct Yuv {
    float y, u, v;
};
Yuv ToYuv(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8, bool fullRange) {
    const float r = r8, g = g8, b = b8;  // BT.709
    if (fullRange) return {0.2126f * r + 0.7152f * g + 0.0722f * b, 128 - 0.1146f * r - 0.3854f * g + 0.5f * b, 128 + 0.5f * r - 0.4542f * g - 0.0458f * b};
    return {16 + 0.1826f * r + 0.6142f * g + 0.0620f * b, 128 - 0.1006f * r - 0.3386f * g + 0.4392f * b, 128 + 0.4392f * r - 0.3989f * g - 0.0403f * b};
}

constexpr int kMaxParts = 16;

}  // namespace

void Draw(const processing::Nv12Frame& f, Id id, const Anchor& a, float opacity, bool fullRange) {
    const Art art = ArtFor(id);
    if (art.count == 0 || art.count > kMaxParts || opacity < 0.01f || !f.y || !f.uv) return;
    const float ex = a.eyeRight.x - a.eyeLeft.x, ey = a.eyeRight.y - a.eyeLeft.y;
    const float scale = Len(ex, ey);  // pixels per local unit
    if (scale < 4.0f) return;
    const float ux = ex / scale, uy = ey / scale;  // local x axis (eye line), output pixels
    const float vx = -uy, vy = ux;                 // local y axis (down the face)
    const float ox = (a.eyeLeft.x + a.eyeRight.x) / 2, oy = (a.eyeLeft.y + a.eyeRight.y) / 2;
    // Nose in local units (anchors nose-relative parts).
    const float nlx = ((a.nose.x - ox) * ux + (a.nose.y - oy) * uy) / scale;
    const float nly = ((a.nose.x - ox) * vx + (a.nose.y - oy) * vy) / scale;

    // Positioned parts, their colours and local bounding circles (on the stack).
    Part parts[kMaxParts];
    Yuv colour[kMaxParts];
    float bcx[kMaxParts], bcy[kMaxParts], brad[kMaxParts];
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    const float aa = 1.5f / scale;  // anti-aliasing margin in local units
    for (int i = 0; i < art.count; ++i) {
        Part s = art.parts[i];
        if (s.nose) {
            s.p[0] += nlx;
            s.p[1] += nly;
            if (s.shape == Shape::Capsule) {
                s.p[2] += nlx;
                s.p[3] += nly;
            }
        }
        parts[i] = s;
        colour[i] = ToYuv(s.r, s.g, s.b, fullRange);
        Bounds(s, bcx[i], bcy[i], brad[i]);
        brad[i] += aa;
        // Bounding box in output pixels: transform the bounding circle's centre, add the radius.
        const float px = ox + (bcx[i] * ux + bcy[i] * vx) * scale, py = oy + (bcx[i] * uy + bcy[i] * vy) * scale;
        const float rr = brad[i] * scale;
        minX = std::min(minX, px - rr), maxX = std::max(maxX, px + rr);
        minY = std::min(minY, py - rr), maxY = std::max(maxY, py + rr);
    }
    const int x0 = std::max(0, static_cast<int>(minX) & ~1), x1 = std::min(f.width - 1, static_cast<int>(maxX) + 1);
    const int y0 = std::max(0, static_cast<int>(minY) & ~1), y1 = std::min(f.height - 1, static_cast<int>(maxY) + 1);
    if (x1 <= x0 || y1 <= y0) return;
    const float inv = 1.0f / scale;

    // Composites every part at a point (local units); returns the result over `base`.
    auto shade = [&](float lx, float ly, float* acc, int channels, const float* base) {
        float out[3] = {base[0], channels > 1 ? base[1] : 0, channels > 2 ? base[2] : 0};
        bool touched = false;
        for (int i = 0; i < art.count; ++i) {
            const float dx = lx - bcx[i], dy = ly - bcy[i];
            if (dx * dx + dy * dy > brad[i] * brad[i]) continue;
            const float d = Sdf(parts[i], lx, ly) * scale;  // pixels
            const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cover <= 0) continue;
            const float w = cover * parts[i].alpha * opacity;
            if (channels == 1) {
                out[0] += (colour[i].y - out[0]) * w;
            } else {
                out[0] += (colour[i].u - out[0]) * w;
                out[1] += (colour[i].v - out[1]) * w;
            }
            touched = true;
        }
        for (int c = 0; c < channels; ++c) acc[c] = out[c];
        return touched;
    };

    for (int y = y0; y <= y1; ++y) {
        std::uint8_t* row = f.y + static_cast<std::ptrdiff_t>(y) * f.yStride;
        const float py = static_cast<float>(y) + 0.5f - oy;
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f - ox;
            const float lx = (px * ux + py * uy) * inv, ly = (px * vx + py * vy) * inv;
            const float base = row[x];
            float o;
            if (shade(lx, ly, &o, 1, &base)) row[x] = static_cast<std::uint8_t>(std::clamp(o + 0.5f, 0.0f, 255.0f));
        }
    }
    for (int y = y0 / 2; y <= y1 / 2 && y < f.height / 2; ++y) {
        std::uint8_t* row = f.uv + static_cast<std::ptrdiff_t>(y) * f.uvStride;
        const float py = static_cast<float>(2 * y) + 1.0f - oy;
        for (int x = x0 / 2; x <= x1 / 2 && x < f.width / 2; ++x) {
            const float px = static_cast<float>(2 * x) + 1.0f - ox;
            const float lx = (px * ux + py * uy) * inv, ly = (px * vx + py * vy) * inv;
            const float base[2] = {static_cast<float>(row[2 * x]), static_cast<float>(row[2 * x + 1])};
            float o[2];
            if (shade(lx, ly, o, 2, base)) {
                row[2 * x] = static_cast<std::uint8_t>(std::clamp(o[0] + 0.5f, 0.0f, 255.0f));
                row[2 * x + 1] = static_cast<std::uint8_t>(std::clamp(o[1] + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

}  // namespace ixc::effects::stickers
