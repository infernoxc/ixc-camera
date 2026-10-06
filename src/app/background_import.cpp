#include "app/background_import.h"

#include "effects/background_image.h"
#include "effects/backgrounds.h"
#include "profiles/active_profile.h"
#include "profiles/profile.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace ixc::app {

namespace {

// "My Beach Photo.jpg" → "my-beach-photo-1a2b": valid name ([a-z0-9-]), unique per file content.
std::string NameFor(const std::filesystem::path& file, std::uint32_t hash) {
    std::string base;
    for (wchar_t c : file.stem().wstring()) {
        if (base.size() >= 40) break;
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) base.push_back(static_cast<char>(c));
        else if (!base.empty() && base.back() != '-') base.push_back('-');
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "picture";
    char tail[16];
    std::snprintf(tail, sizeof tail, "-%04x", hash & 0xFFFFu);
    return base + tail;
}

}  // namespace

std::string ImportBackground(const std::filesystem::path& file, std::wstring& error) {
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w < 16 || h < 16) {
        error = L"This picture can't be opened (JPG, PNG, WEBP or BMP; WEBP needs the Windows WebP extension).";
        return {};
    }
    if (w > 16384 || h > 16384) {
        error = L"This picture is too large (more than 16384 pixels on a side).";
        return {};
    }
    // Scale on decode to at most 2x the stored size (cheap, keeps memory low), then the area
    // downscale in MakeBackgroundImage does the rest.
    ComPtr<IWICBitmapSource> source = frame;
    const double fit = std::min(1.0, std::min(2.0 * effects::kMaxBackgroundW / w, 2.0 * effects::kMaxBackgroundH / h));
    if (fit < 1.0) {
        ComPtr<IWICBitmapScaler> scaler;
        const UINT sw = std::max(16u, static_cast<UINT>(w * fit)), sh = std::max(16u, static_cast<UINT>(h * fit));
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant))) {
            source = scaler;
            w = sw;
            h = sh;
        }
    }
    ComPtr<IWICFormatConverter> conv;
    hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(source.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    std::vector<std::uint8_t> rgb;
    const UINT stride = w * 3;
    if (SUCCEEDED(hr)) {
        try {
            rgb.resize(static_cast<size_t>(stride) * h);
        } catch (const std::bad_alloc&) {
            hr = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) hr = conv->CopyPixels(nullptr, stride, static_cast<UINT>(rgb.size()), rgb.data());
    if (FAILED(hr)) {
        error = L"This picture could not be decoded.";
        return {};
    }
    const auto img = effects::MakeBackgroundImage(rgb.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(stride));
    if (!img) {
        error = L"This picture could not be converted.";
        return {};
    }
    std::uint32_t hash = 2166136261u;  // FNV-1a over a sample of the pixels
    for (size_t i = 0; i < img->y.size(); i += 97) hash = (hash ^ img->y[i]) * 16777619u;
    const std::string name = NameFor(file, hash);
    const auto dir = BackgroundsDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!effects::SaveBackgroundImage(effects::CustomBackgroundFile(dir, name), *img)) {
        error = L"The picture could not be saved to " + dir.wstring() + L" (reinstalling IXC Camera repairs the settings folder).";
        return {};
    }
    TouchBackground(name);
    RecentBackgrounds();  // trims the folder to the most recent pictures
    return name;
}

std::vector<std::string> RecentBackgrounds() {
    struct Item {
        std::string name;
        std::filesystem::file_time_type time;
    };
    std::vector<Item> items;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(BackgroundsDirectory(), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != L".ixbg") continue;
        std::string n;
        bool ascii = true;
        for (wchar_t c : e.path().stem().wstring()) {
            ascii = ascii && c < 128;
            n.push_back(static_cast<char>(c & 0x7F));
        }
        if (!ascii || !IsValidBackgroundName(n)) continue;  // only names IXC wrote
        items.push_back({n, e.last_write_time(ec)});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.time > b.time; });
    std::vector<std::string> out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i < kMaxRecentBackgrounds) out.push_back(items[i].name);
        else RemoveBackground(items[i].name);  // disk use stays bounded
    }
    return out;
}

void TouchBackground(const std::string& name) {
    if (!IsValidBackgroundName(name)) return;
    std::error_code ec;
    std::filesystem::last_write_time(effects::CustomBackgroundFile(BackgroundsDirectory(), name), std::filesystem::file_time_type::clock::now(), ec);
}

bool RemoveBackground(const std::string& name) {
    if (!IsValidBackgroundName(name)) return false;
    std::error_code ec;
    return std::filesystem::remove(effects::CustomBackgroundFile(BackgroundsDirectory(), name), ec);
}

}  // namespace ixc::app
