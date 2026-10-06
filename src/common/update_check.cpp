#include "common/update_check.h"

#include "common/json.h"

#include <cctype>

namespace ixc {

SemVer ParseSemVer(std::string_view t) {
    SemVer v;
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t.remove_prefix(1);
    int parts[3] = {0, 0, 0};
    int idx = 0;
    size_t i = 0;
    while (idx < 3) {
        if (i >= t.size() || !std::isdigit(static_cast<unsigned char>(t[i]))) return v;
        long n = 0;
        size_t digits = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
            n = n * 10 + (t[i] - '0');
            if (++digits > 6) return v;  // absurd component
            ++i;
        }
        parts[idx++] = static_cast<int>(n);
        if (idx < 3) {
            if (i >= t.size() || t[i] != '.') return v;
            ++i;
        }
    }
    if (i != t.size()) return v;  // trailing text (pre-release tags etc.) is not an update target
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.ok = true;
    return v;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

ReleaseInfo ParseLatestRelease(std::string_view text, std::string_view repo) {
    ReleaseInfo r;
    const json::ParseResult p = json::Parse(text);
    if (!p || !p.value->IsObject()) {
        r.error = "unreadable release information";
        return r;
    }
    const json::Value& o = *p.value;
    auto str = [&](const char* key) -> std::string {
        const json::Value* v = o.Find(key);
        return v && v->IsString() ? v->AsString() : std::string();
    };
    auto flag = [&](const char* key) {
        const json::Value* v = o.Find(key);
        return v && v->IsBool() && v->AsBool();
    };
    if (flag("draft") || flag("prerelease")) {
        r.error = "not a final release";
        return r;
    }
    const std::string tag = str("tag_name");
    const SemVer v = ParseSemVer(tag);
    if (!v.ok || tag.empty() || tag[0] != 'v') {
        r.error = "unexpected release tag";
        return r;
    }
    r.version = tag.substr(1);
    r.name = str("name");
    r.notes = str("body");
    if (r.notes.size() > 4000) r.notes = r.notes.substr(0, 4000) + "…";
    const std::string prefix = "https://github.com/" + std::string(repo) + "/releases/download/" + tag + "/";
    const json::Value* assets = o.Find("assets");
    if (assets && assets->IsArray()) {
        for (const json::Value& a : assets->AsArray()) {
            if (!a.IsObject()) continue;
            const json::Value* n = a.Find("name");
            const json::Value* u = a.Find("browser_download_url");
            if (!n || !u || !n->IsString() || !u->IsString()) continue;
            const std::string& url = u->AsString();
            if (url.rfind(prefix, 0) != 0 || url.size() != prefix.size() + n->AsString().size() || url.substr(prefix.size()) != n->AsString()) continue;
            if (n->AsString() == kInstallerAsset) r.installerUrl = url;
            else if (n->AsString() == kChecksumAsset) r.checksumUrl = url;
        }
    }
    if (r.installerUrl.empty() || r.checksumUrl.empty()) {
        r.error = "the release has no verifiable installer";
        return r;
    }
    r.ok = true;
    return r;
}

std::string FindChecksum(std::string_view sums, std::string_view file) {
    size_t pos = 0;
    while (pos < sums.size()) {
        size_t end = sums.find('\n', pos);
        if (end == std::string_view::npos) end = sums.size();
        std::string_view line = sums.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.size() < 66) continue;
        std::string_view hex = line.substr(0, 64), rest = line.substr(64);
        size_t s = 0;
        while (s < rest.size() && (rest[s] == ' ' || rest[s] == '*' || rest[s] == '\t')) ++s;
        if (s == 0 || rest.substr(s) != file) continue;
        std::string out;
        for (char c : hex) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    }
    return {};
}

}  // namespace ixc
