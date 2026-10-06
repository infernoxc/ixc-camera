#pragma once

// Update check logic (pure; the network part is in app/updater.cpp).
//
// Only releases of the official repository are accepted, and only their installer asset; its
// download URL must be that release's own GitHub download URL, and the installer is run only after
// its SHA-256 matches the release's SHA256SUMS.txt.

#include <string>
#include <string_view>

namespace ixc {

inline constexpr std::string_view kUpdateRepository = "infernoxc/ixc-camera";
inline constexpr std::string_view kInstallerAsset = "IXC-Camera-Setup-x64.exe";
inline constexpr std::string_view kChecksumAsset = "SHA256SUMS.txt";

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    bool ok = false;
};
SemVer ParseSemVer(std::string_view text);  // "1.2.3" or "v1.2.3" (pre-release/build suffixes rejected)
int CompareSemVer(const SemVer& a, const SemVer& b);  // <0, 0, >0 (numeric, not string order)

struct ReleaseInfo {
    bool ok = false;
    std::string error;
    std::string version;       // "0.14.0"
    std::string name;
    std::string notes;         // release body (Markdown), trimmed for display
    std::string installerUrl;  // https://github.com/<repo>/releases/download/v<version>/IXC-Camera-Setup-x64.exe
    std::string checksumUrl;   // .../SHA256SUMS.txt
};
// Parses GitHub's "latest release" JSON for `repo`. Drafts, pre-releases, unexpected tags and
// asset URLs outside the release's own download path are rejected.
ReleaseInfo ParseLatestRelease(std::string_view json, std::string_view repo = kUpdateRepository);

// The lowercase hex SHA-256 of `file` in a sha256sum-style list ("<hex>  <name>" lines), "" if absent.
std::string FindChecksum(std::string_view sums, std::string_view file);

}  // namespace ixc
