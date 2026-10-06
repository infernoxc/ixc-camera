#pragma once

// Decides, from measurements on the user's own machine, whether a frame should be processed on
// the CPU or the GPU. Pure logic (no Direct3D here), so it's deterministic and unit-tested.
//
// Evidence behind the rules (docs/performance.md, Phase 6): both paths produce identical
// bytes, but the GPU path costs ~41 MB and needs an upload + readback per frame. On the
// reference system it only wins clearly for the geometry pass (crop / digital zoom) at 720p
// and above (6.8 ms → 1.65 ms per 1080p frame). Colour, tone, sharpen and mirror are cheaper
// on the CPU once RAM and latency are counted. So:
//   1. Only geometry work at >= 720p is eligible, and only when the GPU is allowed (setting,
//      enough RAM, a hardware adapter).
//   2. The CPU path is measured first; the GPU is tried only if the CPU is genuinely slow.
//   3. The GPU is kept only if it's measurably faster; otherwise it's released for the session.
//   4. Any GPU failure falls back to the CPU for the rest of the session.
// That is the Auto processing mode. The user can also choose GPU (the GPU is brought up at once,
// for any work and size, and kept even where it isn't faster; a failure still falls back to the
// CPU) or CPU (the GPU is never touched). A failure is remembered until the mode changes.

#include <cstdint>

namespace ixc::processing {

enum class Backend { Cpu, Gpu };

class BackendSelector {
public:
    struct Config {
        int measureFrames = 30;          // frames averaged before each decision
        double cpuSlowMs = 3.0;          // try the GPU only above this CPU cost per frame
        double gpuMustBeBelow = 0.7;     // keep the GPU only if gpu wall < 70% of cpu time
        std::uint64_t minPixels = 1280ull * 720ull;
    };

    enum class State { Cpu, MeasuringCpu, GpuRequested, MeasuringGpu, Gpu, CpuFinal };
    enum class Mode { Auto, Cpu, Gpu };

    BackendSelector() : BackendSelector(Config{}) {}
    explicit BackendSelector(Config c) : cfg_(c) {}

    // New settings or a new session. In Auto, eligible = geometry work && size >= minPixels.
    void Reset(Mode mode, bool geometryWork, std::uint64_t pixels);
    void Reset(bool gpuAllowed, bool geometryWork, std::uint64_t pixels) { Reset(gpuAllowed ? Mode::Auto : Mode::Cpu, geometryWork, pixels); }

    // Backend to use for the next frame.
    Backend Current() const { return state_ == State::MeasuringGpu || state_ == State::Gpu ? Backend::Gpu : Backend::Cpu; }
    // True once when the GPU should be brought up (the caller initializes it asynchronously).
    bool TakeGpuRequest();

    void OnCpuFrame(double ms);
    void OnGpuReady(bool ok);   // initialization (and self-check) result
    void OnGpuFrame(double wallMs);
    void OnGpuFailure();        // any runtime GPU error

    // True when a held GPU should be released (not in use and not about to be).
    bool ShouldReleaseGpu() const { return state_ == State::Cpu || state_ == State::CpuFinal; }

    State state() const { return state_; }
    double CpuAvgMs() const { return cpuAvg_; }
    double GpuAvgMs() const { return gpuAvg_; }
    bool GpuFailed() const { return gpuFailed_; }
    Mode mode() const { return mode_; }

private:
    Config cfg_;
    State state_ = State::Cpu;
    bool requestPending_ = false;
    bool gpuFailed_ = false;  // sticky until the mode changes
    Mode mode_ = Mode::Auto;
    int count_ = 0;
    double sum_ = 0;
    double cpuAvg_ = 0, gpuAvg_ = 0;
};

}  // namespace ixc::processing
