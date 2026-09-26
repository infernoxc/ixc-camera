#pragma once

// The "active profile" is the copy of the user's current profile that the IXC Camera source
// (running inside the Windows Frame Server service) applies to video. The service account
// can't read the user's %LOCALAPPDATA%, so the app publishes it to
//   %ProgramData%\IXC Camera\active-profile.json
// The installer creates that folder: Users may write, LOCAL SERVICE may read.
//
// The file crosses a privilege boundary (written by a user, read by a service), so the reader
// treats it as hostile: size-limited, strict JSON, every value clamped (ProfileFromJson).

#include "profiles/profile.h"

#include <windows.h>

#include <filesystem>
#include <string>

namespace ixc {

inline constexpr size_t kMaxActiveProfileBytes = 64 * 1024;

std::filesystem::path ActiveProfileDirectory();  // %ProgramData%\IXC Camera
std::filesystem::path ActiveProfilePath();       // ...\active-profile.json

// Writes the profile atomically. Fails with ACCESS_DENIED when IXC Camera isn't installed
// (the folder's permissions come from the installer).
HRESULT PublishActiveProfile(const Profile& profile);

// Reads and validates the published profile. Returns ok == false when absent or invalid, never
// a half-parsed profile. *missing (optional) tells "no file" (use neutral settings) apart from
// "file present but unreadable/invalid" (e.g. caught mid-write: keep the last good settings).
ProfileLoadResult LoadActiveProfile(bool* missing = nullptr);

}  // namespace ixc
