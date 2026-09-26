#pragma once

// Frame-timing statistics with fixed memory (no allocation per frame). Pure logic: callers
// pass timestamps in, which keeps it deterministic for tests.

#include <array>
#include <cstdint>

namespace ixc::camera {

struct FrameStatsSnapshot {
    std::uint64_t framesReceived = 0;
    std::uint64_t stalls = 0;              // arrival gaps > stall threshold
    std::uint64_t timestampJumps = 0;      // device timestamps going backwards or skipping ahead
    std::uint64_t streamTicks = 0;         // source-signalled gaps (MF_SOURCE_READERF_STREAMTICK)
    double fps = 0;                        // arrival rate over the recent window
    double meanIntervalMs = 0;             // recent window
    double maxIntervalMs = 0;              // recent window
    double jitterMs = 0;                   // standard deviation of recent intervals
    double lastLatencyMs = -1;             // capture→delivery, when the device timestamp is usable
    double meanLatencyMs = -1;
    double nominalFps = 0;                 // rate of the negotiated format
    // True when a full window of frames arrived clearly slower than the negotiated rate.
    // Most webcams do this in low light: auto exposure lengthens the exposure time.
    bool underSpeed = false;
};

class FrameStats {
public:
    // expectedIntervalUs: nominal frame interval of the negotiated format.
    explicit FrameStats(std::int64_t expectedIntervalUs = 33333);

    void Reset(std::int64_t expectedIntervalUs);

    // arrivalUs: monotonic arrival time. sampleTimeUs: presentation time from the source.
    // latencyUs: capture→delivery latency when known, otherwise negative.
    void OnFrame(std::int64_t arrivalUs, std::int64_t sampleTimeUs, std::int64_t latencyUs = -1);
    void OnStreamTick() { ++snap_.streamTicks; }

    FrameStatsSnapshot Snapshot() const;

    std::int64_t StallThresholdUs() const { return stallThresholdUs_; }

private:
    static constexpr size_t kWindow = 64;

    std::int64_t expectedUs_ = 33333;
    std::int64_t stallThresholdUs_ = 250000;
    std::array<std::int64_t, kWindow> arrivals_{};
    size_t count_ = 0;  // valid entries in arrivals_ (<= kWindow)
    size_t head_ = 0;   // next write position
    std::int64_t lastArrival_ = 0;
    std::int64_t lastSampleTime_ = 0;
    bool haveLast_ = false;
    double latencySumMs_ = 0;
    std::uint64_t latencyCount_ = 0;
    FrameStatsSnapshot snap_;
};

}  // namespace ixc::camera
