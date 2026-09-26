#pragma once

// Face tracking engine: detector on a background thread + tracker + adaptive cadence.
//
// Contract with the video path (IXC Camera source, app preview):
//   * OnFrame() never waits for inference. On most frames it returns after one comparison.
//     When a detection is due and the worker is idle, it downsamples the frame (0.5-0.8 ms measured)
//     into the worker's buffer and wakes it.
//   * Snapshot() returns the tracked faces predicted for "now" (cheap, lock held briefly).
//   * Not started = no thread, no memory: tracking off costs nothing.
//   * The worker runs below normal priority, one detection at a time, within a CPU budget.
//     Weak CPUs get a slower rate and a smaller input; if even that doesn't fit, tracking turns
//     itself off (state TooSlow) and the video is unaffected.

#include "face/cadence.h"
#include "face/detector.h"
#include "face/face_types.h"
#include "face/tracker.h"
#include "processing/color.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace ixc::face {

struct EngineConfig {
    int maxFaces = 1;
    double cpuBudget = 0.10;         // fraction of one core (see CadenceConfig::budget)
    int fixedIntervalFrames = 0;     // profile override; 0 = adaptive
    float minConfidence = 0.5f;
    bool forcePortable = false;      // benchmarks/tests: use the non-AVX2 build
    bool allowLargeInput = true;     // 320x180 for small faces (fast CPUs with >= 4 GB RAM only)
};

enum class EngineState { Off, Starting, Searching, Tracking, TooSlow, Unavailable };
const char* ToString(EngineState s);

struct EngineStatus {
    EngineState state = EngineState::Off;
    EngineState lastActiveState = EngineState::Off;  // state before the last Stop()
    int faces = 0;
    SimdPath path = SimdPath::Portable;
    int inputWidth = 0, inputHeight = 0;
    double detectHz = 0;           // detections per second over the last few seconds
    double avgDetectMs = 0;        // wall time per detection (worker)
    double avgDetectCpuMs = 0;     // CPU time per detection
    double maxDetectMs = 0;
    double avgStageMs = 0;         // frame-thread cost of preparing a detection
    double maxStageMs = 0;
    double warmupMs = 0;           // one-time start cost (first detector run)
    std::uint64_t framesSeen = 0;
    std::uint64_t detections = 0;
    std::uint64_t detectionsWithFace = 0;
    std::uint64_t landmarksValid = 0;   // detections whose best face had plausible landmarks
    double workerCpuMs = 0;        // total CPU used by the worker thread
};

class FaceEngine {
public:
    FaceEngine() = default;
    ~FaceEngine() { Stop(); }
    FaceEngine(const FaceEngine&) = delete;
    FaceEngine& operator=(const FaceEngine&) = delete;

    // Starts the worker. False when face tracking isn't compiled in (state Unavailable).
    bool Start(const EngineConfig& config);
    // Joins the worker and frees everything. Idempotent.
    void Stop();
    bool Running() const { return running_.load(); }
    double StartedAtMs() const { return startedAtMs_; }
    // Cheap pre-check for the frame path: true when OnFrame would sample this frame, so callers
    // can skip locking the frame buffer otherwise. (Two atomic loads.)
    bool WantsFrame(double nowMs) const {
        return running_.load(std::memory_order_relaxed) && !busy_.load(std::memory_order_acquire) &&
               nowMs >= nextDueMs_.load(std::memory_order_relaxed);
    }

    // Frame thread. nowMs: QPC milliseconds when the frame arrived; frameIntervalMs: nominal.
    void OnFrame(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, double nowMs, double frameIntervalMs);
    // Faces predicted for nowMs (source-normalized coordinates).
    void Snapshot(double nowMs, FaceSnapshot& out) const;
    EngineStatus Status() const;

    static double NowMs();

private:
    void Worker();

    EngineConfig cfg_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> busy_{false};         // worker owns bgr_ while true
    std::atomic<double> nextDueMs_{0};
    std::atomic<int> stageW_{0}, stageH_{0}; // size the worker wants next
    std::atomic<std::uint64_t> framesSeen_{0};
    double startedAtMs_ = 0;

    mutable std::mutex mu_;                 // tracker, cadence, status, wake-up flags
    std::condition_variable cv_;
    bool stop_ = false;
    bool pending_ = false;
    double stagedAtMs_ = 0;
    double frameIntervalMs_ = 33.3;
    std::vector<std::uint8_t> bgr_;         // largest input size, allocated at Start
    int stagedW_ = 0, stagedH_ = 0;
    Tracker tracker_;
    Cadence cadence_;
    EngineStatus status_;
    double stageTotalMs_ = 0;
    std::uint64_t staged_ = 0;
    double hzWindowStart_ = 0;
    std::uint64_t hzWindowCount_ = 0;
};

}  // namespace ixc::face
