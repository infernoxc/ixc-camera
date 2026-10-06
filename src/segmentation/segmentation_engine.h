#pragma once

// Person segmentation engine: SelfieNet on a background thread, feeding a person mask to the
// background effects (effects/effects.h).
//
// Contract with the video path (same shape as face::FaceEngine):
//   * OnFrame() never waits for inference. When a mask is due and the worker is idle, it samples
//     the frame into a 256x144 RGB staging buffer for the network and a 512x288 luma guide for
//     edge refinement (~185 K pixels together), and wakes the worker.
//   * The worker runs the network, then refines the mask along image edges and stabilizes it
//     over time (segmentation/mask_refine.h).
//   * Snapshot() copies the latest mask (512x288 bytes) only when it changed since the caller's
//     last copy, so the frame path normally costs one comparison.
//   * Not started = no thread, no memory. Started = ~7 MB (network weights and activations,
//     refinement buffers).
//   * The worker runs below normal priority within a CPU budget (fraction of one core). On a CPU
//     too slow to produce at least 4 masks per second inside the budget, it parks itself (state
//     TooSlow) and the background effects fall back to their no-mask behaviour.
//   * The network runs on the CPU or the GPU (segmentation/net_runner.h) per the processing mode:
//     Cpu: always the CPU. Gpu: the GPU when it works, else the CPU (the reason is reported).
//     Auto: measures the CPU, then the GPU, and keeps the GPU if it's at least about as fast
//     (it takes the work off the CPU). SetMode() switches live, between two masks; the runner
//     that's no longer used is destroyed (its memory, including video memory, is freed).

#include "processing/color.h"
#include "profiles/profile.h"
#include "segmentation/net_runner.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ixc::seg {

inline constexpr int kNetW = 256, kNetH = 144;   // network input/output
inline constexpr int kMaskW = 512, kMaskH = 288;  // refined mask handed to the effects

enum class SegState { Off, Starting, Running, TooSlow, Unavailable };
const char* ToString(SegState s);

struct SegStatus {
    SegState state = SegState::Off;
    double avgRunMs = 0;      // network time per mask (worker wall time)
    double maxRunMs = 0;
    double avgStageMs = 0;    // frame-thread cost of sampling a frame
    double masksPerSecond = 0;
    std::uint64_t masks = 0;
    size_t memoryBytes = 0;
    SegBackend backend = SegBackend::None;  // where the network runs now
    std::string device;                     // "CPU" or the GPU adapter
    double avgNetMs = 0;                    // network only (recent average)
    std::string gpuNote;                    // why the GPU isn't used (fallback/measurement), "" if it is or wasn't wanted
};

// A mask in source-normalized coordinates (the camera frame before crop/zoom/mirror).
// value: 0 = background, 255 = person. generation 0 = no mask yet.
struct SegMask {
    std::uint64_t generation = 0;
    std::vector<std::uint8_t> value;  // kMaskW * kMaskH, row-major
};

class SegmentationEngine {
public:
    SegmentationEngine() = default;
    ~SegmentationEngine() { Stop(); }
    SegmentationEngine(const SegmentationEngine&) = delete;
    SegmentationEngine& operator=(const SegmentationEngine&) = delete;

    // cpuBudget: fraction of one core the worker may use (clamped to 0.05..0.6).
    bool Start(double cpuBudget = 0.25);
    // Where the network runs (live). factory: how to make a GPU runner; null = CPU only.
    void SetMode(ProcessingMode mode);
    void SetGpuFactory(GpuRunnerFactory factory);
    void Stop();  // joins the worker and frees everything; idempotent
    bool Running() const { return running_.load(); }

    // Cheap pre-check for the frame path (two atomic loads).
    bool WantsFrame(double nowMs) const {
        return running_.load(std::memory_order_relaxed) && !busy_.load(std::memory_order_acquire) &&
               nowMs >= nextDueMs_.load(std::memory_order_relaxed);
    }
    // Frame thread: stage the frame for the worker if a mask is due.
    void OnFrame(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, double nowMs);
    // Copies the latest mask into out when its generation differs from out.generation. Returns
    // true when out holds a mask. out.value is sized on first use only.
    bool Snapshot(SegMask& out) const;
    SegStatus Status() const;

    static double NowMs();

    // Test hook: run the network synchronously on a frame (no thread), producing the same mask
    // the worker would (without temporal smoothing). For tests and ixc_probe benchmarks.
    static bool SegmentOnce(const processing::Nv12Planes& frame, const processing::YuvFormat& fmt, std::vector<std::uint8_t>& mask,
                            double* runMs = nullptr);

private:
    void Worker();

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> busy_{false};          // worker owns rgb_ while true
    std::atomic<double> nextDueMs_{0};
    double budget_ = 0.25;
    std::atomic<int> mode_{static_cast<int>(ProcessingMode::Auto)};
    std::atomic<unsigned> modeVersion_{0};
    std::atomic<GpuRunnerFactory> gpuFactory_{nullptr};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool pending_ = false;
    double stagedAtMs_ = 0;
    std::vector<std::uint8_t> rgb_;         // staging: kNetW * kNetH * 3
    std::vector<std::uint8_t> guide_;       // staging: kMaskW * kMaskH luma
    std::vector<std::uint8_t> mask_;        // latest smoothed mask (under mu_)
    std::uint64_t generation_ = 0;          // under mu_; never reset (starts at 1 for the first mask)
    bool haveMask_ = false;                 // under mu_: a mask exists in this session
    SegStatus status_;
    double stageTotalMs_ = 0;
    std::uint64_t staged_ = 0;
    double rateWindowStart_ = 0;
    std::uint64_t rateWindowCount_ = 0;
};

// Samples an NV12 frame into kNetW x kNetH RGB bytes (2x2 luma average, one chroma sample per
// output pixel). False on inconsistent input. Exposed for tests.
bool SampleNv12ToRgb(const processing::Nv12Planes& src, const processing::YuvFormat& fmt, std::uint8_t* rgb);
// Brightness gain applied to the network input of a dark frame (1 in normal light, up to 2.5).
float LowLightGain(const std::uint8_t* rgb, size_t pixels);
// Samples the luma plane into kMaskW x kMaskH bytes (the refinement guide).
bool SampleLuma(const processing::Nv12Planes& src, std::uint8_t* luma);

// Turns network probabilities into mask bytes: a soft threshold around 0.5 that sharpens the edge
// without making it binary. Exposed for tests.
std::uint8_t ProbabilityToMask(float p);

}  // namespace ixc::seg
