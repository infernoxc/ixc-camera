// ixc_probe — headless camera diagnostics.
//
//   ixc_probe --list
//   ixc_probe --capture <seconds> [options]
//   ixc_probe --cycles <n> [options]          open/close repeatedly, report memory growth
//
// Options:
//   --camera <index|name-substring>   default: first physical camera
//   --width W --height H --fps F      default: 1920x1080 @ 30 (falls back to what the camera offers)
//   --output native|nv12|rgb32        default: nv12
//   --require-camera                  exit 1 (instead of 77 = skipped) when no camera exists
//
// Exit codes: 0 success, 1 failure, 2 usage error, 77 skipped (no camera).

#include "camera/capture_session.h"
#include "camera/device_enum.h"
#include "camera/format_select.h"
#include "common/strings.h"
#include "diagnostics/error.h"

#include <windows.h>
#include <mfapi.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ixc;
using namespace ixc::camera;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kExitSkip = 77;

struct Options {
    enum class Mode { None, List, Capture, Cycles } mode = Mode::None;
    int seconds = 10;
    int cycles = 20;
    std::string camera;
    FormatRequest request{1920, 1080, 30};
    OutputFormat output = OutputFormat::Nv12;
    std::string subtype;  // restrict native mode selection, e.g. "MJPG"
    bool requireCamera = false;
};

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--list") o.mode = Options::Mode::List;
        else if (a == "--capture") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::Capture; o.seconds = std::atoi(v); }
        else if (a == "--cycles") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::Cycles; o.cycles = std::atoi(v); }
        else if (a == "--camera") { const char* v = next(); if (!v) return false; o.camera = v; }
        else if (a == "--width") { const char* v = next(); if (!v) return false; o.request.width = static_cast<std::uint32_t>(std::atoi(v)); }
        else if (a == "--height") { const char* v = next(); if (!v) return false; o.request.height = static_cast<std::uint32_t>(std::atoi(v)); }
        else if (a == "--fps") { const char* v = next(); if (!v) return false; o.request.fps = std::atof(v); }
        else if (a == "--output") {
            const char* v = next();
            if (!v) return false;
            const std::string s = v;
            if (s == "native") o.output = OutputFormat::Native;
            else if (s == "nv12") o.output = OutputFormat::Nv12;
            else if (s == "rgb32") o.output = OutputFormat::Rgb32;
            else return false;
        } else if (a == "--subtype") {
            const char* v = next();
            if (!v) return false;
            o.subtype = v;
            for (auto& c : o.subtype) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else if (a == "--require-camera") o.requireCamera = true;
        else return false;
    }
    return o.mode != Options::Mode::None && o.seconds > 0 && o.seconds <= 7200 && o.cycles > 0 && o.cycles <= 10000;
}

struct ProcessSample {
    std::uint64_t cpu100ns = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSet = 0;
    std::uint64_t peakWorkingSet = 0;
    std::uint64_t handles = 0;
};

// Machine-wide busy time. Needed because Windows Frame Server can decode MJPG in its own
// service process, so process-only CPU understates the real cost of a capture mode.
struct SystemSample {
    std::uint64_t busy = 0;
    std::uint64_t total = 0;
};

std::uint64_t Ft(const FILETIME& t) { return static_cast<std::uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime; }

SystemSample SampleSystem() {
    FILETIME idle, kernel, user;
    SystemSample s;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        s.total = Ft(kernel) + Ft(user);  // kernel time includes idle time
        s.busy = s.total - Ft(idle);
    }
    return s;
}

ProcessSample SampleProcess() {
    ProcessSample s;
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        s.cpu100ns = (static_cast<std::uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                     (static_cast<std::uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.privateBytes = pmc.PrivateUsage;
        s.workingSet = pmc.WorkingSetSize;
        s.peakWorkingSet = pmc.PeakWorkingSetSize;
    }
    DWORD h = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &h)) s.handles = h;
    return s;
}

// Consumer that behaves like the processing pipeline: waits for a frame event (no polling),
// takes the newest frame, touches its pixels, releases it.
class Consumer : public ICaptureListener {
public:
    Consumer() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~Consumer() override { CloseHandle(event_); }

    void OnFrameAvailable() override { SetEvent(event_); }
    void OnStateChanged(CaptureState s, const Error& e) override {
        std::printf("  [state] %s%s%s\n", ToString(s), FAILED(e.hr) ? " — " : "", FAILED(e.hr) ? e.Describe().c_str() : "");
        std::fflush(stdout);
    }

    void Run(CaptureSession* session, const std::atomic<bool>& stop) {
        while (!stop.load()) {
            if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) continue;
            auto frame = session->TakeFrame();
            if (!frame) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED((*frame)->ConvertToContiguousBuffer(&buf))) continue;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                // Sample every 64th byte of the first plane: a rough brightness check that
                // proves real image data arrives (a closed privacy shutter reads near zero).
                const DWORD n = std::min<DWORD>(len, lumaBytes_ ? lumaBytes_ : len);
                std::uint64_t sum = 0, cnt = 0;
                for (DWORD i = 0; i < n; i += 64) { sum += data[i]; ++cnt; }
                if (cnt) lastMean_ = static_cast<double>(sum) / static_cast<double>(cnt);
                buf->Unlock();
                ++consumed_;
            }
        }
    }

    void SetLumaBytes(DWORD n) { lumaBytes_ = n; }
    std::uint64_t Consumed() const { return consumed_; }
    double MeanSample() const { return lastMean_; }

private:
    HANDLE event_;
    std::atomic<std::uint64_t> consumed_{0};
    std::atomic<double> lastMean_{-1};
    DWORD lumaBytes_ = 0;
};

bool PickCamera(const Options& o, CameraInfo& out, int& exitCode) {
    std::vector<CameraInfo> cams;
    const HRESULT hr = EnumerateCameras(cams);
    if (FAILED(hr)) {
        std::printf("error: camera enumeration failed: %s\n", Error{hr, "EnumerateCameras", ""}.Describe().c_str());
        exitCode = 1;
        return false;
    }
    if (!o.camera.empty()) {
        char* end = nullptr;
        const long idx = std::strtol(o.camera.c_str(), &end, 10);
        if (end && *end == '\0' && idx >= 0 && static_cast<size_t>(idx) < cams.size()) { out = cams[static_cast<size_t>(idx)]; return true; }
        for (const auto& c : cams) {
            if (c.name.find(o.camera) != std::string::npos) { out = c; return true; }
        }
        std::printf("error: no camera matches \"%s\"\n", o.camera.c_str());
        exitCode = 1;
        return false;
    }
    for (const auto& c : cams) {
        if (!c.isSoftwareDevice) { out = c; return true; }
    }
    std::printf("no physical camera found\n");
    exitCode = o.requireCamera ? 1 : kExitSkip;
    return false;
}

int List() {
    std::vector<CameraInfo> cams;
    HRESULT hr = EnumerateCameras(cams);
    if (FAILED(hr)) {
        std::printf("error: %s\n", Error{hr, "EnumerateCameras", "Camera enumeration failed."}.Describe().c_str());
        return 1;
    }
    std::printf("cameras: %zu\n", cams.size());
    for (size_t i = 0; i < cams.size(); ++i) {
        const auto& c = cams[i];
        std::printf("\n[%zu] %s%s\n    link: %s\n", i, c.name.c_str(), c.isSoftwareDevice ? "  (software/virtual)" : "",
                    WideToUtf8(c.symbolicLink).c_str());
        std::vector<CaptureFormat> raw;
        hr = EnumerateFormats(c.symbolicLink, raw);
        if (FAILED(hr)) {
            std::printf("    formats: unavailable — %s\n", Error{hr, "EnumerateFormats", "Could not open camera."}.Describe().c_str());
            continue;
        }
        const auto formats = NormalizeFormats(raw);
        std::printf("    native modes: %zu reported, %zu distinct\n", raw.size(), formats.size());
        for (const auto& f : formats) std::printf("      %s\n", Describe(f).c_str());
        for (auto tier : {PerformanceTier::UltraLow, PerformanceTier::Balanced}) {
            if (auto pick = SelectFormat(formats, RequestForTier(tier))) {
                std::printf("    default for %s tier: %s\n", std::string(ToString(tier)).c_str(), Describe(formats[*pick]).c_str());
            }
        }
    }
    return 0;
}

bool Resolve(const Options& o, CaptureConfig& cfg, int& exitCode) {
    CameraInfo cam;
    if (!PickCamera(o, cam, exitCode)) return false;
    std::vector<CaptureFormat> raw;
    const HRESULT hr = EnumerateFormats(cam.symbolicLink, raw);
    if (FAILED(hr)) {
        std::printf("error: %s\n", Error{hr, "EnumerateFormats", "Could not open " + cam.name + "."}.Describe().c_str());
        exitCode = 1;
        return false;
    }
    auto formats = NormalizeFormats(raw);
    if (!o.subtype.empty()) {
        std::erase_if(formats, [&](const CaptureFormat& f) { return SubtypeName(f.subtype) != o.subtype; });
    }
    const auto pick = SelectFormat(formats, o.request);
    if (!pick) {
        std::printf("error: camera reports no usable formats\n");
        exitCode = 1;
        return false;
    }
    cfg.symbolicLink = cam.symbolicLink;
    cfg.cameraName = cam.name;
    cfg.format = formats[*pick];
    cfg.output = o.output;
    return true;
}

const char* OutputName(OutputFormat f) {
    switch (f) {
        case OutputFormat::Native: return "native";
        case OutputFormat::Nv12: return "nv12";
        case OutputFormat::Rgb32: return "rgb32";
    }
    return "?";
}

int Capture(const Options& o) {
    CaptureConfig cfg;
    int exitCode = 0;
    if (!Resolve(o, cfg, exitCode)) return exitCode;

    Consumer consumer;
    ComPtr<CaptureSession> session;
    if (FAILED(CaptureSession::Create(&consumer, session))) return 1;

    std::printf("camera: %s\nrequested: %s  output: %s\n", cfg.cameraName.c_str(), Describe(cfg.format).c_str(), OutputName(cfg.output));
    // 3 s machine-wide baseline with the camera closed, to separate IXC's cost from background load.
    const SystemSample idle0 = SampleSystem();
    Sleep(3000);
    const SystemSample idle1 = SampleSystem();
    const double idlePct = idle1.total > idle0.total
                               ? 100.0 * static_cast<double>(idle1.busy - idle0.busy) / static_cast<double>(idle1.total - idle0.total)
                               : -1;
    const ProcessSample before = SampleProcess();
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    const Error err = session->Start(cfg);
    if (FAILED(err.hr)) {
        std::printf("error: %s\nerror class: %s\n", err.Describe().c_str(), std::string(ToString(ClassifyHResult(err.hr))).c_str());
        session->Close();
        return 1;
    }
    const FrameLayout layout = session->Layout();
    if (IsEqualGUID(layout.subtype, MFVideoFormat_NV12)) consumer.SetLumaBytes(static_cast<DWORD>(layout.width) * layout.height);

    std::atomic<bool> stop{false};
    std::thread worker([&] { consumer.Run(session.Get(), stop); });

    // Report once per second (the probe is a diagnostic tool; this is not the product path).
    const ProcessSample warm = SampleProcess();
    const SystemSample sys0 = SampleSystem();
    for (int s = 1; s <= o.seconds; ++s) {
        Sleep(1000);
        const auto st = session->Stats();
        std::printf("  t=%3ds  fps=%5.1f  frames=%llu  dropped=%llu  stalls=%llu  maxGap=%.1fms  lat=%.1fms\n", s, st.fps,
                    static_cast<unsigned long long>(st.framesReceived), static_cast<unsigned long long>(session->DroppedFrames()),
                    static_cast<unsigned long long>(st.stalls), st.maxIntervalMs, st.lastLatencyMs);
        std::fflush(stdout);
    }
    QueryPerformanceCounter(&t1);
    const SystemSample sys1 = SampleSystem();
    const ProcessSample during = SampleProcess();
    const auto st = session->Stats();
    const auto active = session->ActiveFormat();
    const auto state = session->State();

    stop = true;
    worker.join();
    session->Stop();
    const ProcessSample after = SampleProcess();
    session->Close();

    const double wall = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    const double cpuSec = static_cast<double>(during.cpu100ns - before.cpu100ns) / 1e7;
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    std::printf("\n--- result ---\n");
    std::printf("active format:        %s\n", Describe(active).c_str());
    std::printf("delivered as:         %s %ux%u stride %d\n", SubtypeName(layout.subtype).c_str(), layout.width, layout.height, layout.stride);
    std::printf("final state:          %s\n", ToString(state));
    std::printf("duration:             %.1f s\n", wall);
    std::printf("frames received:      %llu (avg %.2f fps)\n", static_cast<unsigned long long>(st.framesReceived),
                static_cast<double>(st.framesReceived) / wall);
    std::printf("frames consumed:      %llu\n", static_cast<unsigned long long>(consumer.Consumed()));
    std::printf("frames dropped:       %llu (newest-frame policy)\n", static_cast<unsigned long long>(session->DroppedFrames()));
    std::printf("stalls:               %llu\n", static_cast<unsigned long long>(st.stalls));
    std::printf("timestamp jumps:      %llu\n", static_cast<unsigned long long>(st.timestampJumps));
    std::printf("stream ticks (gaps):  %llu\n", static_cast<unsigned long long>(st.streamTicks));
    std::printf("interval mean/max:    %.2f / %.2f ms (jitter %.2f ms)\n", st.meanIntervalMs, st.maxIntervalMs, st.jitterMs);
    if (st.underSpeed) {
        std::printf("UNDER-SPEED:          camera delivers %.1f of %.1f FPS (typically auto exposure in low light)\n", st.fps,
                    st.nominalFps);
    }
    if (st.meanLatencyMs >= 0) std::printf("capture->app latency: %.2f ms mean\n", st.meanLatencyMs);
    else std::printf("capture->app latency: not available (no device timestamp)\n");
    std::printf("sample value mean:    %.1f%s\n", consumer.MeanSample(),
                IsEqualGUID(layout.subtype, MFVideoFormat_NV12) ? " (luma 0-255)" : " (raw bytes)");
    std::printf("cpu:                  %.2f%% of one core, %.2f%% of machine (%lu logical CPUs)\n", 100.0 * cpuSec / wall,
                100.0 * cpuSec / wall / si.dwNumberOfProcessors, si.dwNumberOfProcessors);
    if (sys1.total > sys0.total) {
        std::printf("machine-wide cpu:     %.2f%% streaming vs %.2f%% camera-closed baseline (all processes incl. Frame Server)\n",
                    100.0 * static_cast<double>(sys1.busy - sys0.busy) / static_cast<double>(sys1.total - sys0.total), idlePct);
    }
    std::printf("private bytes:        %.1f MB before open, %.1f MB streaming, %.1f MB after stop\n", before.privateBytes / 1048576.0,
                during.privateBytes / 1048576.0, after.privateBytes / 1048576.0);
    std::printf("working set:          %.1f MB streaming, peak %.1f MB\n", during.workingSet / 1048576.0, during.peakWorkingSet / 1048576.0);
    std::printf("handles:              %llu before, %llu streaming, %llu after\n", static_cast<unsigned long long>(before.handles),
                static_cast<unsigned long long>(warm.handles), static_cast<unsigned long long>(after.handles));

    const bool ok = st.framesReceived > 0 && state == CaptureState::Streaming;
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int Cycles(const Options& o) {
    CaptureConfig cfg;
    int exitCode = 0;
    if (!Resolve(o, cfg, exitCode)) return exitCode;

    Consumer consumer;
    ComPtr<CaptureSession> session;
    if (FAILED(CaptureSession::Create(&consumer, session))) return 1;
    std::printf("camera: %s  mode: %s  output: %s  cycles: %d\n", cfg.cameraName.c_str(), Describe(cfg.format).c_str(),
                OutputName(cfg.output), o.cycles);

    ProcessSample baseline{};
    int failures = 0;
    double firstFrameMsSum = 0, firstFrameMsMax = 0;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);

    for (int i = 1; i <= o.cycles; ++i) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        const Error err = session->Start(cfg);
        if (FAILED(err.hr)) {
            ++failures;
            std::printf("  cycle %d: start failed: %s\n", i, err.Describe().c_str());
            continue;
        }
        // Wait for the first frame (bounded), then a short streaming period.
        double firstMs = -1;
        for (int w = 0; w < 300; ++w) {
            if (session->Stats().framesReceived > 0) {
                QueryPerformanceCounter(&b);
                firstMs = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
                break;
            }
            Sleep(10);
        }
        Sleep(500);
        while (session->TakeFrame()) {}
        session->Stop();
        if (firstMs < 0) {
            ++failures;
            std::printf("  cycle %d: no frame within 3 s\n", i);
        } else {
            firstFrameMsSum += firstMs;
            if (firstMs > firstFrameMsMax) firstFrameMsMax = firstMs;
        }
        if (i == 2) baseline = SampleProcess();  // after warm-up (MF loads its DLLs on cycle 1)
    }
    const ProcessSample end = SampleProcess();
    session->Close();

    const int ok = o.cycles - failures;
    const double growthMb = o.cycles >= 3 ? (static_cast<double>(end.privateBytes) - static_cast<double>(baseline.privateBytes)) / 1048576.0 : 0;
    const long long handleGrowth = o.cycles >= 3 ? static_cast<long long>(end.handles) - static_cast<long long>(baseline.handles) : 0;
    std::printf("\n--- result ---\n");
    std::printf("successful cycles:    %d / %d\n", ok, o.cycles);
    if (ok) std::printf("open->first frame:    %.0f ms mean, %.0f ms max\n", firstFrameMsSum / ok, firstFrameMsMax);
    std::printf("private bytes:        %.2f MB after cycle 2, %.2f MB at end (growth %.2f MB)\n",
                baseline.privateBytes / 1048576.0, end.privateBytes / 1048576.0, growthMb);
    std::printf("handles:              %llu after cycle 2, %llu at end (growth %lld)\n", static_cast<unsigned long long>(baseline.handles),
                static_cast<unsigned long long>(end.handles), handleGrowth);

    // Leak gate: allocator and MF caches settle quickly; steady growth beyond this is a leak.
    const bool leak = growthMb > 4.0 || handleGrowth > 32;
    const bool pass = failures == 0 && !leak;
    std::printf("RESULT: %s%s\n", pass ? "PASS" : "FAIL", leak ? " (resource growth)" : "");
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options o;
    if (!ParseArgs(argc, argv, o)) {
        std::printf("usage: ixc_probe --list | --capture <sec> | --cycles <n>  [--camera X] [--width W --height H --fps F]\n"
                    "                 [--output native|nv12|rgb32] [--subtype NV12|MJPG|YUY2] [--require-camera]\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    int rc = 1;
    {
        MediaFoundationScope mf;
        if (FAILED(mf.hr())) {
            std::printf("error: %s\n", Error{mf.hr(), "MFStartup", "Media Foundation is unavailable."}.Describe().c_str());
        } else {
            switch (o.mode) {
                case Options::Mode::List: rc = List(); break;
                case Options::Mode::Capture: rc = Capture(o); break;
                case Options::Mode::Cycles: rc = Cycles(o); break;
                case Options::Mode::None: break;
            }
        }
    }
    CoUninitialize();
    return rc;
}
