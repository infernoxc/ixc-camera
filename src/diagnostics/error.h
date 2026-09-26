#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace ixc {

// A failure with enough context for a user-facing message and a log line:
//   "IXC Camera could not register the system camera. HRESULT: 0x80070005 (Access is denied.)
//    Stage: RegisterVirtualCamera."
struct Error {
    HRESULT hr = S_OK;
    std::string stage;    // machine-stable operation name, e.g. "RegisterVirtualCamera"
    std::string summary;  // plain-language sentence, e.g. "IXC Camera could not register the system camera."

    bool ok() const { return SUCCEEDED(hr); }
    std::string Describe() const;
};

// "0x80070005"
std::string HResultHex(HRESULT hr);

// System message text for an HRESULT (trimmed, UTF-8); "Unknown error" when Windows has none.
std::string HResultMessage(HRESULT hr);

// How the caller should react to a failure.
enum class ErrorClass {
    Transient,       // retry with bounded backoff may succeed (device busy, timeout)
    DeviceLost,      // camera was removed or invalidated; wait for re-arrival
    AccessDenied,    // privacy settings or permissions; tell the user, never bypass
    Unsupported,     // format/feature not available on this hardware; pick a fallback
    Fatal,           // programming error or corrupted state; stop the affected component
};

ErrorClass ClassifyHResult(HRESULT hr);
std::string_view ToString(ErrorClass c);

}  // namespace ixc
