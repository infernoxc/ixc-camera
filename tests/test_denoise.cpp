#include "effects/effects.h"
#include "ixc_test.h"
#include "processing/image_pipeline.h"
#include "processing/temporal_denoise.h"
#include "profiles/profile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace ixc;
using processing::TemporalDenoiser;

namespace {

constexpr int kW = 160, kH = 96;

// Deterministic, roughly Gaussian camera noise (sum of 4 uniforms).
struct Noise {
    std::uint32_t s = 12345;
    double Next(double sigma) {
        double sum = 0;
        for (int i = 0; i < 4; ++i) {
            s = s * 1664525u + 1013904223u;
            sum += static_cast<double>(s >> 8) / 16777216.0 - 0.5;
        }
        return sum * sigma * std::sqrt(3.0);  // variance of the sum of 4 U(-0.5,0.5) = 1/3
    }
};

struct Clip {
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(static_cast<size_t>(kW) * kH * 3 / 2, 128);
    processing::Nv12Frame View() { return {buf.data(), buf.data() + static_cast<size_t>(kW) * kH, kW, kW, kW, kH}; }
    std::uint8_t& Y(int x, int y) { return buf[static_cast<size_t>(y) * kW + x]; }
    std::uint8_t& U(int x, int y) { return buf[static_cast<size_t>(kW) * kH + static_cast<size_t>(y / 2) * kW + (x / 2) * 2]; }
};

std::uint8_t Px(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

// Clean picture: a horizontal gradient with a few vertical bars (real detail to keep).
double Clean(int x, int /*y*/) { return 60 + x * 0.6 + ((x / 20) % 2 ? 25 : 0); }

void Render(Clip& c, Noise& n, double sigma, int squareX = -1, double squareY = 200) {
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            double v = Clean(x, y);
            if (squareX >= 0 && x >= squareX && x < squareX + 24 && y >= 36 && y < 60) v = squareY;
            c.Y(x, y) = Px(v + n.Next(sigma));
        }
    for (int y = 0; y < kH; y += 2)
        for (int x = 0; x < kW; x += 2) {
            const bool sq = squareX >= 0 && x >= squareX && x < squareX + 24 && y >= 36 && y < 60;
            c.U(x, y) = Px((sq ? 70 : 128) + n.Next(sigma * 0.7));
            c.buf[static_cast<size_t>(kW) * kH + static_cast<size_t>(y / 2) * kW + (x / 2) * 2 + 1] = Px(128 + n.Next(sigma * 0.7));
        }
}

// RMS difference from the clean picture over the luma plane (inside a margin).
double LumaError(Clip& c) {
    double sum = 0;
    int n = 0;
    for (int y = 2; y < kH - 2; ++y)
        for (int x = 2; x < kW - 2; ++x) {
            const double d = c.Y(x, y) - Clean(x, y);
            sum += d * d;
            ++n;
        }
    return std::sqrt(sum / n);
}

}  // namespace

IXC_TEST(Denoise_StaticSceneNoiseIsReduced) {
    TemporalDenoiser d;
    Noise n;
    Clip c;
    double before = 0, after = 0;
    for (int i = 0; i < 30; ++i) {
        Render(c, n, 6.0);
        if (i == 29) before = LumaError(c);
        d.Apply(c.View(), 60);
    }
    after = LumaError(c);
    // Input noise ~6 levels RMS; a recursive average over the static scene must cut it well down.
    IXC_CHECK(before > 5.0);
    IXC_CHECK(after < before * 0.6);
    IXC_CHECK(d.MemoryBytes() >= static_cast<size_t>(kW) * kH * 3);
}

IXC_TEST(Denoise_MovingObjectLeavesNoGhost) {
    TemporalDenoiser d;
    Noise n;
    Clip c;
    for (int i = 0; i < 20; ++i) {  // bright square parked at x=20 long enough to fill the history
        Render(c, n, 3.0, 20);
        d.Apply(c.View(), 100);
    }
    Render(c, n, 3.0, 100);  // it jumps to x=100 in one frame
    d.Apply(c.View(), 100);
    // Where it was: background again at once (no bright trail), within the noise.
    double oldArea = 0, newArea = 0, oldChroma = 0;
    int cnt = 0;
    for (int y = 40; y < 56; ++y)
        for (int x = 24; x < 40; ++x) {
            oldArea += std::abs(c.Y(x, y) - Clean(x, y));
            newArea += c.Y(x + 80, y);
            oldChroma += std::abs(c.U(x, y) - 128);
            ++cnt;
        }
    IXC_CHECK(oldArea / cnt < 6.0);
    IXC_CHECK(oldChroma / cnt < 6.0);  // colour doesn't trail either
    // Where it is now: the square at full brightness, not faded by the old history.
    IXC_CHECK(newArea / cnt > 190.0);
}

IXC_TEST(Denoise_NoDriftOnCleanOrSlowlyChangingPicture) {
    TemporalDenoiser d;
    Clip c;
    // A clean constant picture stays exactly as it is.
    for (int i = 0; i < 10; ++i) {
        std::fill(c.buf.begin(), c.buf.begin() + kW * kH, std::uint8_t{100});
        d.Apply(c.View(), 100);
    }
    IXC_CHECK_EQ(static_cast<int>(c.Y(50, 50)), 100);
    // A slow fade (one level per frame, like auto exposure settling) is followed closely: the
    // fractional history doesn't get stuck at a rounding step.
    int worst = 0;
    for (int i = 1; i <= 60; ++i) {
        std::fill(c.buf.begin(), c.buf.begin() + kW * kH, static_cast<std::uint8_t>(100 + i));
        d.Apply(c.View(), 100);
        worst = std::max(worst, std::abs(c.Y(50, 50) - (100 + i)));
    }
    IXC_CHECK(worst <= 4);
    // Once the fade stops, the output settles on the true value.
    for (int i = 0; i < 30; ++i) {
        std::fill(c.buf.begin(), c.buf.begin() + kW * kH, std::uint8_t{160});
        d.Apply(c.View(), 100);
    }
    IXC_CHECK(std::abs(c.Y(50, 50) - 160) <= 1);
}

IXC_TEST(Denoise_OffKeepsFrameAndReleasesMemory) {
    TemporalDenoiser d;
    Noise n;
    Clip c;
    Render(c, n, 5.0);
    d.Apply(c.View(), 50);
    d.Apply(c.View(), 50);
    IXC_CHECK(d.MemoryBytes() > 0);
    Render(c, n, 5.0);
    const auto before = c.buf;
    d.Apply(c.View(), 0);
    IXC_CHECK(c.buf == before);
    IXC_CHECK_EQ(d.MemoryBytes(), size_t{0});
    // Switching it back on starts over from the current frame (unchanged), without stale history.
    d.Apply(c.View(), 50);
    IXC_CHECK(c.buf == before);
}

IXC_TEST(Denoise_SmoothMotionGainRaisesDenoiseAndSharpenThreshold) {
    IXC_CHECK_EQ(TemporalDenoiser::AutoStrengthForGain(0), 0.0);
    IXC_CHECK_EQ(TemporalDenoiser::AutoStrengthForGain(0.05), 0.0);
    IXC_CHECK(TemporalDenoiser::AutoStrengthForGain(0.5) > 25);
    IXC_CHECK(TemporalDenoiser::AutoStrengthForGain(1.5) == 60);
    IXC_CHECK(TemporalDenoiser::AutoStrengthForGain(9) == 60);

    Profile p;
    p.image.denoise = 10;
    const Profile g = processing::WithSmoothMotionGain(p, 1.0);
    IXC_CHECK_EQ(g.image.exposureEv, 1.0);
    IXC_CHECK(g.image.denoise > 40);
    p.image.denoise = 90;  // the user's stronger setting wins
    IXC_CHECK_EQ(processing::WithSmoothMotionGain(p, 1.0).image.denoise, 90.0);

    Profile q;
    IXC_CHECK_EQ(processing::CompileParams(q, 640, 360, false).sharpenThreshold, 2);
    q.image.denoise = 100;
    IXC_CHECK_EQ(processing::CompileParams(q, 640, 360, false).sharpenThreshold, 6);

    // Effects: denoise is its own setting, on even with the Effects master switch off.
    Profile e;
    e.effectsEnabled = false;
    IXC_CHECK(!effects::CompileEffects(e, false)->Active());
    e.image.denoise = 30;
    const auto cfg = effects::CompileEffects(e, false);
    IXC_CHECK(cfg->Active());
    IXC_CHECK_EQ(cfg->denoise, 30.0);
    IXC_CHECK(!cfg->needsSegmentation);
}

IXC_TEST(Denoise_RendererAppliesAndReleases) {
    Profile p;
    p.image.denoise = 80;
    const auto on = effects::CompileEffects(p, false);
    p.image.denoise = 0;
    const auto off = effects::CompileEffects(p, false);
    effects::EffectRenderer r;
    Noise n;
    Clip c;
    double before = 0;
    for (int i = 0; i < 20; ++i) {
        Render(c, n, 6.0);
        if (i == 19) before = LumaError(c);
        r.Apply(c.View(), *on, {});
    }
    IXC_CHECK(LumaError(c) < before * 0.7);
    IXC_CHECK(r.ScratchBytes() >= static_cast<size_t>(kW) * kH * 3);
    r.Apply(c.View(), *off, {});
    IXC_CHECK_EQ(r.ScratchBytes(), size_t{0});
}

IXC_TEST(Denoise_Sse2MatchesScalarReference) {
    // Odd-sized rows exercise the scalar edges next to the 8-pixel SSE2 blocks.
    TemporalDenoiser fast, ref;
    ref.SetScalarForTesting(true);
    Noise n;
    Clip a, b;
    bool same = true;
    for (int i = 0; i < 12; ++i) {
        Render(a, n, i < 6 ? 8.0 : 2.0, i * 9 % 120, 210);  // moving square, changing noise
        b.buf = a.buf;
        fast.Apply(a.View(), 35 + i * 5);
        ref.Apply(b.View(), 35 + i * 5);
        same = same && a.buf == b.buf;
    }
    IXC_CHECK(same);
    std::vector<std::uint8_t> odd(static_cast<size_t>(102) * 34 * 3 / 2, 90), odd2;
    TemporalDenoiser f2, r2;
    r2.SetScalarForTesting(true);
    for (int i = 0; i < 6; ++i) {
        for (size_t k = 0; k < odd.size(); ++k) odd[k] = static_cast<std::uint8_t>(90 + n.Next(6));
        odd2 = odd;
        const processing::Nv12Frame fa{odd.data(), odd.data() + 102 * 34, 102, 102, 102, 34};
        const processing::Nv12Frame fb{odd2.data(), odd2.data() + 102 * 34, 102, 102, 102, 34};
        f2.Apply(fa, 70);
        r2.Apply(fb, 70);
        IXC_CHECK(odd == odd2);
    }
}
