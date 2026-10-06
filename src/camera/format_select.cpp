#include "camera/format_select.h"

#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace ixc::camera {

std::string SubtypeName(const GUID& s) {
    if (IsEqualGUID(s, MFVideoFormat_RGB32)) return "RGB32";
    if (IsEqualGUID(s, MFVideoFormat_ARGB32)) return "ARGB32";
    if (IsEqualGUID(s, MFVideoFormat_RGB24)) return "RGB24";
    // FOURCC-based subtypes share the MFVideoFormat base GUID with Data1 = FOURCC.
    GUID base = MFVideoFormat_Base;
    base.Data1 = s.Data1;
    if (IsEqualGUID(base, s)) {
        char cc[5] = {static_cast<char>(s.Data1 & 0xFF), static_cast<char>((s.Data1 >> 8) & 0xFF),
                      static_cast<char>((s.Data1 >> 16) & 0xFF), static_cast<char>((s.Data1 >> 24) & 0xFF), 0};
        if (std::all_of(cc, cc + 4, [](char c) { return c >= 0x20 && c < 0x7F; })) return cc;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "{%08lX-%04X-%04X-...}", static_cast<unsigned long>(s.Data1), s.Data2, s.Data3);
    return buf;
}

std::string Describe(const CaptureFormat& f) {
    char fps[16];
    const double r = f.Fps();
    if (std::fabs(r - std::round(r)) < 0.005) std::snprintf(fps, sizeof(fps), "%.0f", r);
    else std::snprintf(fps, sizeof(fps), "%.2f", r);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ux%u @ %s FPS (%s)", f.width, f.height, fps, SubtypeName(f.subtype).c_str());
    return buf;
}

bool IsSoftwareDeviceLink(const std::wstring& link) {
    std::wstring lower(link.substr(0, 16));
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    return lower.rfind(L"\\\\?\\root#", 0) == 0 || lower.rfind(L"\\\\?\\swd#", 0) == 0;
}

int SubtypeRank(const GUID& s) {
    if (IsEqualGUID(s, MFVideoFormat_NV12)) return 0;
    if (IsEqualGUID(s, MFVideoFormat_YUY2)) return 1;
    if (IsEqualGUID(s, MFVideoFormat_MJPG)) return 2;
    return 3;
}

FormatRequest RequestForTier(PerformanceTier tier) {
    switch (tier) {
        case PerformanceTier::UltraLow:
        case PerformanceTier::Low:
            return {1280, 720, 30};
        case PerformanceTier::Auto:
        case PerformanceTier::Balanced:
        case PerformanceTier::High: {
            FormatRequest r;
            r.best = true;
            return r;
        }
    }
    return {1920, 1080, 30};
}

std::vector<CaptureFormat> NormalizeFormats(std::vector<CaptureFormat> formats) {
    std::vector<CaptureFormat> out;
    out.reserve(formats.size());
    for (const auto& f : formats) {
        if (f.width == 0 || f.height == 0 || f.fpsNumerator == 0 || f.fpsDenominator == 0) continue;
        if (std::none_of(out.begin(), out.end(), [&](const CaptureFormat& o) { return o.SameMode(f); })) out.push_back(f);
    }
    std::stable_sort(out.begin(), out.end(), [](const CaptureFormat& a, const CaptureFormat& b) {
        const auto areaA = static_cast<std::uint64_t>(a.width) * a.height;
        const auto areaB = static_cast<std::uint64_t>(b.width) * b.height;
        if (areaA != areaB) return areaA > areaB;
        if (a.width != b.width) return a.width > b.width;
        if (a.Fps() != b.Fps()) return a.Fps() > b.Fps();
        return SubtypeRank(a.subtype) < SubtypeRank(b.subtype);
    });
    return out;
}

namespace {

// FormatRequest::best: largest smooth resolution, then its highest frame rate, then the cheapest
// pixel format. A camera with no smooth mode at all gets its fastest mode.
std::optional<size_t> SelectBest(const std::vector<CaptureFormat>& formats) {
    constexpr double kSmoothFps = 23.5;  // 24/25/30/60 count as smooth; 15 doesn't
    auto area = [](const CaptureFormat& f) { return static_cast<std::uint64_t>(f.width) * f.height; };
    const bool anySmooth = std::any_of(formats.begin(), formats.end(), [&](const CaptureFormat& f) { return f.Fps() >= kSmoothFps; });
    std::optional<size_t> best;
    for (size_t i = 0; i < formats.size(); ++i) {
        const CaptureFormat& f = formats[i];
        if (anySmooth && f.Fps() < kSmoothFps) continue;
        if (!best) {
            best = i;
            continue;
        }
        const CaptureFormat& b = formats[*best];
        const bool better = anySmooth ? (area(f) != area(b) ? area(f) > area(b)
                                         : f.Fps() != b.Fps() ? f.Fps() > b.Fps()
                                                              : SubtypeRank(f.subtype) < SubtypeRank(b.subtype))
                                      : (f.Fps() != b.Fps() ? f.Fps() > b.Fps() : area(f) > area(b));
        if (better) best = i;
    }
    return best;
}

}  // namespace

std::optional<size_t> SelectFormat(const std::vector<CaptureFormat>& formats, const FormatRequest& req) {
    if (formats.empty()) return std::nullopt;
    if (req.best) return SelectBest(formats);

    // 1. Resolution: exact match if possible; otherwise the largest mode not exceeding the
    //    request; otherwise the smallest mode above it.
    const std::uint64_t wantArea = static_cast<std::uint64_t>(req.width) * req.height;
    auto area = [](const CaptureFormat& f) { return static_cast<std::uint64_t>(f.width) * f.height; };

    std::uint64_t chosenArea = 0;
    std::uint32_t chosenW = 0, chosenH = 0;
    bool haveRes = false;
    if (req.width && req.height) {
        for (const auto& f : formats) {
            if (f.width == req.width && f.height == req.height) { chosenW = f.width; chosenH = f.height; haveRes = true; break; }
        }
    }
    if (!haveRes) {
        const double targetFps = req.fps > 0 ? req.fps : 30.0;
        // Auto (no size): the largest resolution that reaches the target frame rate.
        // With a size: the largest not exceeding it.
        for (const auto& f : formats) {
            const bool fits = wantArea ? area(f) <= wantArea : f.Fps() + 0.5 >= targetFps;
            if (fits && area(f) > chosenArea) { chosenArea = area(f); chosenW = f.width; chosenH = f.height; haveRes = true; }
        }
    }
    if (!haveRes) {
        // Nothing fits: take the smallest above the request (or, in auto mode, the largest overall).
        for (const auto& f : formats) {
            const bool better = !haveRes || (wantArea ? area(f) < chosenArea : area(f) > chosenArea);
            if (better) { chosenArea = area(f); chosenW = f.width; chosenH = f.height; haveRes = true; }
        }
    }

    // 2. Frame rate within that resolution: closest to the request, preferring >= request.
    //    3. Tie-break on pixel format.
    const double wantFps = req.fps > 0 ? req.fps : 30.0;
    std::optional<size_t> best;
    auto score = [&](const CaptureFormat& f) {
        const double d = f.Fps() - wantFps;
        // Below-target rates are penalized heavily; above-target rates mildly.
        const double fpsPenalty = d < -0.5 ? 1000.0 + (-d) * 10.0 : std::fabs(d) < 0.5 ? 0.0 : d;
        return fpsPenalty * 10.0 + SubtypeRank(f.subtype);
    };
    for (size_t i = 0; i < formats.size(); ++i) {
        const auto& f = formats[i];
        if (f.width != chosenW || f.height != chosenH) continue;
        if (!best || score(f) < score(formats[*best])) best = i;
    }
    return best;
}

}  // namespace ixc::camera
