#include "common/fileio.h"

#include <algorithm>
#include <climits>

namespace ixc {

namespace {

class FileHandle {
public:
    explicit FileHandle(HANDLE h) : h_(h) {}
    ~FileHandle() { if (valid()) CloseHandle(h_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h_; }

private:
    HANDLE h_;
};

HRESULT LastErrorHr() { return HRESULT_FROM_WIN32(GetLastError()); }

}  // namespace

HRESULT ReadFileLimited(const std::filesystem::path& path, size_t maxBytes, std::string& out) {
    out.clear();
    FileHandle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!f.valid()) return LastErrorHr();

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f.get(), &size)) return LastErrorHr();
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(f.get(), out.data() + done, chunk, &read, nullptr)) { out.clear(); return LastErrorHr(); }
        if (read == 0) break;  // file shrank while reading
        done += read;
    }
    out.resize(done);
    return S_OK;
}

HRESULT WriteFileAtomic(const std::filesystem::path& path, std::string_view data) {
    if (data.size() > static_cast<size_t>(UINT_MAX)) return E_INVALIDARG;
    std::filesystem::path tmp = path;
    tmp += L".tmp";

    {
        FileHandle f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!f.valid()) return LastErrorHr();
        DWORD written = 0;
        if (!WriteFile(f.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
            written != data.size()) {
            const HRESULT hr = LastErrorHr();
            DeleteFileW(tmp.c_str());
            return FAILED(hr) ? hr : E_FAIL;
        }
        if (!FlushFileBuffers(f.get())) {
            const HRESULT hr = LastErrorHr();
            DeleteFileW(tmp.c_str());
            return hr;
        }
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const HRESULT hr = LastErrorHr();
        DeleteFileW(tmp.c_str());
        return hr;
    }
    return S_OK;
}

}  // namespace ixc
