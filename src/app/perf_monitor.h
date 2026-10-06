#pragma once

// Live resource use of the IXC app process, shown at the bottom of the window.
//
// CPU: share of the whole machine, like Task Manager (process CPU time / wall time / logical
// processors). RAM: private working set, the "Memory" column in Task Manager. Sampled once per
// second from a timer; each sample is two system calls.
//
// The IXC Camera system camera runs inside the Windows camera service (another process), so its
// cost isn't included here; ixc_probe and docs/performance.md measure it separately.

#include <windows.h>

#include <string>

namespace ixc::app {

struct PerfSample {
    double cpuPercent = 0;  // 0..100 of the whole machine
    double ramMB = 0;       // private working set
    bool valid = false;     // false for the first sample (no CPU interval yet)
};

class PerfMonitor {
public:
    PerfSample Sample();
    static std::wstring Format(const PerfSample& s);

private:
    ULONGLONG lastCpu100ns_ = 0;
    ULONGLONG lastWall100ns_ = 0;
};

}  // namespace ixc::app
