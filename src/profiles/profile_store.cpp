#include "profiles/profile_store.h"

#include "common/fileio.h"
#include "common/strings.h"
#include "diagnostics/error.h"

#include <algorithm>
#include <array>

namespace ixc {

namespace {

bool IsReservedDeviceName(std::string_view s) {
    static constexpr std::array<std::string_view, 22> kReserved = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return std::find(kReserved.begin(), kReserved.end(), s) != kReserved.end();
}

}  // namespace

ProfileStore::ProfileStore(std::filesystem::path directory) : dir_(std::move(directory)) {}

bool ProfileStore::IsValidStem(std::string_view stem) {
    if (stem.empty() || stem.size() > 48 || stem.front() == '-' || stem.back() == '-') return false;
    if (IsReservedDeviceName(stem)) return false;
    return std::all_of(stem.begin(), stem.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::string ProfileStore::MakeStem(std::string_view name) {
    std::string stem;
    bool dash = false;
    for (const char ch : name) {
        char c = ch;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (ok) {
            stem.push_back(c);
            dash = false;
        } else if (!dash && !stem.empty()) {
            stem.push_back('-');
            dash = true;
        }
        if (stem.size() >= 40) break;
    }
    while (!stem.empty() && stem.back() == '-') stem.pop_back();
    if (stem.empty() || IsReservedDeviceName(stem)) stem = "profile" + (stem.empty() ? std::string() : "-" + stem);
    return stem;
}

std::filesystem::path ProfileStore::PathFor(const std::string& stem) const {
    return dir_ / (Utf8ToWide(stem) + L".json");
}

std::vector<ProfileStore::Entry> ProfileStore::List() const {
    std::vector<Entry> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != L".json") continue;
        std::string stem = WideToUtf8(it->path().stem().wstring());
        if (!IsValidStem(stem)) continue;
        out.push_back({std::move(stem), it->path()});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.stem < b.stem; });
    return out;
}

ProfileLoadResult ProfileStore::Load(const std::string& stem) const {
    ProfileLoadResult r;
    if (!IsValidStem(stem)) {
        r.error = "invalid profile file name";
        return r;
    }
    std::string text;
    const HRESULT hr = ReadFileLimited(PathFor(stem), kMaxProfileFileBytes, text);
    if (FAILED(hr)) {
        r.error = "could not read profile file: " + HResultHex(hr) + " (" + HResultMessage(hr) + ")";
        return r;
    }
    return ProfileFromJson(text);
}

HRESULT ProfileStore::Save(const Profile& profile, std::string& stem) const {
    if (stem.empty()) stem = MakeStem(profile.name);
    if (!IsValidStem(stem)) return E_INVALIDARG;

    Profile copy = profile;
    Validate(copy);  // never persist out-of-range values

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) return HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value()));
    return WriteFileAtomic(PathFor(stem), ProfileToJson(copy));
}

HRESULT ProfileStore::Remove(const std::string& stem) const {
    if (!IsValidStem(stem)) return E_INVALIDARG;
    if (!DeleteFileW(PathFor(stem).c_str())) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

}  // namespace ixc
