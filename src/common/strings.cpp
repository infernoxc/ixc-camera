#include "common/strings.h"

#include <windows.h>

#include <climits>

namespace ixc {

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > static_cast<size_t>(INT_MAX)) return {};
    const int src = static_cast<int>(utf8.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src, out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > static_cast<size_t>(INT_MAX)) return {};
    const int src = static_cast<int>(wide.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), src, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), src, out.data(), n, nullptr, nullptr);
    return out;
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }

        size_t len = 0;
        char32_t cp = 0;
        char32_t min = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return false;

        if (n - i < len) return false;
        for (size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

}  // namespace ixc
