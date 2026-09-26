#include "processing/backend_selector.h"

namespace ixc::processing {

void BackendSelector::Reset(bool gpuAllowed, bool geometryWork, std::uint64_t pixels) {
    count_ = 0;
    sum_ = 0;
    requestPending_ = false;
    const bool eligible = gpuAllowed && geometryWork && pixels >= cfg_.minPixels && !gpuFailed_;
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
    if (state_ != State::MeasuringCpu) return;
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
    if (state_ != State::MeasuringGpu) return;
    sum_ += wallMs;
    if (++count_ < cfg_.measureFrames) return;
    gpuAvg_ = sum_ / count_;
    count_ = 0;
    sum_ = 0;
    state_ = gpuAvg_ < cpuAvg_ * cfg_.gpuMustBeBelow ? State::Gpu : State::CpuFinal;
}

void BackendSelector::OnGpuFailure() {
    gpuFailed_ = true;
    state_ = State::CpuFinal;
    requestPending_ = false;
}

}  // namespace ixc::processing
