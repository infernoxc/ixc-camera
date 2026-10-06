#pragma once

// Live resource use of the IXC app process, shown at the bottom of the window.
//
// CPU: share of the whole machine, like Task Manager (process CPU time / wall time / logical
// processors). RAM: private bytes (memory committed by this process only, not shared DLLs); close to the
// "Memory" column in Task Manager. Sampled once per
// second from a timer; each sample is two system calls.
//
// The IXC Camera system camera runs inside the Windows camera service (another process), so its
// cost isn't included here; ixc_probe and docs/performance.md measure it separately.

#include <windows.h>

#include <string>
#include <vector>

namespace ixc::app {

struct PerfSample {
    double cpuPercent = 0;  // 0..100 of the whole machine
    double ramMB = 0;       // private bytes
    bool valid = false;     // false for the first sample (no CPU interval yet)
    double gpuPercent = -1; // busiest GPU engine used by this process, 0..100 (-1 = unknown)
    double vramMB = -1;     // dedicated video memory of this process (-1 = unknown)
};

class PerfMonitor {
public:
    ~PerfMonitor();
    PerfSample Sample();
    // GPU use comes from the Windows "GPU Engine" / "GPU Process Memory" performance counters
    // (any vendor). Only queried while enabled (the diagnostics view is open).
    void EnableGpuCounters(bool on);
    static std::wstring Format(const PerfSample& s);

private:
    ULONGLONG lastCpu100ns_ = 0;
    ULONGLONG lastWall100ns_ = 0;
    void* query_ = nullptr;  // PDH_HQUERY
    void* gpuEngine_ = nullptr;
    void* gpuMemory_ = nullptr;
    std::vector<unsigned char> buf_;  // counter array scratch (grown, never per sample once sized)
};

}  // namespace ixc::app
