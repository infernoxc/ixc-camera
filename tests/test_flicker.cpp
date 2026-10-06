#include "camera/exposure_governor.h"
#include "camera/flicker_detector.h"
#include "camera/power_line.h"
#include "ixc_test.h"
#include "profiles/profile.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace ixc::camera;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRows = 240;

// Row profile of a room: uneven, non-periodic brightness (window light, a desk edge, a person).
double Scene(int r) {
    double v = 70 + 50 * std::exp(-((r - 60.0) * (r - 60.0)) / 900.0);  // bright patch upper left
    if (r > 170) v -= 25;                                               // desk edge
    return v + 0.05 * r;
}

struct Rng {
    std::uint32_t s = 7;
    double Next() {  // -0.5..0.5
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 16777216.0 - 0.5;
    }
};

// Bands: relative ripple `depth` at `cycles` per frame height, phase moving `drift` cycles per frame.
std::vector<float> Frame(int t, double depth, double cycles, double drift, Rng& rng, int blockTop = -1, double gain = 1.0) {
    std::vector<float> rows(kRows);
    for (int r = 0; r < kRows; ++r) {
        double v = Scene(r) * gain;
        if (blockTop >= 0 && r >= blockTop && r < blockTop + 60) v -= 30;  // a person moving up/down
        v *= 1 + depth * std::sin(2 * kPi * (cycles * r / kRows + drift * t));
        rows[static_cast<size_t>(r)] = static_cast<float>(v + rng.Next() * 0.6);
    }
    return rows;
}

}  // namespace

IXC_TEST(Flicker_PeriodicShareFindsSinusoid) {
    std::vector<float> d(200);
    for (int i = 0; i < 200; ++i) d[static_cast<size_t>(i)] = static_cast<float>(3.0 * std::sin(2 * kPi * 3.4 * i / 200) + 0.02 * i + 5);
    double amp = 0;
    const double share = FlickerDetector::PeriodicShare(d.data(), 200, amp);
    IXC_CHECK(share > 0.8);
    IXC_CHECK(std::abs(amp - 3.0) < 0.6);
    for (int i = 0; i < 200; ++i) d[static_cast<size_t>(i)] = static_cast<float>(i < 100 ? 10 : -10);  // a step (motion), not periodic
    IXC_CHECK(FlickerDetector::PeriodicShare(d.data(), 200, amp) < 0.45);
}

IXC_TEST(Flicker_RowMeansSampleRows) {
    const int w = 64, h = 512;
    std::vector<std::uint8_t> y(static_cast<size_t>(w) * h);
    for (int r = 0; r < h; ++r)
        for (int x = 0; x < w; ++x) y[static_cast<size_t>(r) * w + x] = static_cast<std::uint8_t>(r / 2);
    float rows[FlickerDetector::kMaxRows];
    const int n = FlickerDetector::RowMeans(y.data(), w, w, h, rows);
    IXC_CHECK_EQ(n, FlickerDetector::kMaxRows);
    IXC_CHECK(rows[0] < 2 && rows[n - 1] > 252);
    IXC_CHECK_EQ(FlickerDetector::RowMeans(y.data(), w, 8, h, rows), 0);
}

IXC_TEST(Flicker_MovingBandsDetected) {
    // 30 FPS under 100 Hz light: 3.33 flicker cycles per frame time, phase moves 1/3 per frame.
    FlickerDetector d;
    Rng rng;
    for (int t = 0; t < 30; ++t) d.AddReference(Frame(t, 0, 0, 0, rng).data(), kRows);
    int detectedAt = -1;
    for (int t = 0; t < 40 && detectedAt < 0; ++t)
        if (d.AddFixed(Frame(t, 0.06, 3.33, 1.0 / 3, rng).data(), kRows)) detectedAt = t;
    IXC_CHECK(detectedAt >= 0 && detectedAt <= 32);
}

IXC_TEST(Flicker_StandingBandsDetected) {
    // 30 FPS under 120 Hz light: bands stand still; found against the auto-exposure profile.
    FlickerDetector d;
    Rng rng;
    for (int t = 0; t < 20; ++t) d.AddReference(Frame(t, 0, 0, 0, rng).data(), kRows);
    bool hit = false;
    for (int t = 0; t < 20; ++t) hit = d.AddFixed(Frame(t, 0.08, 4.0, 0, rng, -1, 0.7).data(), kRows) || hit;
    IXC_CHECK(hit);
}

IXC_TEST(Flicker_NoFalseAlarms) {
    Rng rng;
    {  // still scene, darker under fixed exposure (normal): no bands
        FlickerDetector d;
        for (int t = 0; t < 20; ++t) d.AddReference(Frame(t, 0, 0, 0, rng).data(), kRows);
        bool hit = false;
        for (int t = 0; t < 120; ++t) hit = d.AddFixed(Frame(t, 0, 0, 0, rng, -1, 0.7).data(), kRows) || hit;
        IXC_CHECK(!hit);
    }
    {  // a person moving up and down through the frame
        FlickerDetector d;
        for (int t = 0; t < 20; ++t) d.AddReference(Frame(t, 0, 0, 0, rng, 40).data(), kRows);
        bool hit = false;
        for (int t = 0; t < 120; ++t) {
            const int top = 40 + static_cast<int>(60 * std::sin(t * 0.3));
            hit = d.AddFixed(Frame(t, 0, 0, 0, rng, top, 0.8).data(), kRows) || hit;
        }
        IXC_CHECK(!hit);
    }
    {  // faint ripple below visibility (0.3%) is ignored
        FlickerDetector d;
        bool hit = false;
        for (int t = 0; t < 120; ++t) hit = d.AddFixed(Frame(t, 0.003, 3.33, 1.0 / 3, rng).data(), kRows) || hit;
        IXC_CHECK(!hit);
    }
}

IXC_TEST(Flicker_GovernorGivesExposureBackAndRemembers) {
    ExposureGovernor g;
    g.Reset(true, 30);
    // Slow camera: observe, then switch to fixed exposure.
    ExposureGovernor::Action a = ExposureGovernor::Action::None;
    for (int i = 0; i < 200 && a != ExposureGovernor::Action::SetManualExposure; ++i) a = g.OnFrame(i ? 50 : 0, 100);
    IXC_CHECK(a == ExposureGovernor::Action::SetManualExposure);
    g.OnExposureApplied(true);
    IXC_CHECK(g.ExposureChanged());
    IXC_CHECK(g.AbortForFlicker() == ExposureGovernor::Action::RestoreAutoExposure);
    IXC_CHECK(!g.ExposureChanged());
    IXC_CHECK(g.state() == ExposureGovernor::State::Disabled);
    IXC_CHECK_EQ(g.CompensationEv(), 0.0);
    const ExposureGovernor::Hint h = g.CurrentHint();
    IXC_CHECK(h.flicker && !h.valid);
    // Next session at the same rate: stays on auto exposure; at another rate it tries again.
    g.Reset(true, 30, h);
    IXC_CHECK(g.state() == ExposureGovernor::State::Disabled);
    g.Reset(true, 60, h);
    IXC_CHECK(g.state() == ExposureGovernor::State::Observing);
}

IXC_TEST(Flicker_RegionMainsFrequency) {
    IXC_CHECK_EQ(MainsHzForRegion("IN"), 50);
    IXC_CHECK_EQ(MainsHzForRegion("gb"), 50);
    IXC_CHECK_EQ(MainsHzForRegion("US"), 60);
    IXC_CHECK_EQ(MainsHzForRegion("br"), 60);
    IXC_CHECK_EQ(MainsHzForRegion("KR"), 60);
    IXC_CHECK_EQ(MainsHzForRegion("JP"), 0);  // both
    IXC_CHECK_EQ(MainsHzForRegion(""), 0);
    IXC_CHECK_EQ(MainsHzForRegion("001"), 0);
    IXC_CHECK_EQ(MainsHzForRegion("1A"), 0);
    IXC_CHECK(ResolveAntiFlicker(ixc::AntiFlicker::Auto, "IN") == ixc::AntiFlicker::Hz50);
    IXC_CHECK(ResolveAntiFlicker(ixc::AntiFlicker::Auto, "CA") == ixc::AntiFlicker::Hz60);
    IXC_CHECK(ResolveAntiFlicker(ixc::AntiFlicker::Auto, "JP") == ixc::AntiFlicker::Auto);
    IXC_CHECK(ResolveAntiFlicker(ixc::AntiFlicker::Off, "US") == ixc::AntiFlicker::Off);
    IXC_CHECK(ResolveAntiFlicker(ixc::AntiFlicker::Hz50, "US") == ixc::AntiFlicker::Hz50);
}

IXC_TEST(Flicker_ProfileSetting) {
    ixc::Profile p;
    IXC_CHECK(p.antiFlicker == ixc::AntiFlicker::Auto);
    p.antiFlicker = ixc::AntiFlicker::Hz50;
    const auto r = ixc::ProfileFromJson(ixc::ProfileToJson(p));
    IXC_CHECK(r.ok && r.profile.antiFlicker == ixc::AntiFlicker::Hz50 && r.warnings.empty());
    std::string text = ixc::ProfileToJson(p);
    const auto at = text.find("\"50hz\"");
    IXC_CHECK(at != std::string::npos);
    text.replace(at, 6, "\"55hz\"");
    const auto bad = ixc::ProfileFromJson(text);
    IXC_CHECK(bad.ok && bad.profile.antiFlicker == ixc::AntiFlicker::Auto && bad.warnings.size() == 1);  // never silently
}
