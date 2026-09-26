#include "face/face_engine.h"

#include "face/downscale.h"

#include <algorithm>

namespace ixc::face {

namespace {
double ThreadCpuMs() {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    auto ms = [](const FILETIME& f) {
        return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4;
    };
    return ms(k) + ms(u);
}
}  // namespace

const char* ToString(EngineState s) {
    switch (s) {
        case EngineState::Off: return "off";
        case EngineState::Starting: return "starting";
        case EngineState::Searching: return "searching";
        case EngineState::Tracking: return "tracking";
        case EngineState::TooSlow: return "off (CPU too slow)";
        case EngineState::Unavailable: return "not included in this build";
    }
    return "?";
}

double FaceEngine::NowMs() {
    static const double toMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return static_cast<double>(n.QuadPart) * toMs;
}

bool FaceEngine::Start(const EngineConfig& config) {
    Stop();
    if (!DetectorAvailable()) {
        std::lock_guard lock(mu_);
        status_ = {};
        status_.state = EngineState::Unavailable;
        return false;
    }
    cfg_ = config;
    cfg_.maxFaces = std::clamp(cfg_.maxFaces, 1, kMaxFaces);
    CadenceConfig cc;
    cc.budget = std::clamp(config.cpuBudget, 0.02, 0.5);
    cc.fixedIntervalFrames = std::max(0, config.fixedIntervalFrames);
    cc.allowLarge = config.allowLargeInput;
    {
        std::lock_guard lock(mu_);
        stop_ = false;
        pending_ = false;
        tracker_.Reset();
        cadence_ = Cadence(cc);
        cadence_.Reset(NowMs());
        status_ = {};
        status_.state = EngineState::Starting;
        status_.path = config.forcePortable ? SimdPath::Portable : BestSimdPath();
        stageTotalMs_ = 0;
        staged_ = 0;
        hzWindowStart_ = NowMs();
        hzWindowCount_ = 0;
        bgr_.assign(static_cast<size_t>(InputWidth(InputSize::Large)) * InputHeight(InputSize::Large) * 3, 0);
    }
    framesSeen_.store(0);
    stageW_.store(InputWidth(cadence_.Size()));
    stageH_.store(InputHeight(cadence_.Size()));
    nextDueMs_.store(0);
    startedAtMs_ = NowMs();
    busy_.store(true);  // the worker warms up first; frames aren't staged until it's ready
    running_.store(true);
    worker_ = std::thread([this] { Worker(); });
    return true;
}

void FaceEngine::Stop() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
    running_.store(false);
    std::lock_guard lock(mu_);
    tracker_.Reset();
    std::vector<std::uint8_t>().swap(bgr_);  // give the memory back
    status_.lastActiveState = status_.state;
    status_.state = EngineState::Off;
    status_.faces = 0;
}

void FaceEngine::OnFrame(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, double nowMs,
                         double frameIntervalMs) {
    if (!running_.load(std::memory_order_relaxed)) return;
    framesSeen_.fetch_add(1, std::memory_order_relaxed);
    // Common case: nothing to do (worker busy or not due yet). No lock, no allocation.
    if (busy_.load(std::memory_order_acquire) || nowMs < nextDueMs_.load(std::memory_order_relaxed)) return;

    const int w = stageW_.load(), h = stageH_.load();
    const double t0 = NowMs();
    // The worker is idle (busy_ false), so bgr_ is ours until we hand it over.
    if (!DownscaleNv12ToBgr(frame, fmt, bgr_.data(), w, h)) return;
    const double stageMs = NowMs() - t0;
    {
        std::lock_guard lock(mu_);
        stagedW_ = w;
        stagedH_ = h;
        stagedAtMs_ = nowMs;
        frameIntervalMs_ = frameIntervalMs > 0 ? frameIntervalMs : 33.3;
        pending_ = true;
        stageTotalMs_ += stageMs;
        ++staged_;
        status_.avgStageMs = stageTotalMs_ / static_cast<double>(staged_);
        status_.maxStageMs = std::max(status_.maxStageMs, stageMs);
    }
    busy_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void FaceEngine::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"IXC face tracking");
    Detector detector(cfg_.forcePortable ? SimdPath::Portable : BestSimdPath());
    Detection dets[kMaxFaces * 2];

    // Warm-up: the first run initializes the network weights. Timed as the start cost.
    {
        std::vector<std::uint8_t> blank(static_cast<size_t>(InputWidth(InputSize::Small)) * InputHeight(InputSize::Small) * 3, 0);
        const double t0 = NowMs();
        detector.Detect(blank.data(), InputWidth(InputSize::Small), InputHeight(InputSize::Small),
                        InputWidth(InputSize::Small) * 3, cfg_.minConfidence, dets, kMaxFaces * 2);
        std::lock_guard lock(mu_);
        status_.warmupMs = NowMs() - t0;
        status_.path = detector.path();
        status_.state = EngineState::Searching;
    }
    busy_.store(false, std::memory_order_release);

    for (;;) {
        int w = 0, h = 0;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stop_ || pending_; });
            if (stop_) break;
            pending_ = false;
            w = stagedW_;
            h = stagedH_;
        }

        const double cpu0 = ThreadCpuMs(), t0 = NowMs();
        const int n = detector.Detect(bgr_.data(), w, h, w * 3, cfg_.minConfidence, dets, kMaxFaces * 2);
        const double wallMs = NowMs() - t0, cpuMs = ThreadCpuMs() - cpu0;

        std::lock_guard lock(mu_);
        tracker_.Update(dets, n, stagedAtMs_, cfg_.maxFaces);
        // Wall time drives the cadence: per-call thread CPU time has ~15.6 ms granularity, and wall
        // time is conservative (it also counts time the worker was preempted on a busy PC).
        const double cost = wallMs;
        cadence_.OnDetection(cost, stagedAtMs_, tracker_.Count(), tracker_.SmallestFace());

        EngineStatus& s = status_;
        ++s.detections;
        if (n > 0) ++s.detectionsWithFace;
        if (n > 0 && dets[0].landmarksPlausible) ++s.landmarksValid;
        s.avgDetectMs += (wallMs - s.avgDetectMs) / static_cast<double>(s.detections);
        s.avgDetectCpuMs += (cpuMs - s.avgDetectCpuMs) / static_cast<double>(s.detections);
        s.maxDetectMs = std::max(s.maxDetectMs, wallMs);
        s.workerCpuMs += cpuMs;
        s.faces = tracker_.Count();
        s.inputWidth = w;
        s.inputHeight = h;
        const double now = NowMs();
        ++hzWindowCount_;
        if (now - hzWindowStart_ >= 2000) {
            s.detectHz = 1000.0 * static_cast<double>(hzWindowCount_) / (now - hzWindowStart_);
            hzWindowStart_ = now;
            hzWindowCount_ = 0;
        }

        if (cadence_.TooSlow()) {
            s.state = EngineState::TooSlow;
            tracker_.Reset();
            s.faces = 0;
            // Stay parked: frames are never staged again (busy_ stays true) until Stop/Start.
            continue;
        }
        s.state = tracker_.Count() > 0 ? EngineState::Tracking : EngineState::Searching;
        stageW_.store(InputWidth(cadence_.Size()));
        stageH_.store(InputHeight(cadence_.Size()));
        nextDueMs_.store(stagedAtMs_ + cadence_.IntervalMs(tracker_.Stable(), frameIntervalMs_));
        busy_.store(false, std::memory_order_release);
    }
    {
        std::lock_guard lock(mu_);
        status_.workerCpuMs = ThreadCpuMs();
    }
}

void FaceEngine::Snapshot(double nowMs, FaceSnapshot& out) const {
    std::lock_guard lock(mu_);
    tracker_.Predict(nowMs, out);
}

EngineStatus FaceEngine::Status() const {
    std::lock_guard lock(mu_);
    EngineStatus s = status_;
    s.framesSeen = framesSeen_.load();
    return s;
}

}  // namespace ixc::face
