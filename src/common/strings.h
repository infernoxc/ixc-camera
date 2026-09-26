#pragma once

#include <string>
#include <string_view>

namespace ixc {

// Internal text is UTF-8 (std::string). Conversions happen only at Win32 API boundaries.
// Invalid input sequences are replaced with U+FFFD rather than throwing.
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

// Returns true when `s` is well-formed UTF-8 (no overlongs, no surrogates, <= U+10FFFF).
bool IsValidUtf8(std::string_view s);

}  // namespace ixc
