#pragma once

// In-app updates from the official GitHub releases (infernoxc/ixc-camera).
//
//   * Check: one HTTPS request to the public Releases API on a background thread (never the UI or
//     camera threads); offline or rate-limited simply means "no update information".
//   * Download (only after the user asks): the release's SHA256SUMS.txt, then its installer, into
//     %TEMP%\IXC Camera Update. The installer is accepted only if its SHA-256 matches the listed one
//     and is hashed again right before it's started. URLs are restricted to that release's own
//     GitHub download path (common/update_check.h). Nothing runs without the user's confirmation.
//
// Results arrive as window messages; lParam owns a heap object the receiver deletes.

#include "common/update_check.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace ixc::app {

struct UpdateCheckResult {
    bool reached = false;  // the server answered (false: offline, blocked, rate-limited)
    bool manual = false;   // the user pressed "Check for updates"
    ReleaseInfo release;   // release.ok: a valid release (may or may not be newer)
    std::wstring error;
};
struct UpdateDownloadResult {
    bool ok = false;
    std::filesystem::path installer;  // verified installer
    std::string version;
    std::string sha256;  // verified digest (checked again right before launch)
    std::wstring error;
};

class Updater {
public:
    static constexpr UINT kCheckedMessage = WM_APP + 30;     // lParam: UpdateCheckResult*
    static constexpr UINT kDownloadedMessage = WM_APP + 31;  // lParam: UpdateDownloadResult*

    ~Updater();
    bool Busy() const { return busy_.load(); }
    // Starts a check (ignored while busy).
    void Check(HWND notify, bool manual);
    // Starts the verified download of `release` (ignored while busy).
    void Download(HWND notify, const ReleaseInfo& release);

    // Right before launching: the file on disk still has the verified digest.
    static bool VerifyFile(const std::filesystem::path& file, const std::string& sha256Hex);

private:
    void Join();
    std::thread worker_;
    std::atomic<bool> busy_{false}, cancel_{false};
};

}  // namespace ixc::app
