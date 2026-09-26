#pragma once

// Synchronous, size-bounded file log. No background thread, no buffering queue.
//
// Normal mode records major events only; Debug mode adds pipeline detail. Callers must never
// log secrets, tokens, browsing data or anything unrelated to IXC.
//
// Rotation: when the active file exceeds maxFileBytes it is renamed to <name>.1.log
// (shifting older files up) and at most `maxBackups` old files are kept, so total disk use is
// bounded by roughly (maxBackups + 1) * maxFileBytes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ixc::log {

enum class Level { Error, Warning, Info, Debug };
enum class Mode { Normal, Debug };

struct Config {
    std::filesystem::path directory;   // created if missing
    std::wstring baseName = L"ixc";    // produces ixc.log, ixc.1.log, ...
    std::uint64_t maxFileBytes = 1u << 20;
    int maxBackups = 2;
    Mode mode = Mode::Normal;
};

// Returns false if the log file cannot be opened; logging then becomes a no-op.
bool Init(const Config& config);
void Shutdown();
void SetMode(Mode mode);
bool Enabled(Level level);

void Write(Level level, std::string_view component, std::string_view message);

inline void Error(std::string_view component, std::string_view msg) { Write(Level::Error, component, msg); }
inline void Warn(std::string_view component, std::string_view msg) { Write(Level::Warning, component, msg); }
inline void Info(std::string_view component, std::string_view msg) { Write(Level::Info, component, msg); }
inline void Debug(std::string_view component, std::string_view msg) {
    if (Enabled(Level::Debug)) Write(Level::Debug, component, msg);
}

}  // namespace ixc::log
