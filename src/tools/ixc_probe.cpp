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
#include "effects/effects.h"
#include "face/downscale.h"
#include "face/face_engine.h"
#include "face/tracker.h"
#include "processing/gpu/gpu_pipeline.h"
#include "processing/image_pipeline.h"
#include "profiles/active_profile.h"

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

using namespace ixc;
using namespace ixc::camera;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kExitSkip = 77;

struct Options {
    enum class Mode { None, List, ListDirectShow, DirectShowCapture, Capture, Cycles, SourceTest, SourceEffectTest, BenchPipeline, BenchGpu, BenchGpuMemory, CameraControls, BenchFace, BenchEffects, ApplyEffects } mode = Mode::None;
    int seconds = 10;
    int cycles = 20;
    std::string camera;
    FormatRequest request{1920, 1080, 30};
    OutputFormat output = OutputFormat::Nv12;
    std::string subtype;  // restrict native mode selection, e.g. "MJPG"
    std::string sourceDll;  // --source-test
    int aePriority = -1;    // --set-ae-priority
    int exposure = 99;      // --set-exposure (99 = leave, 100 = auto)
    bool requireCamera = false;
    bool smoothMotion = false;
    bool noSmooth = false;
    std::string snapshot;  // --snapshot file.bmp (DirectShow capture): last frame, for visual comparison
    std::string applyIn, applyOut, applyEffects = "blush.tone";  // --apply-effects in.bmp out.bmp [--effects a,b] [--strength N]
    double applyStrength = 100;
};

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--list") o.mode = Options::Mode::List;
        else if (a == "--list-dshow") o.mode = Options::Mode::ListDirectShow;
        else if (a == "--dshow-capture") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::DirectShowCapture; o.seconds = std::atoi(v); }
        else if (a == "--bench-pipeline") o.mode = Options::Mode::BenchPipeline;
        else if (a == "--bench-gpu") o.mode = Options::Mode::BenchGpu;
        else if (a == "--camera-controls") o.mode = Options::Mode::CameraControls;
        else if (a == "--set-exposure") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::CameraControls; o.exposure = std::string(v) == "auto" ? 100 : std::atoi(v); }
        else if (a == "--set-ae-priority") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::CameraControls; o.aePriority = std::atoi(v); }
        else if (a == "--bench-gpu-memory") o.mode = Options::Mode::BenchGpuMemory;
        else if (a == "--bench-effects") o.mode = Options::Mode::BenchEffects;
        else if (a == "--apply-effects") { const char* src = next(); const char* dst = next(); if (!src || !dst) return false; o.mode = Options::Mode::ApplyEffects; o.applyIn = src; o.applyOut = dst; }
        else if (a == "--effects") { const char* v = next(); if (!v) return false; o.applyEffects = v; }
        else if (a == "--strength") { const char* v = next(); if (!v) return false; o.applyStrength = std::atof(v); }
        else if (a == "--bench-face") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::BenchFace; o.seconds = std::atoi(v); }
        else if (a == "--source-effect-test") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::SourceEffectTest; o.sourceDll = v; }
        else if (a == "--source-test") { const char* v = next(); if (!v) return false; o.mode = Options::Mode::SourceTest; o.sourceDll = v; }
        else if (a == "--seconds") { const char* v = next(); if (!v) return false; o.seconds = std::atoi(v); }
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
        else if (a == "--smooth-motion") o.smoothMotion = true;
        else if (a == "--no-smooth-motion") o.noSmooth = true;
        else if (a == "--snapshot") { const char* v = next(); if (!v) return false; o.snapshot = v; }
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
        std::printf("  [state] %s%s%s\n", ToString(s), FAILED(e.hr) ? " - " : "", FAILED(e.hr) ? e.Describe().c_str() : "");
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
    HANDLE FrameEvent() const { return event_; }  // auto-reset, signalled per new frame
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
        // Not present (e.g. IXC Camera not installed on a CI runner): a skip, unless required.
        std::printf("no camera matches \"%s\"\n", o.camera.c_str());
        exitCode = o.requireCamera ? 1 : kExitSkip;
        return false;
    }
    for (const auto& c : cams) {
        if (!c.isSoftwareDevice) { out = c; return true; }
    }
    std::printf("no physical camera found\n");
    exitCode = o.requireCamera ? 1 : kExitSkip;
    return false;
}

bool Resolve(const Options& o, CaptureConfig& cfg, int& exitCode);

// Loads the IXC media source DLL directly (no registration, no Frame Server), activates it
// against a physical camera through the symbolic-link fallback, and reads frames through it.
// This validates the source before it's ever loaded into the shared Windows camera service.
int SourceTest(const Options& o) {
    CaptureConfig cfg;
    int exitCode = 0;
    if (!Resolve(o, cfg, exitCode)) return exitCode;

    wchar_t fullPath[MAX_PATH];
    if (!GetFullPathNameW(Utf8ToWide(o.sourceDll).c_str(), MAX_PATH, fullPath, nullptr)) return 1;
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires an absolute path.
    HMODULE dll = LoadLibraryExW(fullPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        std::printf("error: cannot load %s (%lu)\n", o.sourceDll.c_str(), GetLastError());
        return 1;
    }
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    // {3011A045-BC7A-469D-86D0-2800938E32BF} and the physical-link attribute, as in vcam_ids.h
    const GUID clsid = {0x3011a045, 0xbc7a, 0x469d, {0x86, 0xd0, 0x28, 0x00, 0x93, 0x8e, 0x32, 0xbf}};
    const GUID attrLink = {0xcb5a6a96, 0x8cbe, 0x498b, {0x8b, 0x00, 0x95, 0x13, 0x88, 0x43, 0x43, 0xfb}};

    int rc = 1;
    {
        ComPtr<IClassFactory> factory;
        ComPtr<IMFActivate> activate;
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFSourceReader> reader;
        HRESULT hr = getClassObject ? getClassObject(clsid, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate));
        if (SUCCEEDED(hr)) hr = activate->SetString(attrLink, cfg.symbolicLink.c_str());
        if (SUCCEEDED(hr)) hr = activate->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr)) {
            std::printf("error: %s\n", Error{hr, "ActivateSource", "The IXC media source could not be activated."}.Describe().c_str());
        } else {
            ComPtr<IMFAttributes> ra;
            MFCreateAttributes(&ra, 1);
            ra->SetUINT32(MF_LOW_LATENCY, TRUE);
            hr = MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader);
            std::vector<CaptureFormat> exposed;
            for (DWORD i = 0; SUCCEEDED(hr) && i < 1024; ++i) {
                ComPtr<IMFMediaType> t;
                if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t))) break;
                CaptureFormat f;
                if (FormatFromMediaType(t.Get(), f)) exposed.push_back(f);
            }
            size_t nonNv12 = 0;
            for (const auto& f : exposed) if (!IsEqualGUID(f.subtype, MFVideoFormat_NV12)) ++nonNv12;
            const auto distinct = NormalizeFormats(exposed);
            std::printf("source exposes %zu media types (%zu distinct), %zu not NV12\n", exposed.size(), distinct.size(), nonNv12);
            for (size_t i = 0; i < distinct.size() && i < 6; ++i) std::printf("  %s\n", Describe(distinct[i]).c_str());
            if (distinct.size() > 6) std::printf("  ...\n");

            // Select the requested mode among the exposed ones and stream synchronously.
            std::optional<size_t> pick = SelectFormat(distinct, o.request);
            ComPtr<IMFMediaType> chosen;
            for (DWORD i = 0; pick && i < 1024; ++i) {
                ComPtr<IMFMediaType> t;
                if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t))) break;
                CaptureFormat f;
                if (FormatFromMediaType(t.Get(), f) && f.SameMode(distinct[*pick])) { chosen = t; break; }
            }
            if (chosen) hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, chosen.Get());
            else hr = MF_E_INVALIDMEDIATYPE;

            std::uint64_t frames = 0, errors = 0;
            double lumaMean = -1;
            LARGE_INTEGER f0, t0, t1;
            QueryPerformanceFrequency(&f0);
            QueryPerformanceCounter(&t0);
            t1 = t0;  // the loop may not run if mode selection failed
            const ProcessSample before = SampleProcess();
            while (SUCCEEDED(hr)) {
                QueryPerformanceCounter(&t1);
                if (static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f0.QuadPart) >= o.seconds) break;
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                const HRESULT rhr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &sample);
                if (FAILED(rhr) || (flags & MF_SOURCE_READERF_ERROR)) { ++errors; hr = FAILED(rhr) ? rhr : E_FAIL; break; }
                if (!sample) continue;
                ++frames;
                ComPtr<IMFMediaBuffer> buf;
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    const DWORD n = std::min<DWORD>(len, distinct[*pick].width * distinct[*pick].height);
                    std::uint64_t sum = 0, cnt = 0;
                    for (DWORD i = 0; i < n; i += 64) { sum += data[i]; ++cnt; }
                    if (cnt) lumaMean = static_cast<double>(sum) / static_cast<double>(cnt);
                    buf->Unlock();
                }
            }
            const ProcessSample after = SampleProcess();
            const double secs = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f0.QuadPart);
            if (pick) std::printf("streamed %s through the IXC source\n", Describe(distinct[*pick]).c_str());
            std::printf("frames: %llu in %.1f s (%.2f fps), errors: %llu, luma mean %.1f\n", static_cast<unsigned long long>(frames), secs,
                        secs > 0 ? static_cast<double>(frames) / secs : 0.0, static_cast<unsigned long long>(errors), lumaMean);
            std::printf("private bytes: %.1f MB -> %.1f MB\n", before.privateBytes / 1048576.0, after.privateBytes / 1048576.0);
            if (FAILED(hr)) std::printf("stream error: %s\n", Error{hr, "ReadSample", "Streaming through the IXC source failed."}.Describe().c_str());

            const bool pass = SUCCEEDED(hr) && frames > 0 && nonNv12 == 0 && !exposed.empty();
            std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
            rc = pass ? 0 : 1;
        }
        reader.Reset();
        if (source) source->Shutdown();
        if (activate) activate->ShutdownObject();
    }
    // Leave the DLL loaded: MF work queues may still reference it briefly (process exits next).
    return rc;
}

// Pure CPU benchmark of the image pipeline on synthetic NV12 frames (no camera needed).
// Reports ms per frame for representative setting combinations at common sizes.
int BenchPipeline() {
    struct Case {
        const char* name;
        void (*apply)(Profile&);
    };
    const Case cases[] = {
        {"neutral (pass-through)", [](Profile&) {}},
        {"colour only (LUTs)", [](Profile& p) { p.image.brightness = 10; p.image.contrast = 10; p.image.saturation = 10; p.image.temperature = 10; }},
        {"colour + mirror", [](Profile& p) { p.image.brightness = 10; p.mirror = true; }},
        {"colour + sharpen", [](Profile& p) { p.image.brightness = 10; p.image.sharpness = 40; }},
        {"colour + sharpen + zoom 1.5x", [](Profile& p) { p.image.brightness = 10; p.image.sharpness = 40; p.zoom = 1.5; }},
    };
    const int sizes[][2] = {{1280, 720}, {1920, 1080}};
    std::printf("%-30s %12s %12s\n", "pipeline", "720p ms", "1080p ms");
    for (const auto& c : cases) {
        double ms[2] = {0, 0};
        for (int si = 0; si < 2; ++si) {
            const int w = sizes[si][0], h = sizes[si][1];
            std::vector<std::uint8_t> src(static_cast<size_t>(w) * h * 3 / 2), dst(src.size());
            for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);  // texture-like noise
            Profile p;
            p.image.sharpness = 0;
            c.apply(p);
            const processing::PipelineParams params = processing::CompileParams(p, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), true);
            processing::Nv12Processor proc;
            const processing::Nv12Planes in{src.data(), src.data() + static_cast<size_t>(w) * h, w, w, w, h};
            const processing::Nv12Frame out{dst.data(), dst.data() + static_cast<size_t>(w) * h, w, w, w, h};
            if (params.identity) { ms[si] = 0; continue; }
            proc.Process(in, out, params);  // warm-up (builds tables)
            const int iters = 60;
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            for (int i = 0; i < iters; ++i) proc.Process(in, out, params);
            QueryPerformanceCounter(&b);
            ms[si] = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / iters;
        }
        std::printf("%-30s %12.2f %12.2f\n", c.name, ms[0], ms[1]);
    }
    std::printf("(single thread; at 30 FPS the frame budget is 33.3 ms)\n");
    return 0;
}

// CPU vs GPU (Direct3D 11) pipeline benchmark on synthetic frames, including the costs that
// matter on weak PCs: CPU time the GPU path still consumes (upload, driver, readback) and the
// memory a GPU device adds. Also verifies both paths produce identical bytes.
// Memory of the GPU path step by step (device, resources, steady state), 1080p default profile.
int BenchGpuMemory() {
    auto mb = [](const ProcessSample& s) { return static_cast<double>(s.privateBytes) / 1048576.0; };
    const int w = 1920, h = 1080;
    std::vector<std::uint8_t> src(static_cast<size_t>(w) * h * 3 / 2, 100), dst(src.size());
    const ProcessSample s0 = SampleProcess();
    processing::GpuNv12Processor gpu;
    if (FAILED(gpu.Initialize({}))) { std::printf("no GPU\n"); return 0; }
    const ProcessSample s1 = SampleProcess();
    Profile p;
    const processing::PipelineParams params = processing::CompileParams(p, w, h, true);
    const processing::Nv12Planes in{src.data(), src.data() + static_cast<size_t>(w) * h, w, w, w, h};
    const processing::Nv12Frame out{dst.data(), dst.data() + static_cast<size_t>(w) * h, w, w, w, h};
    gpu.Process(in, out, params);
    const ProcessSample s2 = SampleProcess();
    for (int i = 0; i < 300; ++i) gpu.Process(in, out, params);
    const ProcessSample s3 = SampleProcess();
    for (int i = 0; i < 300; ++i) gpu.Process(in, out, params);
    const ProcessSample s4 = SampleProcess();
    gpu.Release();
    const ProcessSample s5 = SampleProcess();
    std::printf("private MB: start %.1f | device %.1f (+%.1f) | first 1080p frame %.1f (+%.1f) | +300 frames %.1f | +600 frames %.1f | released %.1f\n",
                mb(s0), mb(s1), mb(s1) - mb(s0), mb(s2), mb(s2) - mb(s1), mb(s3), mb(s4), mb(s5));
    return 0;
}

int BenchGpu() {
    struct Case {
        const char* name;
        void (*apply)(Profile&);
    };
    const Case cases[] = {
        {"colour only", [](Profile& p) { p.image.brightness = 10; p.image.contrast = 10; p.image.saturation = 10; }},
        {"default (subtle sharpen)", [](Profile& p) { p.image.sharpness = 15; }},
        {"colour + sharpen 40", [](Profile& p) { p.image.brightness = 10; p.image.sharpness = 40; }},
        {"zoom 1.5 + sharpen 40", [](Profile& p) { p.image.brightness = 10; p.image.sharpness = 40; p.zoom = 1.5; }},
    };
    auto cpuSeconds = [] {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        return static_cast<double>(Ft(k) + Ft(u)) / 1e7;
    };
    auto now = [] { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart); };

    const ProcessSample m0 = SampleProcess();
    processing::GpuNv12Processor gpu;
    const HRESULT ghr = gpu.Initialize({});
    const ProcessSample m1 = SampleProcess();
    if (FAILED(ghr)) {
        std::printf("no usable Direct3D 11 GPU (%s): the CPU path is the only option on this PC\n", HResultHex(ghr).c_str());
        return 0;
    }
    std::printf("GPU: %ls\n", gpu.AdapterName().c_str());
    std::printf("memory: GPU device + shaders add %.1f MB private bytes (%.1f MB working set)\n",
                (static_cast<double>(m1.privateBytes) - static_cast<double>(m0.privateBytes)) / 1048576.0,
                (static_cast<double>(m1.workingSet) - static_cast<double>(m0.workingSet)) / 1048576.0);

    std::printf("\n%-26s %6s | %9s | %9s %9s %8s %8s %8s | %s\n", "settings", "size", "CPU ms", "GPU wall", "GPU cpu", "upload", "gpu", "readbk", "identical");
    const int sizes[][2] = {{1280, 720}, {1920, 1080}};
    for (const auto& c : cases) {
        for (const auto& sz : sizes) {
            const int w = sz[0], h = sz[1];
            std::vector<std::uint8_t> src(static_cast<size_t>(w) * h * 3 / 2), a(src.size()), b(src.size());
            std::uint32_t seed = 99;
            for (size_t i = 0; i < src.size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                // smooth gradient + edges + mild noise: closer to a real frame than pure noise
                const size_t x = i % static_cast<size_t>(w), y = i / static_cast<size_t>(w);
                src[i] = static_cast<std::uint8_t>((x * 180 / static_cast<size_t>(w) + ((x / 40 + y / 30) % 4 == 0 ? 50 : 0) + (seed >> 30)) & 0xFF);
            }
            Profile p;
            p.image.sharpness = 0;
            c.apply(p);
            const processing::PipelineParams params = processing::CompileParams(p, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), true);
            const processing::Nv12Planes in{src.data(), src.data() + static_cast<size_t>(w) * h, w, w, w, h};
            const processing::Nv12Frame outA{a.data(), a.data() + static_cast<size_t>(w) * h, w, w, w, h};
            const processing::Nv12Frame outB{b.data(), b.data() + static_cast<size_t>(w) * h, w, w, w, h};

            processing::Nv12Processor cpu;
            cpu.Process(in, outA, params);
            gpu.Process(in, outB, params);  // warm-up: resources, tables, shader caches
            const bool identical = a == b;

            const int iters = 100;
            double t = now();
            for (int i = 0; i < iters; ++i) cpu.Process(in, outA, params);
            const double cpuMs = (now() - t) * 1000.0 / iters;

            // Process CPU time has 15.6 ms granularity: 600 frames keeps the error below ~0.03 ms.
            const int gpuIters = 600;
            processing::GpuNv12Processor::Timing tm, sum;
            const double c0 = cpuSeconds();
            t = now();
            for (int i = 0; i < gpuIters; ++i) {
                gpu.Process(in, outB, params, &tm);
                sum.uploadMs += tm.uploadMs;
                sum.gpuMs += tm.gpuMs;
                sum.readbackMs += tm.readbackMs;
            }
            const double wallMs = (now() - t) * 1000.0 / gpuIters;
            const double gpuCpuMs = (cpuSeconds() - c0) * 1000.0 / gpuIters;
            std::printf("%-26s %4dp | %9.2f | %9.2f %9.2f %8.2f %8.2f %8.2f | %s\n", c.name, h, cpuMs, wallMs, gpuCpuMs, sum.uploadMs / gpuIters,
                        sum.gpuMs / gpuIters, sum.readbackMs / gpuIters, identical ? "yes" : "NO");
        }
    }
    const ProcessSample m2 = SampleProcess();
    std::printf("\nmemory after 1080p GPU resources: +%.1f MB private bytes over no GPU\n",
                (static_cast<double>(m2.privateBytes) - static_cast<double>(m0.privateBytes)) / 1048576.0);
    std::printf("CPU ms = CPU path (single thread). GPU wall = upload + dispatch + readback, synchronous.\n"
                "GPU cpu = CPU time the GPU path consumed per frame (all threads of this process).\n");
    return 0;
}

// Verifies the IXC image pipeline inside the real source DLL, including live settings reload:
// streams with neutral settings, publishes a brighter profile mid-stream, and checks the
// delivered frames change. %ProgramData% is redirected to a temp folder so the user's real
// settings are never touched.
int SourceEffectTest(const Options& o) {
    CaptureConfig cfg;
    int exitCode = 0;
    if (!Resolve(o, cfg, exitCode)) return exitCode;

    wchar_t tempBuf[MAX_PATH];
    GetTempPathW(MAX_PATH, tempBuf);
    const std::filesystem::path fakeProgramData = std::filesystem::path(tempBuf) / L"ixc-effect-test";
    std::filesystem::create_directories(fakeProgramData / L"IXC Camera");
    SetEnvironmentVariableW(L"ProgramData", fakeProgramData.c_str());

    Profile neutral;
    neutral.image.sharpness = 0;
    Profile bright = neutral;
    bright.image.brightness = 60;
    bright.image.contrast = 10;
    if (FAILED(PublishActiveProfile(neutral))) { std::printf("error: cannot write test settings\n"); return 1; }

    wchar_t fullPath[MAX_PATH];
    if (!GetFullPathNameW(Utf8ToWide(o.sourceDll).c_str(), MAX_PATH, fullPath, nullptr)) return 1;
    HMODULE dll = LoadLibraryExW(fullPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject")) : nullptr;
    if (!getClassObject) { std::printf("error: cannot load %s\n", o.sourceDll.c_str()); return 1; }
    const GUID clsid = {0x3011a045, 0xbc7a, 0x469d, {0x86, 0xd0, 0x28, 0x00, 0x93, 0x8e, 0x32, 0xbf}};
    const GUID attrLink = {0xcb5a6a96, 0x8cbe, 0x498b, {0x8b, 0x00, 0x95, 0x13, 0x88, 0x43, 0x43, 0xfb}};

    ComPtr<IClassFactory> factory;
    ComPtr<IMFActivate> activate;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = getClassObject(clsid, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate));
    if (SUCCEEDED(hr)) hr = activate->SetString(attrLink, cfg.symbolicLink.c_str());
    if (SUCCEEDED(hr)) hr = activate->ActivateObject(IID_PPV_ARGS(&source));
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    // Pick the requested size among the source's (NV12) modes.
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0; SUCCEEDED(hr) && i < 1024; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t))) break;
        CaptureFormat f;
        if (FormatFromMediaType(t.Get(), f) && f.width == cfg.format.width && f.height == cfg.format.height &&
            std::abs(f.Fps() - cfg.format.Fps()) < 0.5) { chosen = t; break; }
    }
    if (SUCCEEDED(hr)) hr = chosen ? reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, chosen.Get()) : MF_E_INVALIDMEDIATYPE;
    if (FAILED(hr)) {
        std::printf("error: %s\n", Error{hr, "OpenSource", "The IXC source could not be opened for the effect test."}.Describe().c_str());
        return 1;
    }

    // Reads frames for `seconds`, returning mean luma and CPU used by this process.
    auto measure = [&](double seconds, double& meanLuma, double& cpuPct, std::uint64_t& frames) {
        frames = 0;
        double sum = 0;
        const ProcessSample p0 = SampleProcess();
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        b = a;
        while (static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) < seconds) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &s)) ||
                (flags & MF_SOURCE_READERF_ERROR)) break;
            QueryPerformanceCounter(&b);
            if (!s) continue;
            ComPtr<IMFMediaBuffer> buf;
            BYTE* d = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&d, nullptr, &len))) {
                const DWORD n = std::min<DWORD>(len, cfg.format.width * cfg.format.height);
                std::uint64_t acc = 0, cnt = 0;
                for (DWORD i = 0; i < n; i += 97) { acc += d[i]; ++cnt; }
                if (cnt) sum += static_cast<double>(acc) / static_cast<double>(cnt);
                buf->Unlock();
                ++frames;
            }
        }
        const ProcessSample p1 = SampleProcess();
        const double wall = static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
        meanLuma = frames ? sum / static_cast<double>(frames) : -1;
        cpuPct = wall > 0 ? 100.0 * static_cast<double>(p1.cpu100ns - p0.cpu100ns) / 1e7 / wall : 0;
    };

    double warmL = 0, warmC = 0, nL = 0, nC = 0, bL = 0, bC = 0;
    std::uint64_t warmF = 0, nF = 0, bF = 0;
    measure(1.5, warmL, warmC, warmF);  // settle auto exposure
    measure(3.0, nL, nC, nF);
    PublishActiveProfile(bright);        // live change while streaming
    measure(0.7, warmL, warmC, warmF);   // allow the watcher to pick it up
    measure(3.0, bL, bC, bF);
    PublishActiveProfile(neutral);

    reader.Reset();
    source->Shutdown();
    activate->ShutdownObject();
    std::filesystem::remove_all(fakeProgramData);

    std::printf("mode: %s through the IXC source\n", Describe(cfg.format).c_str());
    std::printf("neutral settings:  %llu frames, mean luma %.1f, cpu %.1f%% of one core\n", static_cast<unsigned long long>(nF), nL, nC);
    std::printf("brightness +60:    %llu frames, mean luma %.1f, cpu %.1f%% of one core\n", static_cast<unsigned long long>(bF), bL, bC);
    const bool pass = nF > 0 && bF > 0 && bL > nL + 15;
    std::printf("RESULT: %s%s\n", pass ? "PASS" : "FAIL", pass ? "" : " (processed frames did not get brighter)");
    return pass ? 0 : 1;
}

// ---- DirectShow capture test (how OBS "Video Capture Device" and older apps read cameras) ------
// qedit.h is no longer in the SDK; the Sample Grabber interfaces are declared here. The filter
// itself still ships with Windows (qedit.dll).
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double time, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double time, BYTE* buffer, long length) = 0;
};
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long whichMethod) = 0;
};
constexpr CLSID kClsidSampleGrabber = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
constexpr CLSID kClsidNullRenderer = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

class GrabberCounter final : public ISampleGrabberCB {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISampleGrabberCB)) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // stack object; lifetime managed by the caller
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double sampleTime, IMediaSample* s) override {
        // Timing record per frame (fixed array: no allocation in the callback).
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const std::uint64_t slot = frames.load();
        if (slot < kMaxRecords) {
            arrivalQpc[slot] = now.QuadPart;
            stamp[slot] = sampleTime;
        }
        ++frames;
        BYTE* p = nullptr;
        if (s && SUCCEEDED(s->GetPointer(&p)) && p) {
            const long n = s->GetActualDataLength();
            std::uint64_t sum = 0, cnt = 0;
            for (long i = 0; i < n; i += 64) { sum += p[i]; ++cnt; }
            if (cnt) meanByte = static_cast<double>(sum) / static_cast<double>(cnt);
            if (keepLast) {  // --snapshot (diagnostic tool): keep a copy of the newest frame
                std::lock_guard lock(lastMu);
                last.assign(p, p + n);
            }
        }
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE*, long) override { return S_OK; }
    static constexpr std::uint64_t kMaxRecords = 4096;
    std::atomic<std::uint64_t> frames{0};
    std::atomic<double> meanByte{-1};
    bool keepLast = false;
    std::mutex lastMu;
    std::vector<BYTE> last;
    std::vector<long long> arrivalQpc = std::vector<long long>(kMaxRecords);
    std::vector<double> stamp = std::vector<double>(kMaxRecords);
};

// Frame-timing statistics shared by the DirectShow and Media Foundation smoothness reports.
struct TimingReport {
    double fps = 0, meanMs = 0, jitterMs = 0, maxGapMs = 0, p99Ms = 0;
    int longGaps = 0;          // intervals > 1.5x the nominal frame time
    int stampBackwards = 0;    // timestamps not increasing
    double queueSpreadMs = 0;  // max-min of (arrival - timestamp): variation caused by buffering
};

TimingReport AnalyzeTiming(const std::vector<double>& arrivalMs, const std::vector<double>& stampMs, double nominalMs) {
    TimingReport r;
    const size_t n = arrivalMs.size();
    if (n < 3) return r;
    std::vector<double> iv;
    for (size_t i = 1; i < n; ++i) iv.push_back(arrivalMs[i] - arrivalMs[i - 1]);
    double sum = 0, sq = 0;
    for (double v : iv) { sum += v; sq += v * v; r.maxGapMs = std::max(r.maxGapMs, v); if (v > nominalMs * 1.5) ++r.longGaps; }
    r.meanMs = sum / iv.size();
    r.jitterMs = std::sqrt(std::max(0.0, sq / iv.size() - r.meanMs * r.meanMs));
    r.fps = 1000.0 / r.meanMs;
    std::vector<double> sorted = iv;
    std::sort(sorted.begin(), sorted.end());
    r.p99Ms = sorted[std::min(sorted.size() - 1, static_cast<size_t>(sorted.size() * 0.99))];
    double mn = 1e18, mx = -1e18;
    for (size_t i = 0; i < n; ++i) {
        if (i && stampMs[i] <= stampMs[i - 1]) ++r.stampBackwards;
        const double d = arrivalMs[i] - stampMs[i];
        mn = std::min(mn, d);
        mx = std::max(mx, d);
    }
    r.queueSpreadMs = mx - mn;
    return r;
}

void PrintTiming(const char* label, const TimingReport& r, double nominalFps) {
    std::printf("%s: %.2f fps (nominal %.0f) | interval mean %.1f ms, jitter %.1f ms, p99 %.1f ms, max %.1f ms | long gaps %d | "
                "timestamps not increasing %d | arrival-vs-timestamp spread %.1f ms\n",
                label, r.fps, nominalFps, r.meanMs, r.jitterMs, r.p99Ms, r.maxGapMs, r.longGaps, r.stampBackwards, r.queueSpreadMs);
}

int DirectShowCapture(const Options& o) {
    const std::string want = o.camera.empty() ? "IXC Camera" : o.camera;
    ComPtr<ICreateDevEnum> devEnum;
    ComPtr<IEnumMoniker> monikers;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (SUCCEEDED(hr)) hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0);
    if (hr != S_OK) {
        // No video devices at all (e.g. a CI runner): a skip, like the Media Foundation tests.
        std::printf("no DirectShow video devices\n");
        return o.requireCamera ? 1 : kExitSkip;
    }

    ComPtr<IBaseFilter> source;
    std::string name;
    ComPtr<IMoniker> m;
    while (!source && monikers->Next(1, &m, nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) &&
            v.vt == VT_BSTR && WideToUtf8(v.bstrVal).find(want) != std::string::npos) {
            name = WideToUtf8(v.bstrVal);
            hr = m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source));
            if (FAILED(hr)) std::printf("error: %s\n", Error{hr, "BindCaptureFilter", "Could not create the DirectShow filter."}.Describe().c_str());
        }
        VariantClear(&v);
        m.Reset();
    }
    if (!source) {
        if (name.empty()) {  // not present at all: skip unless required
            std::printf("DirectShow device \"%s\" not found\n", want.c_str());
            return o.requireCamera ? 1 : kExitSkip;
        }
        return 1;  // present but the filter couldn't be created (error printed above)
    }

    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    ComPtr<IBaseFilter> grabberFilter, nullRenderer;
    ComPtr<ISampleGrabber> grabber;
    ComPtr<IMediaControl> control;
    GrabberCounter counter;
    const char* stage = "CreateGraph";
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder));
    if (SUCCEEDED(hr)) hr = builder->SetFiltergraph(graph.Get());
    if (SUCCEEDED(hr)) { stage = "AddFilters"; hr = graph->AddFilter(source.Get(), L"Camera"); }
    if (SUCCEEDED(hr)) hr = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberFilter));
    if (SUCCEEDED(hr)) hr = grabberFilter.As(&grabber);
    if (SUCCEEDED(hr)) {
        AM_MEDIA_TYPE mt{};
        mt.majortype = MEDIATYPE_Video;
        if (!o.snapshot.empty()) {  // RGB24 so the frame can be written as a BMP
            mt.subtype = MEDIASUBTYPE_RGB24;
            mt.formattype = FORMAT_VideoInfo;
            counter.keepLast = true;
        }
        hr = grabber->SetMediaType(&mt);
    }
    if (SUCCEEDED(hr)) hr = grabber->SetCallback(&counter, 0);
    if (SUCCEEDED(hr)) hr = graph->AddFilter(grabberFilter.Get(), L"Grabber");
    if (SUCCEEDED(hr)) hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullRenderer));
    if (SUCCEEDED(hr)) hr = graph->AddFilter(nullRenderer.Get(), L"Null");
    // Force the requested format (like choosing Resolution/FPS/Video Format in OBS), so the
    // physical camera and IXC Camera are compared like for like.
    double nominalFps = 0;
    if (SUCCEEDED(hr) && o.request.width) {
        ComPtr<IAMStreamConfig> cfg;
        stage = "SetFormat";
        hr = builder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source.Get(), IID_PPV_ARGS(&cfg));
        int count = 0, size = 0;
        if (SUCCEEDED(hr)) hr = cfg->GetNumberOfCapabilities(&count, &size);
        bool set = false;
        for (int i = 0; SUCCEEDED(hr) && i < count && !set; ++i) {
            AM_MEDIA_TYPE* mt = nullptr;
            VIDEO_STREAM_CONFIG_CAPS caps{};
            if (FAILED(cfg->GetStreamCaps(i, &mt, reinterpret_cast<BYTE*>(&caps))) || !mt) continue;
            if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
                const bool sizeOk = vih->bmiHeader.biWidth == static_cast<LONG>(o.request.width) &&
                                    std::labs(vih->bmiHeader.biHeight) == static_cast<LONG>(o.request.height);
                const bool subOk = o.subtype.empty() || SubtypeName(mt->subtype) == o.subtype;
                const REFERENCE_TIME interval = static_cast<REFERENCE_TIME>(10'000'000.0 / o.request.fps);
                const bool fpsOk = interval >= caps.MinFrameInterval && interval <= caps.MaxFrameInterval;
                if (sizeOk && subOk && fpsOk) {
                    vih->AvgTimePerFrame = interval;
                    set = SUCCEEDED(cfg->SetFormat(mt));
                    nominalFps = o.request.fps;
                }
            }
            if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
            if (mt->pUnk) mt->pUnk->Release();
            CoTaskMemFree(mt);
        }
        if (SUCCEEDED(hr) && !set) {
            std::printf("camera: %s\nerror: format %ux%u @ %.0f %s not offered by DirectShow\nRESULT: FAIL\n", name.c_str(), o.request.width,
                        o.request.height, o.request.fps, o.subtype.c_str());
            return 1;
        }
    }
    if (SUCCEEDED(hr)) { stage = "RenderStream"; hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source.Get(), grabberFilter.Get(), nullRenderer.Get()); }
    if (SUCCEEDED(hr)) hr = graph.As(&control);
    if (SUCCEEDED(hr)) { stage = "Run"; hr = control->Run(); }
    if (FAILED(hr)) {
        std::printf("camera: %s\nerror: %s\nRESULT: FAIL\n", name.c_str(), Error{hr, stage, "DirectShow capture failed."}.Describe().c_str());
        if (control) control->Stop();
        return 1;
    }

    long snapW = 0, snapH = 0;
    AM_MEDIA_TYPE connected{};
    std::string fmt = "unknown";
    if (SUCCEEDED(grabber->GetConnectedMediaType(&connected))) {
        if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat && connected.cbFormat >= sizeof(VIDEOINFOHEADER)) {
            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(connected.pbFormat);
            snapW = vih->bmiHeader.biWidth;
            snapH = std::labs(vih->bmiHeader.biHeight);
            char buf[128];
            const double connectedFps = vih->AvgTimePerFrame ? 10'000'000.0 / static_cast<double>(vih->AvgTimePerFrame) : 0;
            std::snprintf(buf, sizeof(buf), "%ldx%ld %s @ %.2f fps%s", vih->bmiHeader.biWidth, std::labs(vih->bmiHeader.biHeight),
                          SubtypeName(connected.subtype).c_str(), connectedFps, o.request.width ? "" : " (device default)");
            fmt = buf;
            if (nominalFps == 0 && connectedFps > 0) nominalFps = connectedFps;
        }
        if (connected.cbFormat) CoTaskMemFree(connected.pbFormat);
        if (connected.pUnk) connected.pUnk->Release();
    }
    const ProcessSample p0 = SampleProcess();
    Sleep(static_cast<DWORD>(o.seconds) * 1000);
    control->Stop();
    if (!o.snapshot.empty()) std::printf("snapshot: frame %ldx%ld, buffer %zu bytes\n", snapW, snapH, counter.last.size());
    if (!o.snapshot.empty() && snapW > 0 && snapH > 0) {
        std::lock_guard lock(counter.lastMu);
        const size_t rowBytes = (static_cast<size_t>(snapW) * 3 + 3) & ~size_t{3};
        if (counter.last.size() >= rowBytes * static_cast<size_t>(snapH)) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = snapW;
            ih.biHeight = snapH;  // DirectShow RGB24 is bottom-up, like a BMP
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            ih.biCompression = BI_RGB;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + static_cast<DWORD>(rowBytes * snapH);
            FILE* bf = nullptr;
            if (fopen_s(&bf, o.snapshot.c_str(), "wb") == 0 && bf) {
                fwrite(&fh, sizeof(fh), 1, bf);
                fwrite(&ih, sizeof(ih), 1, bf);
                fwrite(counter.last.data(), 1, rowBytes * snapH, bf);
                fclose(bf);
                std::printf("snapshot: %s (%ldx%ld)\n", o.snapshot.c_str(), snapW, snapH);
            }
        }
    }
    const ProcessSample p1 = SampleProcess();
    const auto frames = counter.frames.load();
    std::printf("camera: %s (DirectShow)\nconnected format: %s\nframes: %llu in %d s (%.2f fps), sample value mean %.1f\n", name.c_str(),
                fmt.c_str(), static_cast<unsigned long long>(frames), o.seconds, static_cast<double>(frames) / o.seconds,
                counter.meanByte.load());

    // Skip the first second (start-up), then analyse arrival times and timestamps.
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const size_t n = static_cast<size_t>(std::min<std::uint64_t>(frames, GrabberCounter::kMaxRecords));
    std::vector<double> arrival, stamps;
    for (size_t i = 0; i < n; ++i) {
        const double a = 1000.0 * static_cast<double>(counter.arrivalQpc[i] - counter.arrivalQpc[0]) / static_cast<double>(qf.QuadPart);
        if (a < 1000.0) continue;
        arrival.push_back(a);
        stamps.push_back(counter.stamp[i] * 1000.0);
    }
    const double nominal = nominalFps > 0 ? nominalFps : 30.0;
    PrintTiming("timing", AnalyzeTiming(arrival, stamps, 1000.0 / nominal), nominal);
    // Interval histogram (10 ms buckets): an uneven cadence looks choppy even with a good average.
    int hist[12] = {};
    for (size_t i = 1; i < arrival.size(); ++i) hist[std::min(11, static_cast<int>((arrival[i] - arrival[i - 1]) / 10.0))]++;
    std::printf("interval histogram:");
    for (int b = 0; b < 12; ++b) if (hist[b]) std::printf(" %d-%dms:%d", b * 10, b * 10 + 9, hist[b]);
    std::printf("\n");
    std::printf("process: cpu %.1f%% of one core, private %.1f MB\n",
                100.0 * static_cast<double>(p1.cpu100ns - p0.cpu100ns) / 1e7 / o.seconds, p1.privateBytes / 1048576.0);
    std::printf("RESULT: %s\n", frames > 0 ? "PASS" : "FAIL");
    return frames > 0 ? 0 : 1;
}

// Reads (and with --set-ae-priority 0|1 temporarily sets) the camera's UVC exposure controls.
// "Auto-exposure priority" lets a webcam lower its frame rate in low light; that alone can turn
// 30 FPS into 15-20 FPS.
int CameraControls(const Options& o) {
    CameraInfo cam;
    int exitCode = 0;
    if (!PickCamera(o, cam, exitCode)) return exitCode;
    ComPtr<IMFMediaSource> source;
    HRESULT hr = CreateCameraSource(cam.symbolicLink, source);
    ComPtr<IKsControl> ks;
    if (SUCCEEDED(hr)) hr = source.As(&ks);
    if (FAILED(hr)) { std::printf("error: %s\n", Error{hr, "OpenControls", "Camera controls unavailable."}.Describe().c_str()); return 1; }
    auto get = [&](ULONG id, LONG& value, ULONG& flags) {
        KSPROPERTY_CAMERACONTROL_S s{};
        s.Property.Set = PROPSETID_VIDCAP_CAMERACONTROL;
        s.Property.Id = id;
        s.Property.Flags = KSPROPERTY_TYPE_GET;
        ULONG ret = 0;
        const HRESULT h = ks->KsProperty(&s.Property, sizeof(s), &s, sizeof(s), &ret);
        value = s.Value;
        flags = s.Flags;
        return h;
    };
    auto set = [&](ULONG id, LONG value, ULONG flags) {
        KSPROPERTY_CAMERACONTROL_S s{};
        s.Property.Set = PROPSETID_VIDCAP_CAMERACONTROL;
        s.Property.Id = id;
        s.Property.Flags = KSPROPERTY_TYPE_SET;
        s.Value = value;
        s.Flags = flags;
        ULONG ret = 0;
        return ks->KsProperty(&s.Property, sizeof(s), &s, sizeof(s), &ret);
    };
    LONG v = 0;
    ULONG f = 0;
    HRESULT h = get(KSPROPERTY_CAMERACONTROL_AUTO_EXPOSURE_PRIORITY, v, f);
    std::printf("camera: %s\nauto-exposure priority: %s\n", cam.name.c_str(),
                SUCCEEDED(h) ? (v ? "ON (camera may lower FPS in low light)" : "OFF (constant frame rate)") : ("not supported (" + HResultHex(h) + ")").c_str());
    h = get(KSPROPERTY_CAMERACONTROL_EXPOSURE, v, f);
    if (SUCCEEDED(h)) std::printf("exposure: value %ld, mode %s\n", v, (f & KSPROPERTY_CAMERACONTROL_FLAGS_AUTO) ? "auto" : "manual");
    if (o.exposure != 99) {
        // 100 = back to automatic exposure; otherwise a fixed log2-seconds value (e.g. -7 = 1/128 s).
        h = o.exposure == 100 ? set(KSPROPERTY_CAMERACONTROL_EXPOSURE, v, KSPROPERTY_CAMERACONTROL_FLAGS_AUTO)
                              : set(KSPROPERTY_CAMERACONTROL_EXPOSURE, o.exposure, KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL);
        std::printf("set exposure %s: %s\n", o.exposure == 100 ? "auto" : std::to_string(o.exposure).c_str(), SUCCEEDED(h) ? "ok" : HResultHex(h).c_str());
    }
    if (o.aePriority >= 0) {
        h = set(KSPROPERTY_CAMERACONTROL_AUTO_EXPOSURE_PRIORITY, o.aePriority, KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL);
        std::printf("set auto-exposure priority %d: %s\n", o.aePriority, SUCCEEDED(h) ? "ok" : HResultHex(h).c_str());
    }
    source->Shutdown();
    return 0;
}

// Video capture devices as DirectShow applications (OBS, many conferencing apps) see them.
int ListDirectShow() {
    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) {
        std::printf("error: %s\n", Error{hr, "CreateDevEnum", "DirectShow device enumeration unavailable."}.Describe().c_str());
        return 1;
    }
    ComPtr<IEnumMoniker> monikers;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0);
    int n = 0;
    std::printf("DirectShow video input devices:\n");
    if (hr == S_OK) {
        ComPtr<IMoniker> m;
        while (monikers->Next(1, &m, nullptr) == S_OK) {
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                VARIANT name, path;
                VariantInit(&name);
                VariantInit(&path);
                bag->Read(L"FriendlyName", &name, nullptr);
                bag->Read(L"DevicePath", &path, nullptr);
                std::printf("  [%d] %s\n      %s\n", n, name.vt == VT_BSTR ? WideToUtf8(name.bstrVal).c_str() : "?",
                            path.vt == VT_BSTR ? WideToUtf8(path.bstrVal).c_str() : "(no device path)");
                VariantClear(&name);
                VariantClear(&path);
            }
            ++n;
            m.Reset();
        }
    }
    std::printf("total: %d\n", n);
    return 0;
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

// ---- face tracking benchmark ------------------------------------------------------------------------
// Runs the real FaceEngine on live camera frames (kept in memory only, never saved), once per
// detector build, and reports what matters for low-end PCs: detection rate and cost, frame-thread
// cost, detection success and landmark validity, CPU and RAM.

// Locks an NV12 sample and exposes its planes; unlocks on destruction.
class LockedNv12 {
public:
    LockedNv12(IMFSample* s, const FrameLayout& l) {
        if (FAILED(s->GetBufferByIndex(0, &buf_))) return;
        BYTE* start = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buf_.As(&b2_)) && SUCCEEDED(b2_->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0_, &pitch_, &start, &length))) {
            locked2d_ = true;
        } else if (SUCCEEDED(buf_->Lock(&start, nullptr, &length))) {
            scan0_ = start;
            pitch_ = l.stride > 0 ? l.stride : static_cast<LONG>(l.width);
        } else {
            return;
        }
        locked_ = true;
        const size_t used = length - static_cast<size_t>(scan0_ - start);
        const size_t rows = pitch_ > 0 ? used / static_cast<size_t>(pitch_) * 2 / 3 : 0;
        if (pitch_ <= 0 || rows < l.height) return;
        planes_ = {scan0_, scan0_ + rows * static_cast<size_t>(pitch_), pitch_, pitch_, static_cast<int>(l.width & ~1u),
                   static_cast<int>(l.height & ~1u)};
        ok_ = true;
    }
    ~LockedNv12() {
        if (!locked_) return;
        if (locked2d_) b2_->Unlock2D();
        else buf_->Unlock();
    }
    bool ok() const { return ok_; }
    const processing::Nv12Planes& planes() const { return planes_; }

private:
    ComPtr<IMFMediaBuffer> buf_;
    ComPtr<IMF2DBuffer2> b2_;
    BYTE* scan0_ = nullptr;
    LONG pitch_ = 0;
    bool locked_ = false, locked2d_ = false, ok_ = false;
    processing::Nv12Planes planes_{};
};

// Full area-average reference downscale (what the Phase 7 detector benchmark used).
void AreaAverageBgr(const processing::Nv12Planes& p, const processing::YuvFormat& fmt, std::uint8_t* dst, int w, int h) {
    for (int oy = 0; oy < h; ++oy) {
        const int y0 = oy * p.height / h, y1 = (oy + 1) * p.height / h;
        for (int ox = 0; ox < w; ++ox) {
            const int x0 = ox * p.width / w, x1 = (ox + 1) * p.width / w;
            int sb = 0, sg = 0, sr = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::uint8_t* c = p.uv + static_cast<ptrdiff_t>(y / 2) * p.uvStride + (x / 2) * 2;
                    const std::uint32_t v = processing::YuvToBgra(p.y[static_cast<ptrdiff_t>(y) * p.yStride + x], c[0], c[1], fmt);
                    sb += v & 0xFF;
                    sg += (v >> 8) & 0xFF;
                    sr += (v >> 16) & 0xFF;
                    ++n;
                }
            }
            std::uint8_t* o = dst + (static_cast<size_t>(oy) * w + ox) * 3;
            o[0] = static_cast<std::uint8_t>(sb / n);
            o[1] = static_cast<std::uint8_t>(sg / n);
            o[2] = static_cast<std::uint8_t>(sr / n);
        }
    }
}

int BenchFace(const Options& o) {
    CaptureConfig cfg;
    int exitCode = 0;
    if (!Resolve(o, cfg, exitCode)) return exitCode;
    cfg.output = OutputFormat::Nv12;
    cfg.smoothMotion = !o.noSmooth;  // default: the camera's real rate, as IXC Camera delivers it
    if (!face::DetectorAvailable()) {
        std::printf("face tracking is not included in this build (IXC_WITH_FACE_TRACKING=OFF)\n");
        return 77;
    }

    Consumer consumer;  // used only for its frame event
    ComPtr<CaptureSession> session;
    if (FAILED(CaptureSession::Create(&consumer, session))) return 1;
    const Error err = session->Start(cfg);
    if (FAILED(err.hr)) {
        std::printf("error: %s\n", err.Describe().c_str());
        session->Close();
        return 1;
    }
    const FrameLayout layout = session->Layout();
    processing::YuvFormat fmt = processing::DefaultYuvFormat(layout.height);
    if (layout.nominalRange == MFNominalRange_0_255) fmt.fullRange = true;
    const auto active = session->ActiveFormat();
    std::printf("camera: %s  %s -> NV12 %ux%u  (CPU AVX2: %s)\n", cfg.cameraName.c_str(), Describe(active).c_str(), layout.width,
                layout.height, face::BestSimdPath() == face::SimdPath::Avx2 ? "yes" : "no");
    Sleep(4000);  // Smooth motion settles, camera exposure settles

    // Takes the newest frame (waits up to 1 s) and runs f(planes).
    const HANDLE ev = consumer.FrameEvent();
    auto withFrame = [&](auto&& f) {
        for (int tries = 0; tries < 50; ++tries) {
            auto frame = session->TakeFrame();
            if (frame) {
                LockedNv12 lk(frame->Get(), layout);
                if (lk.ok()) {
                    f(lk.planes());
                    return true;
                }
            }
            WaitForSingleObject(ev, 20);
        }
        return false;
    };

    // 1) Detector input check, same detector, 10 frames: the engine's input (sparse sampler +
    //    brightness normalization) vs the same sampler without normalization vs a full area
    //    average (the Phase 7 benchmark's method). Box agreement is IoU against the area average.
    {
        face::Detector det(face::BestSimdPath());
        struct Variant {
            const char* name;
            std::vector<std::uint8_t> img = std::vector<std::uint8_t>(240 * 135 * 3);
            int found = 0, lmOk = 0, agree = 0;
            double conf = 0, iou = 0;
            face::Detection d[2];
            int n = 0;
        } v[3] = {{"engine (normalized)"}, {"sampler, raw"}, {"area average, raw"}};
        double lumaSum = 0, gainSum = 0;
        for (int i = 0; i < 10; ++i) {
            withFrame([&](const processing::Nv12Planes& p) {
                float gain = 1;
                face::DownscaleNv12ToBgr(p, fmt, v[0].img.data(), 240, 135, true, &gain);
                face::DownscaleNv12ToBgr(p, fmt, v[1].img.data(), 240, 135, false);
                AreaAverageBgr(p, fmt, v[2].img.data(), 240, 135);
                gainSum += gain;
                std::uint64_t s = 0;
                for (int y = 0; y < p.height; y += 8)
                    for (int x = 0; x < p.width; x += 8) s += p.y[static_cast<ptrdiff_t>(y) * p.yStride + x];
                lumaSum += static_cast<double>(s) / ((p.height / 8.0) * (p.width / 8.0));
            });
            for (auto& x : v) {
                x.n = det.Detect(x.img.data(), 240, 135, 240 * 3, 0.5f, x.d, 2);
                if (x.n > 0) {
                    ++x.found;
                    x.conf += x.d[0].confidence;
                    x.lmOk += x.d[0].landmarksPlausible;
                }
            }
            for (auto& x : v) {
                if (x.n > 0 && v[2].n > 0) {
                    ++x.agree;
                    x.iou += face::IoU(x.d[0].box, v[2].d[0].box);
                }
            }
            Sleep(100);
        }
        std::printf("detector input check (240x135, 10 frames, raw luma %.0f, normalization gain %.2f):\n", lumaSum / 10, gainSum / 10);
        for (auto& x : v) {
            std::printf("  %-20s found %2d/10  confidence %.2f  landmarks ok %2d  IoU vs area average %.2f\n", x.name, x.found,
                        x.found ? x.conf / x.found : 0.0, x.lmOk, x.agree ? x.iou / x.agree : 0.0);
        }
    }

    // 2) The engine, per detector build.
    std::printf("\n%-9s %-8s %6s %14s %8s %14s %9s %9s %8s %9s %11s %9s\n", "build", "input", "det/s", "detect ms", "cpu ms",
                "stage ms", "onFrame", "face %", "lm ok %", "worker %", "RAM +MB", "warmup");
    std::vector<face::SimdPath> paths = {face::SimdPath::Portable};
    if (face::BestSimdPath() == face::SimdPath::Avx2) paths.insert(paths.begin(), face::SimdPath::Avx2);
    for (face::SimdPath path : paths) {
        const ProcessSample before = SampleProcess();
        face::FaceEngine engine;
        face::EngineConfig ec;
        ec.forcePortable = path == face::SimdPath::Portable;
        const double startT = face::FaceEngine::NowMs();
        engine.Start(ec);
        const double startCallMs = face::FaceEngine::NowMs() - startT;
        std::uint64_t frames = 0, framesWithFace = 0;
        double onFrameMax = 0, onFrameSum = 0, peakPrivate = 0;
        face::FaceSnapshot snap;
        float cx = 0, cy = 0, bw = 0;
        face::Landmarks lm{};
        const double t0 = face::FaceEngine::NowMs();
        double nextMem = t0;
        while (face::FaceEngine::NowMs() - t0 < o.seconds * 1000.0) {
            WaitForSingleObject(ev, 100);  // next frame
            auto frame = session->TakeFrame();
            if (!frame) continue;
            LockedNv12 lk(frame->Get(), layout);
            if (!lk.ok()) continue;
            const double now = face::FaceEngine::NowMs();
            engine.OnFrame(lk.planes(), fmt, now, 1000.0 / std::max(1.0, active.Fps()));
            const double callMs = face::FaceEngine::NowMs() - now;
            onFrameMax = std::max(onFrameMax, callMs);
            onFrameSum += callMs;
            ++frames;
            engine.Snapshot(now, snap);
            if (snap.count > 0) {
                ++framesWithFace;
                cx = snap.faces[0].box.Center().x;
                cy = snap.faces[0].box.Center().y;
                bw = snap.faces[0].box.w;
                lm = snap.faces[0].lm;
            }
            if (now >= nextMem) {
                peakPrivate = std::max(peakPrivate, static_cast<double>(SampleProcess().privateBytes));
                nextMem = now + 500;
            }
        }
        const double elapsed = face::FaceEngine::NowMs() - t0;
        const face::EngineStatus s = engine.Status();
        engine.Stop();
        const ProcessSample after = SampleProcess();
        char stage[32], detect[32], ram[32], input[16];
        std::snprintf(input, sizeof(input), "%dx%d", s.inputWidth, s.inputHeight);
        std::snprintf(detect, sizeof(detect), "%.1f / %.1f", s.avgDetectMs, s.maxDetectMs);
        std::snprintf(stage, sizeof(stage), "%.2f / %.2f", s.avgStageMs, s.maxStageMs);
        std::snprintf(ram, sizeof(ram), "%.1f / %.1f", (peakPrivate - static_cast<double>(before.privateBytes)) / 1048576.0,
                      (static_cast<double>(after.privateBytes) - static_cast<double>(before.privateBytes)) / 1048576.0);
        std::printf("%-9s %-8s %6.1f %14s %8.1f %14s %6.2f ms %8.0f%% %8.0f%% %8.1f%% %11s %6.1f ms\n", face::ToString(s.path),
                    input, s.detections * 1000.0 / elapsed, detect, s.avgDetectCpuMs, stage, onFrameMax,
                    frames ? 100.0 * framesWithFace / frames : 0.0, s.detectionsWithFace ? 100.0 * s.landmarksValid / s.detectionsWithFace : 0.0,
                    100.0 * s.workerCpuMs / elapsed, ram, s.warmupMs);
        std::printf("          state %s, %llu frames (%.1f fps), %llu detections, Start() %.2f ms, avg onFrame %.3f ms\n",
                    face::ToString(s.state), static_cast<unsigned long long>(frames), frames * 1000.0 / elapsed,
                    static_cast<unsigned long long>(s.detections), startCallMs, frames ? onFrameSum / frames : 0.0);
        if (framesWithFace) {
            std::printf("          last face: centre (%.2f, %.2f) width %.2f; eyes (%.2f,%.2f) (%.2f,%.2f) nose (%.2f,%.2f) "
                        "mouth (%.2f,%.2f) (%.2f,%.2f)\n",
                        cx, cy, bw, lm.leftEye.x, lm.leftEye.y, lm.rightEye.x, lm.rightEye.y, lm.nose.x, lm.nose.y, lm.mouthLeft.x,
                        lm.mouthLeft.y, lm.mouthRight.x, lm.mouthRight.y);
        }
    }
    session->Stop();
    session->Close();
    return 0;
}

// Runs IXC's effects on a still frame (24-bit BMP) with a real face detection, for side-by-side
// comparison with a reference look. The images stay local (visual tuning only).
int ApplyEffectsToBmp(const Options& o) {
    auto load = [](const std::string& path, int& w, int& h, std::vector<std::uint8_t>& rgb) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        bool ok = fread(&fh, sizeof(fh), 1, f) == 1 && fread(&ih, sizeof(ih), 1, f) == 1 && fh.bfType == 0x4D42 && ih.biBitCount == 24;
        if (ok) {
            w = ih.biWidth;
            h = std::abs(ih.biHeight);
            const size_t row = (static_cast<size_t>(w) * 3 + 3) & ~size_t{3};
            std::vector<std::uint8_t> raw(row * h);
            fseek(f, static_cast<long>(fh.bfOffBits), SEEK_SET);
            ok = fread(raw.data(), 1, raw.size(), f) == raw.size();
            rgb.resize(static_cast<size_t>(w) * h * 3);  // top-down, B,G,R
            for (int y = 0; y < h && ok; ++y) {
                const int src = ih.biHeight > 0 ? h - 1 - y : y;
                memcpy(&rgb[static_cast<size_t>(y) * w * 3], &raw[static_cast<size_t>(src) * row], static_cast<size_t>(w) * 3);
            }
        }
        fclose(f);
        return ok;
    };
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgb;
    if (!load(o.applyIn, w, h, rgb) || w < 64 || h < 64) {
        std::printf("error: cannot read %s (24-bit BMP)\n", o.applyIn.c_str());
        return 1;
    }
    w &= ~1;
    h &= ~1;
    // RGB -> NV12, BT.709 video range (what the camera path delivers at HD).
    std::vector<std::uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
    const int stride = w;
    const size_t srcRow = static_cast<size_t>(w) * 3;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &rgb[static_cast<size_t>(y) * srcRow + static_cast<size_t>(x) * 3];
            const double R = p[2], G = p[1], B = p[0];
            nv12[static_cast<size_t>(y) * stride + x] = static_cast<std::uint8_t>(std::clamp(16 + 0.1826 * R + 0.6142 * G + 0.0620 * B + 0.5, 0.0, 255.0));
        }
    }
    std::uint8_t* uv = nv12.data() + static_cast<size_t>(w) * h;
    for (int y = 0; y < h / 2; ++y) {
        for (int x = 0; x < w / 2; ++x) {
            double R = 0, G = 0, B = 0;
            for (int k = 0; k < 4; ++k) {
                const std::uint8_t* p = &rgb[static_cast<size_t>(y * 2 + k / 2) * srcRow + static_cast<size_t>(x * 2 + k % 2) * 3];
                R += p[2] / 4.0;
                G += p[1] / 4.0;
                B += p[0] / 4.0;
            }
            uv[static_cast<size_t>(y) * stride + x * 2] = static_cast<std::uint8_t>(std::clamp(128 - 0.1006 * R - 0.3386 * G + 0.4392 * B + 0.5, 0.0, 255.0));
            uv[static_cast<size_t>(y) * stride + x * 2 + 1] = static_cast<std::uint8_t>(std::clamp(128 + 0.4392 * R - 0.3989 * G - 0.0403 * B + 0.5, 0.0, 255.0));
        }
    }
    const processing::YuvFormat fmt{processing::YuvMatrix::Bt709, false};
    const processing::Nv12Planes planes{nv12.data(), uv, stride, stride, w, h};
    // Face: one detection at 320x180, turned into a tracked face.
    face::FaceSnapshot snap;
    {
        std::vector<std::uint8_t> detIn(320 * 180 * 3);
        face::DownscaleNv12ToBgr(planes, fmt, detIn.data(), 320, 180);
        face::Detector det(face::BestSimdPath());
        face::Detection d[4];
        const int n = det.Detect(detIn.data(), 320, 180, 320 * 3, 0.5f, d, 4);
        face::Tracker tr;
        tr.Update(d, n, 0, 1);
        tr.Predict(0, snap);
        std::printf("faces: %d%s\n", snap.count, snap.count && snap.faces[0].landmarksValid ? " (landmarks ok)" : "");
    }
    std::vector<EffectEntry> list;
    for (size_t p = 0; p <= o.applyEffects.size();) {
        const size_t q = o.applyEffects.find(',', p);
        const std::string id = o.applyEffects.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!id.empty()) list.push_back({id, o.applyStrength});
        if (q == std::string::npos) break;
        p = q + 1;
    }
    const auto cfg = effects::CompileEffects(list, false);
    effects::EffectRenderer r;
    effects::FrameContext ctx;
    ctx.faces = &snap;
    std::vector<std::uint8_t> work;
    for (int i = 0; i < 30; ++i) {  // lets face effects fade in fully
        work = nv12;
        const processing::Nv12Frame fr{work.data(), work.data() + static_cast<size_t>(w) * h, stride, stride, w, h};
        r.Apply(fr, *cfg, ctx);
    }
    // Colour at landmark-relative skin/lip points (mean RGB of a small square), before and after.
    if (snap.count > 0) {
        const face::TrackedFace& tf = snap.faces[0];
        const face::Landmarks& l = tf.lm;
        const float ed = std::hypot((l.rightEye.x - l.leftEye.x) * w, (l.rightEye.y - l.leftEye.y) * h);
        const float ex = (l.leftEye.x + l.rightEye.x) / 2 * w, ey = (l.leftEye.y + l.rightEye.y) / 2 * h;
        const struct { const char* name; float x, y; } pts[] = {
            {"forehead", ex, ey - 0.55f * ed},
            {"cheek L", l.leftEye.x * w - 0.05f * ed, l.leftEye.y * h + 0.62f * ed},
            {"cheek R", l.rightEye.x * w + 0.05f * ed, l.rightEye.y * h + 0.62f * ed},
            {"under-eye L", l.leftEye.x * w, l.leftEye.y * h + 0.25f * ed},
            {"nose tip", l.nose.x * w, l.nose.y * h},
            {"lips", (l.mouthLeft.x + l.mouthRight.x) / 2 * w, (l.mouthLeft.y + l.mouthRight.y) / 2 * h + 0.06f * ed},
        };
        const int rad = std::max(2, static_cast<int>(ed * 0.06f));
        auto mean = [&](const std::uint8_t* yp, const std::uint8_t* uvp, float cx, float cy, double out[3]) {
            double r = 0, g = 0, b = 0;
            int n = 0;
            for (int y = static_cast<int>(cy) - rad; y <= static_cast<int>(cy) + rad; ++y) {
                for (int x = static_cast<int>(cx) - rad; x <= static_cast<int>(cx) + rad; ++x) {
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    const std::uint8_t* c = uvp + static_cast<size_t>(y / 2) * stride + (x / 2) * 2;
                    const std::uint32_t v = processing::YuvToBgra(yp[static_cast<size_t>(y) * stride + x], c[0], c[1], fmt);
                    b += v & 0xFF;
                    g += (v >> 8) & 0xFF;
                    r += (v >> 16) & 0xFF;
                    ++n;
                }
            }
            out[0] = n ? r / n : 0;
            out[1] = n ? g / n : 0;
            out[2] = n ? b / n : 0;
        };
        std::printf("eye distance %.0f px\n%-12s %-20s %-20s\n", ed, "point", "input R/G/B", "output R/G/B");
        for (const auto& p : pts) {
            double a[3], b[3];
            mean(nv12.data(), uv, p.x, p.y, a);
            mean(work.data(), work.data() + static_cast<size_t>(w) * h, p.x, p.y, b);
            std::printf("%-12s %5.0f %5.0f %5.0f      %5.0f %5.0f %5.0f\n", p.name, a[0], a[1], a[2], b[0], b[1], b[2]);
        }
    }
    // NV12 -> BMP
    const size_t row = (static_cast<size_t>(w) * 3 + 3) & ~size_t{3};
    std::vector<std::uint8_t> outBmp(row * h);
    const std::uint8_t* wy = work.data();
    const std::uint8_t* wuv = work.data() + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        std::uint8_t* dst = &outBmp[static_cast<size_t>(h - 1 - y) * row];
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* c = wuv + static_cast<size_t>(y / 2) * stride + (x / 2) * 2;
            const std::uint32_t v = processing::YuvToBgra(wy[static_cast<size_t>(y) * stride + x], c[0], c[1], fmt);
            dst[x * 3] = static_cast<std::uint8_t>(v);
            dst[x * 3 + 1] = static_cast<std::uint8_t>(v >> 8);
            dst[x * 3 + 2] = static_cast<std::uint8_t>(v >> 16);
        }
    }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(outBmp.size());
    FILE* f = nullptr;
    if (fopen_s(&f, o.applyOut.c_str(), "wb") || !f) return 1;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(outBmp.data(), 1, outBmp.size(), f);
    fclose(f);
    std::printf("wrote %s with %s\n", o.applyOut.c_str(), o.applyEffects.c_str());
    return 0;
}

// Per-effect cost on a synthetic textured frame with a face (no camera needed).
int BenchEffects() {
    std::printf("%-16s %10s %10s %10s\n", "effect", "720p ms", "1080p ms", "scratch KB");
    const face::FaceSnapshot snap = [] {
        face::FaceSnapshot s;
        s.count = 1;
        auto& f = s.faces[0];
        f.box = {0.42f, 0.25f, 0.16f, 0.28f};
        f.lm = {{0.47f, 0.35f}, {0.53f, 0.35f}, {0.5f, 0.41f}, {0.475f, 0.46f}, {0.525f, 0.46f}};
        f.landmarksValid = true;
        return s;
    }();
    std::vector<std::string> ids;
    for (const auto& e : effects::Catalog()) ids.push_back(e.id);
    ids.push_back("all");
    for (const auto& id : ids) {
        std::vector<EffectEntry> list;
        if (id == "all") {
            for (const auto& e : effects::Catalog()) list.push_back({e.id, 70});
        } else {
            list.push_back({id, 70});
        }
        const auto cfg = effects::CompileEffects(list, false);
        double ms[2] = {0, 0};
        size_t scratch = 0;
        const int sizes[2][2] = {{1280, 720}, {1920, 1080}};
        for (int s = 0; s < 2; ++s) {
            const int w = sizes[s][0], h = sizes[s][1];
            std::vector<std::uint8_t> buf(static_cast<size_t>(w) * h * 3 / 2);
            for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<std::uint8_t>(i < static_cast<size_t>(w) * h ? 60 + (i * 2654435761u >> 26) % 120 : 110 + i % 30);
            const processing::Nv12Frame fr{buf.data(), buf.data() + static_cast<size_t>(w) * h, w, w, w, h};
            effects::EffectRenderer r;
            effects::FrameContext ctx;
            ctx.faces = &snap;
            for (int i = 0; i < 10; ++i) r.Apply(fr, *cfg, ctx);  // warm-up + face fade-in
            LARGE_INTEGER f0, t0, t1;
            QueryPerformanceFrequency(&f0);
            QueryPerformanceCounter(&t0);
            constexpr int kIters = 40;
            for (int i = 0; i < kIters; ++i) r.Apply(fr, *cfg, ctx);
            QueryPerformanceCounter(&t1);
            ms[s] = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f0.QuadPart) / kIters;
            scratch = r.ScratchBytes();
        }
        std::printf("%-16s %10.2f %10.2f %10.1f\n", id.c_str(), ms[0], ms[1], scratch / 1024.0);
    }
    return 0;
}

int Capture(const Options& o) {
    CaptureConfig cfg;
    int exitCode = 0;
    if (!Resolve(o, cfg, exitCode)) return exitCode;
    cfg.smoothMotion = o.smoothMotion;

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
    const double smoothEv = session->SmoothCompensationEv();  // before Stop() clears it

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
    if (o.smoothMotion) {
        std::printf("smooth motion:        +%.2f EV software gain (raw luma %.1f -> ~%.1f after the pipeline)\n", smoothEv,
                    consumer.MeanSample(), consumer.MeanSample() * std::exp2(smoothEv));
    }
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
        std::printf("usage: ixc_probe --list | --list-dshow | --dshow-capture <sec> | --source-test <dll> [--seconds N] | --source-effect-test <dll> | --bench-pipeline | --bench-gpu | --capture <sec> | --cycles <n>  [--camera X] [--width W --height H --fps F]\n"
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
                case Options::Mode::ListDirectShow: rc = ListDirectShow(); break;
                case Options::Mode::DirectShowCapture: rc = DirectShowCapture(o); break;
                case Options::Mode::SourceTest: rc = SourceTest(o); break;
                case Options::Mode::SourceEffectTest: rc = SourceEffectTest(o); break;
                case Options::Mode::BenchPipeline: rc = BenchPipeline(); break;
                case Options::Mode::BenchGpu: rc = BenchGpu(); break;
                case Options::Mode::CameraControls: rc = CameraControls(o); break;
                case Options::Mode::BenchGpuMemory: rc = BenchGpuMemory(); break;
                case Options::Mode::BenchFace: rc = BenchFace(o); break;
                case Options::Mode::BenchEffects: rc = BenchEffects(); break;
                case Options::Mode::ApplyEffects: rc = ApplyEffectsToBmp(o); break;
                case Options::Mode::Capture: rc = Capture(o); break;
                case Options::Mode::Cycles: rc = Cycles(o); break;
                case Options::Mode::None: break;
            }
        }
    }
    CoUninitialize();
    return rc;
}
