#include "segmentation/segmentation_engine.h"

#include "segmentation/mask_refine.h"
#include "segmentation/selfie_net.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ixc::seg {

const char* ToString(SegState s) {
    switch (s) {
        case SegState::Off: return "off";
        case SegState::Starting: return "starting";
        case SegState::Running: return "running";
        case SegState::TooSlow: return "off (CPU too slow)";
        case SegState::Unavailable: return "unavailable";
    }
    return "?";
}

void SegmentationEngine::SetMode(ProcessingMode mode) {
    if (mode_.exchange(static_cast<int>(mode)) != static_cast<int>(mode)) modeVersion_.fetch_add(1);
}

void SegmentationEngine::SetGpuFactory(GpuRunnerFactory factory) {
    if (gpuFactory_.exchange(factory) != factory) modeVersion_.fetch_add(1);
}

double SegmentationEngine::NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static_assert(kNetW == SelfieNet::kWidth && kNetH == SelfieNet::kHeight);

bool SampleNv12ToRgb(const processing::Nv12Planes& src, const processing::YuvFormat& fmt, std::uint8_t* rgb) {
    if (!src.y || !src.uv || !rgb || src.width < kMaskW || src.height < kMaskH || src.yStride < src.width || src.uvStride < src.width) {
        return false;
    }
    for (int oy = 0; oy < kNetH; ++oy) {
        // Two luma rows a quarter and three quarters into this output row's source band.
        const int band0 = oy * src.height / kNetH, band1 = (oy + 1) * src.height / kNetH;
        const int ya = std::min(band0 + (band1 - band0) / 4, src.height - 1), yb = std::min(band0 + 3 * (band1 - band0) / 4, src.height - 1);
        const std::uint8_t* ra = src.y + static_cast<std::ptrdiff_t>(ya) * src.yStride;
        const std::uint8_t* rb = src.y + static_cast<std::ptrdiff_t>(yb) * src.yStride;
        const std::uint8_t* uv = src.uv + static_cast<std::ptrdiff_t>(std::min((band0 + band1) / 4, src.height / 2 - 1)) * src.uvStride;
        std::uint8_t* out = rgb + static_cast<size_t>(oy) * kNetW * 3;
        for (int ox = 0; ox < kNetW; ++ox) {
            const int c0 = ox * src.width / kNetW, c1 = (ox + 1) * src.width / kNetW;
            const int xa = std::min(c0 + (c1 - c0) / 4, src.width - 1), xb = std::min(c0 + 3 * (c1 - c0) / 4, src.width - 1);
            const int luma = (ra[xa] + ra[xb] + rb[xa] + rb[xb] + 2) / 4;
            const int cx = std::min((c0 + c1) / 4, src.width / 2 - 1) * 2;
            const std::uint32_t bgra = processing::YuvToBgra(luma, uv[cx], uv[cx + 1], fmt);
            out[ox * 3 + 0] = static_cast<std::uint8_t>((bgra >> 16) & 0xFF);  // R
            out[ox * 3 + 1] = static_cast<std::uint8_t>((bgra >> 8) & 0xFF);   // G
            out[ox * 3 + 2] = static_cast<std::uint8_t>(bgra & 0xFF);          // B
        }
    }
    return true;
}

bool SampleLuma(const processing::Nv12Planes& src, std::uint8_t* luma) {
    if (!src.y || !luma || src.width < kMaskW || src.height < kMaskH || src.yStride < src.width) return false;
    for (int oy = 0; oy < kMaskH; ++oy) {
        const int b0 = oy * src.height / kMaskH, b1 = (oy + 1) * src.height / kMaskH;
        const std::uint8_t* ra = src.y + static_cast<std::ptrdiff_t>(std::min(b0 + (b1 - b0) / 4, src.height - 1)) * src.yStride;
        const std::uint8_t* rb = src.y + static_cast<std::ptrdiff_t>(std::min(b0 + 3 * (b1 - b0) / 4, src.height - 1)) * src.yStride;
        std::uint8_t* out = luma + static_cast<size_t>(oy) * kMaskW;
        for (int ox = 0; ox < kMaskW; ++ox) {
            const int c0 = ox * src.width / kMaskW, c1 = (ox + 1) * src.width / kMaskW;
            const int xa = std::min(c0 + (c1 - c0) / 4, src.width - 1), xb = std::min(c0 + 3 * (c1 - c0) / 4, src.width - 1);
            out[ox] = static_cast<std::uint8_t>((ra[xa] + ra[xb] + rb[xa] + rb[xb] + 2) / 4);
        }
    }
    return true;
}

std::uint8_t ProbabilityToMask(float p) {
    // Smoothstep over 0.25..0.75: confident pixels saturate, the uncertain band stays soft.
    const float t = std::clamp((p - 0.25f) * 2.0f, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(t * t * (3 - 2 * t) * 255.0f + 0.5f);
}

float LowLightGain(const std::uint8_t* rgb, size_t pixels) {
    if (!rgb || pixels == 0) return 1.0f;
    std::uint64_t sum = 0;
    for (size_t i = 0; i < pixels; ++i) sum += rgb[i * 3] + 2u * rgb[i * 3 + 1] + rgb[i * 3 + 2];
    const double mean = static_cast<double>(sum) / (4.0 * static_cast<double>(pixels));
    if (mean >= 90 || mean < 4) return 1.0f;  // normal light, or nothing to see
    return static_cast<float>(std::min(2.5, 110.0 / mean));
}

namespace {

// Network input: RGB in 0..1. A dark frame is brightened first (the network was trained on
// normally exposed pictures; in a dim room it otherwise loses the hair and shoulders).
void RgbToInput(const std::uint8_t* rgb, float* in) {
    const size_t pixels = static_cast<size_t>(kNetW) * kNetH;
    const float k = LowLightGain(rgb, pixels) / 255.0f;
    for (size_t i = 0; i < pixels * 3; ++i) in[i] = std::min(1.0f, static_cast<float>(rgb[i]) * k);
}

}  // namespace

bool SegmentationEngine::SegmentOnce(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, std::vector<std::uint8_t>& mask,
                                     double* runMs) {
    std::vector<std::uint8_t> rgb(static_cast<size_t>(kNetW) * kNetH * 3), guide(static_cast<size_t>(kMaskW) * kMaskH);
    if (!SampleNv12ToRgb(frame, fmt, rgb.data()) || !SampleLuma(frame, guide.data())) return false;
    SelfieNet net;
    MaskRefiner refiner;
    if (!net.Init() || !refiner.Init(kNetW, kNetH, kMaskW, kMaskH)) return false;
    RgbToInput(rgb.data(), net.Input());
    const double t0 = NowMs();
    const float* p = net.Run();
    mask.resize(static_cast<size_t>(kMaskW) * kMaskH);
    refiner.Refine(p, guide.data(), mask.data());
    if (runMs) *runMs = NowMs() - t0;
    return true;
}

bool SegmentationEngine::Start(double cpuBudget) {
    Stop();
    budget_ = std::clamp(cpuBudget, 0.05, 0.6);
    {
        std::lock_guard lock(mu_);
        stop_ = false;
        pending_ = false;
        haveMask_ = false;  // generation_ keeps counting across sessions: a caller's old copy never matches
        status_ = {};
        status_.state = SegState::Starting;
        stageTotalMs_ = 0;
        staged_ = 0;
        rateWindowStart_ = NowMs();
        rateWindowCount_ = 0;
        try {
            rgb_.assign(static_cast<size_t>(kNetW) * kNetH * 3, 0);
            guide_.assign(static_cast<size_t>(kMaskW) * kMaskH, 0);
            mask_.assign(static_cast<size_t>(kMaskW) * kMaskH, 0);
        } catch (const std::bad_alloc&) {
            rgb_ = {};
            guide_ = {};
            mask_ = {};
            status_.state = SegState::Unavailable;
            return false;
        }
    }
    nextDueMs_.store(0);
    busy_.store(true);  // the worker initializes the network first
    running_.store(true);
    worker_ = std::thread([this] { Worker(); });
    return true;
}

void SegmentationEngine::Stop() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
    running_.store(false);
    std::lock_guard lock(mu_);
    std::vector<std::uint8_t>().swap(rgb_);
    std::vector<std::uint8_t>().swap(guide_);
    std::vector<std::uint8_t>().swap(mask_);
    haveMask_ = false;
    if (status_.state != SegState::Unavailable) status_.state = SegState::Off;
    status_.memoryBytes = 0;
}

void SegmentationEngine::OnFrame(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, double nowMs) {
    if (!WantsFrame(nowMs)) return;
    const double t0 = NowMs();
    // The worker is idle (busy_ false), so rgb_ and guide_ are ours until we hand them over.
    if (!SampleNv12ToRgb(frame, fmt, rgb_.data()) || !SampleLuma(frame, guide_.data())) return;
    const double stageMs = NowMs() - t0;
    {
        std::lock_guard lock(mu_);
        stagedAtMs_ = nowMs;
        pending_ = true;
        stageTotalMs_ += stageMs;
        ++staged_;
        status_.avgStageMs = stageTotalMs_ / static_cast<double>(staged_);
    }
    busy_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void SegmentationEngine::Worker() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"IXC segmentation");
#endif
    MaskRefiner refiner;
    std::vector<std::uint8_t> working;  // the mask being built, swapped in under the lock
    try {
        working.assign(static_cast<size_t>(kMaskW) * kMaskH, 0);
    } catch (const std::bad_alloc&) {
        working.clear();
    }

    // The network runner, chosen by the processing mode (re-chosen when the mode changes).
    std::unique_ptr<NetRunner> runner;
    std::string gpuNote;
    enum class Auto { MeasureCpu, MeasureGpu, Decided } autoPhase = Auto::Decided;
    double sumMs = 0, cpuAvg = 0;
    int measured = 0;
    unsigned seenVersion = ~0u;
    ProcessingMode mode = ProcessingMode::Auto;
    auto useCpu = [&] {
        if (runner && runner->Backend() == SegBackend::Cpu) return true;
        runner.reset();  // free the GPU runner first
        runner = MakeCpuRunner();
        return runner != nullptr;
    };
    auto useGpu = [&] {
        if (runner && runner->Backend() == SegBackend::Gpu) return true;
        const GpuRunnerFactory factory = gpuFactory_.load();
        if (!factory) {
            gpuNote = "GPU path not available here";
            return false;
        }
        runner.reset();
        std::string err;
        runner = factory(err);
        if (!runner) gpuNote = err.empty() ? "GPU unavailable" : err;
        return runner != nullptr;
    };
    auto publish = [&] {  // status after a runner change
        std::lock_guard lock(mu_);
        status_.backend = runner ? runner->Backend() : SegBackend::None;
        status_.device = runner ? runner->Device() : std::string();
        status_.gpuNote = gpuNote;
        status_.memoryBytes = (runner ? runner->MemoryBytes() : 0) + refiner.MemoryBytes() + rgb_.capacity() + guide_.capacity() +
                              mask_.capacity() + working.capacity();
    };
    auto choose = [&] {  // returns false when not even the CPU runner can be made
        seenVersion = modeVersion_.load();
        mode = static_cast<ProcessingMode>(mode_.load());
        gpuNote.clear();
        autoPhase = Auto::Decided;
        bool ok;
        switch (mode) {
            case ProcessingMode::Gpu: ok = useGpu() || useCpu(); break;
            case ProcessingMode::Cpu: ok = useCpu(); break;
            case ProcessingMode::Auto:
            default:
                ok = useCpu();
                autoPhase = gpuFactory_.load() ? Auto::MeasureCpu : Auto::Decided;
                measured = 0;
                sumMs = 0;
                break;
        }
        publish();
        return ok;
    };

    if (working.empty() || !refiner.Init(kNetW, kNetH, kMaskW, kMaskH) || !choose()) {
        std::lock_guard lock(mu_);
        status_.state = SegState::Unavailable;
        return;  // busy_ stays true: frames are never staged
    }
    {
        std::lock_guard lock(mu_);
        status_.state = SegState::Running;
    }
    busy_.store(false, std::memory_order_release);

    std::uint64_t tooSlowStreak = 0;
    for (;;) {
        double stagedAt = 0;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stop_ || pending_; });
            if (stop_) break;
            pending_ = false;
            stagedAt = stagedAtMs_;
        }
        if (modeVersion_.load() != seenVersion && !choose()) {
            std::lock_guard lock(mu_);
            status_.state = SegState::Unavailable;
            break;
        }
        const double t0 = NowMs();
        RgbToInput(rgb_.data(), runner->Input());
        const float* p = runner->Run();
        if (!p && runner->Backend() == SegBackend::Gpu) {
            // The GPU failed (driver reset, device removed): the CPU takes over until the mode
            // is chosen again.
            gpuNote = "GPU stopped working; using the CPU";
            autoPhase = Auto::Decided;
            if (useCpu()) {
                RgbToInput(rgb_.data(), runner->Input());
                p = runner->Run();
            }
            publish();
        }
        if (!p) {
            std::lock_guard lock(mu_);
            status_.state = SegState::Unavailable;
            break;
        }
        const double netMs = NowMs() - t0;
        // Refinement and temporal smoothing work on `working`, which holds the previous mask
        // (the refiner forgets it at session start); the lock is held only for the copy.
        refiner.Refine(p, guide_.data(), working.data());
        const double runMs = NowMs() - t0;
        const bool onGpu = runner->Backend() == SegBackend::Gpu;
        // Auto (after refinement: p belongs to the runner): a few masks on each, then keep the GPU if it's at least about as fast. The first
        // GPU run (shader and buffer warm-up) isn't counted.
        if (autoPhase == Auto::MeasureCpu) {
            sumMs += netMs;
            if (++measured >= 6) {
                cpuAvg = sumMs / measured;
                measured = 0;
                sumMs = 0;
                if (useGpu()) {
                    autoPhase = Auto::MeasureGpu;
                    measured = -1;
                } else {
                    autoPhase = Auto::Decided;
                    useCpu();
                }
                publish();
            }
        } else if (autoPhase == Auto::MeasureGpu) {
            if (measured++ >= 0) sumMs += netMs;
            if (measured >= 6) {
                const double gpuAvg = sumMs / measured;
                autoPhase = Auto::Decided;
                if (gpuAvg > cpuAvg * 1.1) {
                    char note[96];
                    std::snprintf(note, sizeof note, "GPU slower here (%.1f vs %.1f ms): using the CPU", gpuAvg, cpuAvg);
                    gpuNote = note;
                    useCpu();
                }
                publish();
            }
        }

        std::lock_guard lock(mu_);
        std::copy(working.begin(), working.end(), mask_.begin());
        ++generation_;
        haveMask_ = true;
        SegStatus& s = status_;
        ++s.masks;
        s.avgRunMs += (runMs - s.avgRunMs) / static_cast<double>(s.masks);
        s.maxRunMs = std::max(s.maxRunMs, runMs);
        s.avgNetMs = s.avgNetMs == 0 ? netMs : s.avgNetMs + (netMs - s.avgNetMs) * 0.1;
        const double now = NowMs();
        ++rateWindowCount_;
        if (now - rateWindowStart_ >= 2000) {
            s.masksPerSecond = 1000.0 * static_cast<double>(rateWindowCount_) / (now - rateWindowStart_);
            rateWindowStart_ = now;
            rateWindowCount_ = 0;
        }
        // Cadence: the next mask is due once the CPU budget allows, but never faster than ~30/s.
        // On the GPU the worker mostly waits for the GPU: only the refinement is CPU work.
        const double cpuMs = onGpu ? runMs - netMs * 0.8 : runMs;
        const double interval = std::max(33.0, cpuMs / budget_);
        if (interval > 250.0) {
            if (++tooSlowStreak >= 5) {  // 5 consecutive masks slower than 4/s within budget
                s.state = SegState::TooSlow;
                continue;  // parked: busy_ stays true
            }
        } else {
            tooSlowStreak = 0;
        }
        nextDueMs_.store(stagedAt + interval);
        busy_.store(false, std::memory_order_release);
    }
}

bool SegmentationEngine::Snapshot(SegMask& out) const {
    std::lock_guard lock(mu_);
    if (!haveMask_ || status_.state == SegState::TooSlow) {
        out.generation = 0;
        return false;
    }
    if (out.generation != generation_) {
        if (out.value.size() != mask_.size()) out.value.resize(mask_.size());  // first use only
        std::copy(mask_.begin(), mask_.end(), out.value.begin());
        out.generation = generation_;
    }
    return true;
}

SegStatus SegmentationEngine::Status() const {
    std::lock_guard lock(mu_);
    return status_;
}

}  // namespace ixc::seg
