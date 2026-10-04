#include "segmentation/segmentation_engine.h"

#include "segmentation/selfie_net.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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

double SegmentationEngine::NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool SampleNv12ToRgb(const processing::Nv12Planes& src, const processing::YuvFormat& fmt, std::uint8_t* rgb) {
    if (!src.y || !src.uv || !rgb || src.width < kMaskW || src.height < kMaskH || src.yStride < src.width || src.uvStride < src.width) {
        return false;
    }
    for (int oy = 0; oy < kMaskH; ++oy) {
        // Two luma rows a quarter and three quarters into this output row's source band.
        const int band0 = oy * src.height / kMaskH, band1 = (oy + 1) * src.height / kMaskH;
        const int ya = std::min(band0 + (band1 - band0) / 4, src.height - 1), yb = std::min(band0 + 3 * (band1 - band0) / 4, src.height - 1);
        const std::uint8_t* ra = src.y + static_cast<std::ptrdiff_t>(ya) * src.yStride;
        const std::uint8_t* rb = src.y + static_cast<std::ptrdiff_t>(yb) * src.yStride;
        const std::uint8_t* uv = src.uv + static_cast<std::ptrdiff_t>(std::min((band0 + band1) / 4, src.height / 2 - 1)) * src.uvStride;
        std::uint8_t* out = rgb + static_cast<size_t>(oy) * kMaskW * 3;
        for (int ox = 0; ox < kMaskW; ++ox) {
            const int c0 = ox * src.width / kMaskW, c1 = (ox + 1) * src.width / kMaskW;
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

std::uint8_t ProbabilityToMask(float p) {
    // Smoothstep over 0.25..0.75: confident pixels saturate, the uncertain band stays soft.
    const float t = std::clamp((p - 0.25f) * 2.0f, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(t * t * (3 - 2 * t) * 255.0f + 0.5f);
}

namespace {

void RgbToInput(const std::uint8_t* rgb, float* in) {
    constexpr float k = 1.0f / 255.0f;
    for (size_t i = 0; i < static_cast<size_t>(kMaskW) * kMaskH * 3; ++i) in[i] = static_cast<float>(rgb[i]) * k;
}

}  // namespace

bool SegmentationEngine::SegmentOnce(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, std::vector<std::uint8_t>& mask,
                                     double* runMs) {
    std::vector<std::uint8_t> rgb(static_cast<size_t>(kMaskW) * kMaskH * 3);
    if (!SampleNv12ToRgb(frame, fmt, rgb.data())) return false;
    SelfieNet net;
    if (!net.Init()) return false;
    RgbToInput(rgb.data(), net.Input());
    const double t0 = NowMs();
    const float* p = net.Run();
    if (runMs) *runMs = NowMs() - t0;
    mask.resize(static_cast<size_t>(kMaskW) * kMaskH);
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = ProbabilityToMask(p[i]);
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
            rgb_.assign(static_cast<size_t>(kMaskW) * kMaskH * 3, 0);
            mask_.assign(static_cast<size_t>(kMaskW) * kMaskH, 0);
        } catch (const std::bad_alloc&) {
            rgb_ = {};
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
    std::vector<std::uint8_t>().swap(mask_);
    haveMask_ = false;
    if (status_.state != SegState::Unavailable) status_.state = SegState::Off;
    status_.memoryBytes = 0;
}

void SegmentationEngine::OnFrame(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, double nowMs) {
    if (!WantsFrame(nowMs)) return;
    const double t0 = NowMs();
    // The worker is idle (busy_ false), so rgb_ is ours until we hand it over.
    if (!SampleNv12ToRgb(frame, fmt, rgb_.data())) return;
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
    SelfieNet net;
    if (!net.Init()) {
        std::lock_guard lock(mu_);
        status_.state = SegState::Unavailable;
        return;  // busy_ stays true: frames are never staged
    }
    {
        std::lock_guard lock(mu_);
        status_.state = SegState::Running;
        status_.memoryBytes = net.MemoryBytes() + rgb_.capacity() + mask_.capacity();
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
        const double t0 = NowMs();
        RgbToInput(rgb_.data(), net.Input());
        const float* p = net.Run();
        const double runMs = NowMs() - t0;

        std::lock_guard lock(mu_);
        // Temporal smoothing: the first mask is taken as is, later ones blend with the previous
        // one (removes edge flicker; a moving person still updates within 2-3 masks).
        const bool first = !haveMask_;
        for (size_t i = 0; i < mask_.size(); ++i) {
            const int m = ProbabilityToMask(p[i]);
            mask_[i] = first ? static_cast<std::uint8_t>(m) : static_cast<std::uint8_t>((m * 5 + mask_[i] * 3 + 4) / 8);
        }
        ++generation_;
        haveMask_ = true;
        SegStatus& s = status_;
        ++s.masks;
        s.avgRunMs += (runMs - s.avgRunMs) / static_cast<double>(s.masks);
        s.maxRunMs = std::max(s.maxRunMs, runMs);
        const double now = NowMs();
        ++rateWindowCount_;
        if (now - rateWindowStart_ >= 2000) {
            s.masksPerSecond = 1000.0 * static_cast<double>(rateWindowCount_) / (now - rateWindowStart_);
            rateWindowStart_ = now;
            rateWindowCount_ = 0;
        }
        // Cadence: the next mask is due once the budget allows, but never faster than ~30/s.
        const double interval = std::max(33.0, runMs / budget_);
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
