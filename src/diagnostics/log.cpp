#include "diagnostics/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace ixc::log {

namespace {

struct State {
    std::mutex mu;
    Config config;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::uint64_t size = 0;
};

State& S() {
    static State s;
    return s;
}

std::atomic<Mode> g_mode{Mode::Normal};

std::filesystem::path FileFor(const Config& c, int index) {
    std::wstring name = c.baseName;
    if (index > 0) name += L"." + std::to_wstring(index);
    name += L".log";
    return c.directory / name;
}

// Caller holds the mutex.
bool OpenLocked(State& s) {
    s.file = CreateFileW(FileFor(s.config, 0).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    s.size = GetFileSizeEx(s.file, &sz) ? static_cast<std::uint64_t>(sz.QuadPart) : 0;
    return true;
}

void CloseLocked(State& s) {
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
}

void RotateLocked(State& s) {
    CloseLocked(s);
    const Config& c = s.config;
    DeleteFileW(FileFor(c, c.maxBackups).c_str());
    for (int i = c.maxBackups - 1; i >= 0; --i) {
        MoveFileExW(FileFor(c, i).c_str(), FileFor(c, i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (c.maxBackups <= 0) DeleteFileW(FileFor(c, 0).c_str());
    OpenLocked(s);
}

const char* LevelName(Level l) {
    switch (l) {
        case Level::Error: return "ERROR";
        case Level::Warning: return "WARN ";
        case Level::Info: return "INFO ";
        case Level::Debug: return "DEBUG";
    }
    return "?????";
}

}  // namespace

bool Init(const Config& config) {
    State& s = S();
    std::lock_guard lock(s.mu);
    CloseLocked(s);
    s.config = config;
    if (s.config.maxBackups < 0) s.config.maxBackups = 0;
    if (s.config.maxFileBytes < 4096) s.config.maxFileBytes = 4096;
    g_mode.store(config.mode);

    std::error_code ec;
    std::filesystem::create_directories(s.config.directory, ec);
    return OpenLocked(s);
}

void Shutdown() {
    State& s = S();
    std::lock_guard lock(s.mu);
    CloseLocked(s);
}

void SetMode(Mode mode) { g_mode.store(mode); }

bool Enabled(Level level) { return level != Level::Debug || g_mode.load() == Mode::Debug; }

void Write(Level level, std::string_view component, std::string_view message) {
    if (!Enabled(level)) return;

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char prefix[64];
    const int pn = std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u %s [%5lu] ",
                                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                                 LevelName(level), GetCurrentThreadId());

    std::string line;
    line.reserve(static_cast<size_t>(pn > 0 ? pn : 0) + component.size() + message.size() + 4);
    if (pn > 0) line.append(prefix, static_cast<size_t>(pn));
    line.append(component);
    line.append(": ");
    // Keep one event per line so logs stay greppable and cannot be forged by embedded newlines.
    for (const char c : message) line.push_back((c == '\r' || c == '\n') ? ' ' : c);
    line.append("\r\n");

    State& s = S();
    std::lock_guard lock(s.mu);
    if (s.file == INVALID_HANDLE_VALUE) return;
    if (s.size + line.size() > s.config.maxFileBytes && s.size > 0) {
        RotateLocked(s);
        if (s.file == INVALID_HANDLE_VALUE) return;
    }
    DWORD written = 0;
    if (WriteFile(s.file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        s.size += written;
    }
}

}  // namespace ixc::log
