#include "processing/backend_selector.h"

namespace ixc::processing {

void BackendSelector::Reset(Mode mode, bool geometryWork, std::uint64_t pixels) {
    count_ = 0;
    sum_ = 0;
    requestPending_ = false;
    if (mode != mode_) gpuFailed_ = false;  // the user chose again: give the GPU a new chance
    mode_ = mode;
    if (mode == Mode::Gpu && !gpuFailed_) {
        state_ = State::GpuRequested;  // the CPU processes frames until the GPU is ready
        requestPending_ = true;
        return;
    }
    const bool eligible = mode == Mode::Auto && geometryWork && pixels >= cfg_.minPixels && !gpuFailed_;
    if (!eligible) {
        state_ = State::Cpu;
        return;
    }
    // Re-measure: settings changed, so both costs may have changed.
    state_ = State::MeasuringCpu;
}

bool BackendSelector::TakeGpuRequest() {
    const bool r = requestPending_;
    requestPending_ = false;
    return r;
}

void BackendSelector::OnCpuFrame(double ms) {
    if (state_ != State::MeasuringCpu) {
        if (state_ == State::Cpu || state_ == State::CpuFinal) {  // running average for the diagnostics
            cpuAvg_ = cpuAvg_ == 0 ? ms : cpuAvg_ + (ms - cpuAvg_) * 0.05;
        }
        return;
    }
    sum_ += ms;
    if (++count_ < cfg_.measureFrames) return;
    cpuAvg_ = sum_ / count_;
    count_ = 0;
    sum_ = 0;
    if (cpuAvg_ >= cfg_.cpuSlowMs) {
        state_ = State::GpuRequested;  // keep processing on the CPU until the GPU is ready
        requestPending_ = true;
    } else {
        state_ = State::CpuFinal;      // the CPU is cheap enough: no GPU for these settings
    }
}

void BackendSelector::OnGpuReady(bool ok) {
    if (state_ != State::GpuRequested) return;
    if (!ok) {
        gpuFailed_ = true;
        state_ = State::CpuFinal;
        return;
    }
    state_ = State::MeasuringGpu;
    count_ = 0;
    sum_ = 0;
}

void BackendSelector::OnGpuFrame(double wallMs) {
    if (state_ == State::Gpu) {  // keep a running average for the diagnostics
        gpuAvg_ += (wallMs - gpuAvg_) * 0.05;
        return;
    }
    if (state_ != State::MeasuringGpu) return;
    sum_ += wallMs;
    if (++count_ < cfg_.measureFrames) return;
    gpuAvg_ = sum_ / count_;
    count_ = 0;
    sum_ = 0;
    // Chosen by the user: kept. Auto: kept only where it's clearly faster.
    state_ = mode_ == Mode::Gpu || gpuAvg_ < cpuAvg_ * cfg_.gpuMustBeBelow ? State::Gpu : State::CpuFinal;
}

void BackendSelector::OnGpuFailure() {
    gpuFailed_ = true;
    state_ = State::CpuFinal;
    requestPending_ = false;
}

}  // namespace ixc::processing
