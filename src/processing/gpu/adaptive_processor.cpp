#include "processing/gpu/adaptive_processor.h"

#include "processing/gpu/gpu_pipeline.h"

#include <cstring>

namespace ixc::processing {

namespace {
double NowMs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / freq;
}
}  // namespace

AdaptiveNv12Processor::AdaptiveNv12Processor() : systemAllows_(SystemAllowsGpu()) {}

AdaptiveNv12Processor::~AdaptiveNv12Processor() {
    if (initWork_) {
        WaitForThreadpoolWorkCallbacks(initWork_, FALSE);  // let a running init finish
        CloseThreadpoolWork(initWork_);
    }
}

bool AdaptiveNv12Processor::SystemAllowsGpu() {
    MEMORYSTATUSEX m{sizeof(m)};
    if (!GlobalMemoryStatusEx(&m)) return false;
    constexpr unsigned long long kGiB = 1024ull * 1024 * 1024;
    // Windows reports slightly less than the installed RAM; 3.5 GiB covers "4 GB" machines.
    return m.ullTotalPhys >= 3 * kGiB + kGiB / 2 && m.ullAvailPhys >= kGiB;
}

void CALLBACK AdaptiveNv12Processor::InitWork(PTP_CALLBACK_INSTANCE, void* ctx, PTP_WORK) {
    auto* self = static_cast<AdaptiveNv12Processor*>(ctx);
    auto gpu = std::make_unique<GpuNv12Processor>();
    const HRESULT hr = gpu->Initialize({});  // default hardware adapter; software adapters are refused
    std::lock_guard lock(self->initMu_);
    self->initHr_ = hr;
    self->initResult_ = SUCCEEDED(hr) ? std::move(gpu) : nullptr;
    self->initDone_ = true;
}

void AdaptiveNv12Processor::PollInit() {
    std::unique_ptr<GpuNv12Processor> ready;
    HRESULT hr = S_OK;
    {
        std::lock_guard lock(initMu_);
        if (!initDone_) return;
        initDone_ = false;
        initInFlight_ = false;
        ready = std::move(initResult_);
        hr = initHr_;
    }
    if (FAILED(hr) || !ready) {
        selector_.OnGpuReady(false);
        return;
    }
    {
        std::lock_guard lock(gpuMu_);
        adapter_ = ready->AdapterName();
        gpu_ = std::move(ready);
        selfChecked_ = false;
    }
    selector_.OnGpuReady(true);
}

bool AdaptiveNv12Processor::Process(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& params, std::uint64_t generation) {
    if (generation != generation_) {
        generation_ = generation;
        selector_.Reset(params.gpuAllowed && systemAllows_, !params.geometryIdentity,
                        static_cast<std::uint64_t>(src.width) * static_cast<std::uint64_t>(src.height));
    }
    if (initInFlight_) PollInit();
    if (selector_.TakeGpuRequest()) {
        if (!initWork_) initWork_ = CreateThreadpoolWork(&AdaptiveNv12Processor::InitWork, this, nullptr);
        if (initWork_) {
            initInFlight_ = true;
            SubmitThreadpoolWork(initWork_);
        } else {
            selector_.OnGpuReady(false);
        }
    }

    if (selector_.Current() == Backend::Gpu) {
        std::unique_lock lock(gpuMu_);
        if (gpu_) {
            const double t0 = NowMs();
            bool ok = gpu_->Process(src, dst, params);
            const double wall = NowMs() - t0;
            if (ok && !selfChecked_) {
                // Trust but verify: the first GPU frame must equal the CPU reference exactly.
                const size_t ySize = static_cast<size_t>(src.width) * static_cast<size_t>(src.height);
                checkBuffer_.resize(ySize * 3 / 2);
                const Nv12Frame ref{checkBuffer_.data(), checkBuffer_.data() + ySize, src.width, src.width, src.width, src.height};
                ok = cpu_.Process(src, ref, params);
                for (int y = 0; ok && y < src.height; ++y) {
                    ok = std::memcmp(dst.y + static_cast<std::ptrdiff_t>(y) * dst.yStride, ref.y + static_cast<size_t>(y) * src.width,
                                     static_cast<size_t>(src.width)) == 0;
                }
                for (int y = 0; ok && y < src.height / 2; ++y) {
                    ok = std::memcmp(dst.uv + static_cast<std::ptrdiff_t>(y) * dst.uvStride, ref.uv + static_cast<size_t>(y) * src.width,
                                     static_cast<size_t>(src.width)) == 0;
                }
                std::vector<std::uint8_t>().swap(checkBuffer_);
                selfChecked_ = ok;
            }
            if (ok) {
                selector_.OnGpuFrame(wall);
                ++gpuFrames_;
                return true;
            }
            selector_.OnGpuFailure();  // falls through to the CPU for this frame
        } else {
            selector_.OnGpuFailure();
        }
    }

    const double t0 = NowMs();
    const bool ok = cpu_.Process(src, dst, params);
    selector_.OnCpuFrame(NowMs() - t0);
    ++cpuFrames_;
    if (selector_.ShouldReleaseGpu()) {
        std::lock_guard lock(gpuMu_);
        gpu_.reset();  // ~40 MB back as soon as the GPU isn't wanted
    }
    return ok;
}

void AdaptiveNv12Processor::ReleaseGpu() {
    std::lock_guard lock(gpuMu_);
    gpu_.reset();
}

AdaptiveNv12Processor::Stats AdaptiveNv12Processor::GetStats() const {
    Stats s;
    s.backend = selector_.Current();
    s.state = selector_.state();
    s.cpuAvgMs = selector_.CpuAvgMs();
    s.gpuAvgMs = selector_.GpuAvgMs();
    s.gpuFailed = selector_.GpuFailed();
    s.gpuFrames = gpuFrames_;
    s.cpuFrames = cpuFrames_;
    std::lock_guard lock(gpuMu_);
    s.gpuHeld = gpu_ != nullptr;
    s.adapter = adapter_;
    return s;
}

}  // namespace ixc::processing
