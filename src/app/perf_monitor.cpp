#include "app/perf_monitor.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>

namespace ixc::app {

namespace {
ULONGLONG ToU64(const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
}  // namespace

PerfMonitor::~PerfMonitor() { EnableGpuCounters(false); }

void PerfMonitor::EnableGpuCounters(bool on) {
    if (!on) {
        if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
        query_ = gpuEngine_ = gpuMemory_ = nullptr;
        std::vector<unsigned char>().swap(buf_);
        return;
    }
    if (query_) return;
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    wchar_t path[128];
    PDH_HCOUNTER engine = nullptr, memory = nullptr;
    swprintf_s(path, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", GetCurrentProcessId());
    PdhAddEnglishCounterW(q, path, 0, &engine);
    swprintf_s(path, L"\\GPU Process Memory(pid_%lu_*)\\Dedicated Usage", GetCurrentProcessId());
    PdhAddEnglishCounterW(q, path, 0, &memory);
    PdhCollectQueryData(q);  // first collection: rates need two
    query_ = q;
    gpuEngine_ = engine;
    gpuMemory_ = memory;
}

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
    if (query_ && PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) == ERROR_SUCCESS) {
        // Sum per instance group: engines report separately, so the busiest engine is the
        // honest "GPU %" (what Task Manager shows per process); memory adds up across adapters.
        auto read = [&](void* counter, bool max) -> double {
            if (!counter) return -1;
            DWORD bytes = 0, count = 0;
            PDH_STATUS st = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, &bytes, &count, nullptr);
            if (st != PDH_MORE_DATA || bytes == 0) return count == 0 && st == ERROR_SUCCESS ? 0 : -1;
            if (buf_.size() < bytes) buf_.resize(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data());
            if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
            double v = 0;
            for (DWORD i = 0; i < count; ++i) {
                if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                v = max ? std::max(v, items[i].FmtValue.doubleValue) : v + items[i].FmtValue.doubleValue;
            }
            return v;
        };
        s.gpuPercent = read(gpuEngine_, true);
        const double bytes = read(gpuMemory_, false);
        s.vramMB = bytes >= 0 ? bytes / (1024.0 * 1024.0) : -1;
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
