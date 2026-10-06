#include "app/updater.h"

#include "common/fileio.h"
#include "common/sha256.h"
#include "common/strings.h"
#include "ixc/version.h"

#include <winhttp.h>

#include <memory>
#include <system_error>

namespace ixc::app {

namespace {

struct InternetHandle {
    HINTERNET h = nullptr;
    ~InternetHandle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// HTTPS GET of `url` into `out` (at most `limit` bytes). Redirects within HTTPS are followed (GitHub
// serves release files from its download CDN).
bool HttpGet(const std::wstring& url, bool api, size_t limit, std::string& out, const std::atomic<bool>& cancel, std::wstring& error) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"invalid update address";
        return false;
    }
    InternetHandle session, connect, request;
    session.h = WinHttpOpen(L"IXC-Camera/" IXC_VERSION_STRING, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        error = L"network unavailable";
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    connect.h = WinHttpConnect(session.h, host, parts.nPort, 0);
    if (connect.h) {
        request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!request.h || !WinHttpSendRequest(request.h, headers, api ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = L"could not reach GitHub (offline?)";
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = L"GitHub answered " + std::to_wstring(status);
        return false;
    }
    out.clear();
    for (;;) {
        if (cancel.load()) {
            error = L"cancelled";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            error = L"download interrupted";
            return false;
        }
        if (avail == 0) break;
        if (out.size() + avail > limit) {
            error = L"unexpectedly large download";
            return false;
        }
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, out.data() + at, avail, &read)) {
            error = L"download interrupted";
            return false;
        }
        out.resize(at + read);
    }
    return true;
}

}  // namespace

Updater::~Updater() {
    cancel_.store(true);
    Join();
}

void Updater::Join() {
    if (worker_.joinable()) worker_.join();
}

void Updater::Check(HWND notify, bool manual) {
    if (busy_.exchange(true)) return;
    Join();
    cancel_.store(false);
    worker_ = std::thread([this, notify, manual] {
        auto result = std::make_unique<UpdateCheckResult>();
        result->manual = manual;
        std::string body;
        const std::wstring api = L"https://api.github.com/repos/" + Utf8ToWide(kUpdateRepository) + L"/releases/latest";
        result->reached = HttpGet(api, true, 1 << 20, body, cancel_, result->error);
        if (result->reached) {
            result->release = ParseLatestRelease(body);
            if (!result->release.ok) result->error = Utf8ToWide(result->release.error);
        }
        busy_.store(false);
        if (cancel_.load() || !PostMessageW(notify, kCheckedMessage, 0, reinterpret_cast<LPARAM>(result.get()))) return;
        result.release();  // the window owns it now
    });
}

void Updater::Download(HWND notify, const ReleaseInfo& release) {
    if (busy_.exchange(true)) return;
    Join();
    cancel_.store(false);
    worker_ = std::thread([this, notify, release] {
        auto result = std::make_unique<UpdateDownloadResult>();
        result->version = release.version;
        std::string sums, installer;
        if (HttpGet(Utf8ToWide(release.checksumUrl), false, 64 << 10, sums, cancel_, result->error)) {
            const std::string expected = FindChecksum(sums, kInstallerAsset);
            if (expected.empty()) {
                result->error = L"the release lists no checksum for the installer";
            } else if (HttpGet(Utf8ToWide(release.installerUrl), false, 256u << 20, installer, cancel_, result->error)) {
                if (Sha256::Hex(installer.data(), installer.size()) != expected) {
                    result->error = L"the downloaded installer does not match its published checksum (not installed)";
                } else {
                    wchar_t temp[MAX_PATH + 1] = {};
                    GetTempPathW(MAX_PATH + 1, temp);
                    const std::filesystem::path dir = std::filesystem::path(temp) / L"IXC Camera Update";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    const auto file = dir / (L"IXC-Camera-Setup-x64-" + Utf8ToWide(release.version) + L".exe");
                    if (SUCCEEDED(WriteFileAtomic(file, installer)) && VerifyFile(file, expected)) {
                        result->ok = true;
                        result->installer = file;
                        result->sha256 = expected;
                    } else {
                        result->error = L"could not save the installer to " + dir.wstring();
                    }
                }
            }
        }
        busy_.store(false);
        if (cancel_.load() || !PostMessageW(notify, kDownloadedMessage, 0, reinterpret_cast<LPARAM>(result.get()))) return;
        result.release();
    });
}

bool Updater::VerifyFile(const std::filesystem::path& file, const std::string& sha256Hex) {
    std::string data;
    if (FAILED(ReadFileLimited(file, 256u << 20, data))) return false;
    return !sha256Hex.empty() && Sha256::Hex(data.data(), data.size()) == sha256Hex;
}

}  // namespace ixc::app
