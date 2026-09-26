#include "diagnostics/error.h"

#include "common/strings.h"

#include <mferror.h>

#include <cstdio>

namespace ixc {

std::string HResultHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string HResultMessage(HRESULT hr) {
    wchar_t* text = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);

    // Media Foundation error strings live in mferror's message table (mfplat.dll).
    if (n == 0 && HRESULT_FACILITY(hr) == FACILITY_MF) {
        if (HMODULE mf = GetModuleHandleW(L"mfplat.dll")) {
            flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
            n = FormatMessageW(flags, mf, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
        }
    }
    if (n == 0 || !text) return "Unknown error";

    std::wstring w(text, n);
    LocalFree(text);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return WideToUtf8(w) + ".";
}

std::string Error::Describe() const {
    std::string s = summary.empty() ? std::string("IXC Camera operation failed.") : summary;
    s += " HRESULT: ";
    s += HResultHex(hr);
    s += " (";
    s += HResultMessage(hr);
    s += ") Stage: ";
    s += stage.empty() ? std::string("Unknown") : stage;
    s += ".";
    return s;
}

ErrorClass ClassifyHResult(HRESULT hr) {
    switch (hr) {
        case E_ACCESSDENIED:
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY):
            return ErrorClass::AccessDenied;

        case MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED:
        case MF_E_HW_MFT_FAILED_START_STREAMING:
        case HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED):
        case HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED):
        case HRESULT_FROM_WIN32(ERROR_GEN_FAILURE):
            return ErrorClass::DeviceLost;

        case MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED:
        case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
        case HRESULT_FROM_WIN32(ERROR_BUSY):
        case HRESULT_FROM_WIN32(ERROR_TIMEOUT):
        case E_PENDING:
            return ErrorClass::Transient;

        case MF_E_INVALIDMEDIATYPE:
        case MF_E_UNSUPPORTED_FORMAT:
        case MF_E_TOPO_CODEC_NOT_FOUND:
        case E_NOTIMPL:
        case DXGI_ERROR_UNSUPPORTED:
            return ErrorClass::Unsupported;

        default:
            return ErrorClass::Fatal;
    }
}

std::string_view ToString(ErrorClass c) {
    switch (c) {
        case ErrorClass::Transient: return "Transient";
        case ErrorClass::DeviceLost: return "DeviceLost";
        case ErrorClass::AccessDenied: return "AccessDenied";
        case ErrorClass::Unsupported: return "Unsupported";
        case ErrorClass::Fatal: return "Fatal";
    }
    return "Fatal";
}

}  // namespace ixc
