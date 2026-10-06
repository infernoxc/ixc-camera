#include "app/perf_monitor.h"

#include <psapi.h>

#include <cstdio>

namespace ixc::app {

namespace {
ULONGLONG ToU64(const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
}  // namespace

PerfSample PerfMonitor::Sample() {
    PerfSample s;
    FILETIME created, exited, kernel, user, now;
    GetSystemTimeAsFileTime(&now);
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const ULONGLONG cpu = ToU64(kernel) + ToU64(user), wall = ToU64(now);
        if (lastWall100ns_ != 0 && wall > lastWall100ns_) {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const double cores = si.dwNumberOfProcessors > 0 ? static_cast<double>(si.dwNumberOfProcessors) : 1.0;
            s.cpuPercent = 100.0 * static_cast<double>(cpu - lastCpu100ns_) / static_cast<double>(wall - lastWall100ns_) / cores;
            s.valid = true;
        }
        lastCpu100ns_ = cpu;
        lastWall100ns_ = wall;
    }
    PROCESS_MEMORY_COUNTERS_EX mc{};
    mc.cb = sizeof(mc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof(mc))) {
        s.ramMB = static_cast<double>(mc.PrivateUsage) / (1024.0 * 1024.0);
    }
    return s;
}

std::wstring PerfMonitor::Format(const PerfSample& s) {
    wchar_t b[96];
    if (s.valid) swprintf_s(b, L"App: CPU %.1f%%  ·  RAM %.0f MB", s.cpuPercent, s.ramMB);
    else swprintf_s(b, L"App: CPU …  ·  RAM %.0f MB", s.ramMB);
    return b;
}

}  // namespace ixc::app
