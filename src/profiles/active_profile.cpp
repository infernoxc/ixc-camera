#include "profiles/active_profile.h"

#include "common/fileio.h"
#include "diagnostics/error.h"

namespace ixc {

std::filesystem::path ActiveProfileDirectory() {
    // %ProgramData% is a system variable, so it's also set in the Frame Server service. This
    // avoids loading shell32 into the service just to resolve one folder.
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"IXC Camera";
}

std::filesystem::path ActiveProfilePath() {
    const auto dir = ActiveProfileDirectory();
    return dir.empty() ? dir : dir / L"active-profile.json";
}

HRESULT PublishActiveProfile(const Profile& profile) {
    const auto path = ActiveProfilePath();
    if (path.empty()) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    Profile copy = profile;
    Validate(copy);
    return WriteFileAtomic(path, ProfileToJson(copy));
}

ProfileLoadResult LoadActiveProfile(bool* missing) {
    ProfileLoadResult r;
    const auto path = ActiveProfilePath();
    std::string text;
    const HRESULT hr = path.empty() ? HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) : ReadFileLimited(path, kMaxActiveProfileBytes, text);
    if (missing) *missing = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if (FAILED(hr)) {
        r.error = "active profile unavailable: " + HResultHex(hr);
        return r;
    }
    return ProfileFromJson(text);
}

}  // namespace ixc
