#pragma once

// Per-user app settings that aren't part of a camera profile (%LOCALAPPDATA%\IXC Camera\app-settings.json).
// Lenient by design: a missing or damaged file gives defaults (it holds no user content worth
// protecting; profiles themselves are never reset silently).

#include <string>
#include <string_view>

namespace ixc {

struct AppSettings {
    std::string activeProfile = "default";  // profile store stem
    bool hotkeysEnabled = true;             // global Ctrl+Alt hotkeys (see docs/hotkeys in README)
    bool showFaceMarkers = true;            // preview overlay of tracked faces (preview only)
    double lastUpdateCheck = 0;             // Unix time of the last automatic update check (once a day)
    std::string skippedVersion;             // "Skip this version" (no reminder for that release)
    bool operator==(const AppSettings&) const = default;
};

AppSettings AppSettingsFromJson(std::string_view text);
std::string AppSettingsToJson(const AppSettings& s);

}  // namespace ixc
