#pragma once

// CPU-first processor that moves a frame to the GPU only when BackendSelector's on-machine
// measurements show a clear benefit.
//
//   * The CPU path (Nv12Processor) is always present and always the fallback.
//   * The GPU is brought up on a thread-pool work item, so the frame path never stalls on
//     device creation. Before its output is trusted, one frame is compared byte-for-byte
//     with the CPU result; any mismatch or runtime error disables the GPU for the session.
//   * The GPU (about 40 MB) is released as soon as it isn't in use.
//   * Never used on PCs with less than 4 GB of RAM or less than 1 GB free: there, the memory
//     matters more than the CPU time saved.
//   * Direct3D is delay-loaded by the binaries that use this class, so nothing GPU-related is
//     even loaded unless a GPU attempt is made.
//
// Process() is called from one thread. ReleaseGpu() may be called from another.

#include "processing/backend_selector.h"
#include "processing/image_pipeline.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ixc::processing {

class GpuNv12Processor;

class AdaptiveNv12Processor {
public:
    AdaptiveNv12Processor();
    ~AdaptiveNv12Processor();
    AdaptiveNv12Processor(const AdaptiveNv12Processor&) = delete;
    AdaptiveNv12Processor& operator=(const AdaptiveNv12Processor&) = delete;

    // `generation` must change whenever the settings (params) change; that triggers a fresh
    // CPU/GPU decision for the new settings.
    bool Process(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& params, std::uint64_t generation);

    void ReleaseGpu();  // e.g. when a session ends

    struct Stats {
        Backend backend = Backend::Cpu;
        BackendSelector::State state = BackendSelector::State::Cpu;
        double cpuAvgMs = 0, gpuAvgMs = 0;
        bool gpuHeld = false, gpuFailed = false;
        std::wstring adapter;
        std::uint64_t gpuFrames = 0, cpuFrames = 0;
    };
    Stats GetStats() const;

    // True when this PC has enough memory to consider the GPU at all.
    static bool SystemAllowsGpu();

private:
    static void CALLBACK InitWork(PTP_CALLBACK_INSTANCE, void* ctx, PTP_WORK);
    void PollInit();

    Nv12Processor cpu_;
    BackendSelector selector_;
    std::uint64_t generation_ = ~0ull;
    bool systemAllows_ = false;

    mutable std::mutex gpuMu_;               // guards gpu_ (frame thread vs ReleaseGpu)
    std::unique_ptr<GpuNv12Processor> gpu_;  // ready and self-checked
    bool selfChecked_ = false;
    std::vector<std::uint8_t> checkBuffer_;  // CPU reference for the one-time self-check

    std::mutex initMu_;                      // guards the async init hand-off
    std::unique_ptr<GpuNv12Processor> initResult_;
    bool initDone_ = false;
    HRESULT initHr_ = S_OK;
    PTP_WORK initWork_ = nullptr;
    bool initInFlight_ = false;

    std::uint64_t gpuFrames_ = 0, cpuFrames_ = 0;
    std::wstring adapter_;
};

}  // namespace ixc::processing
