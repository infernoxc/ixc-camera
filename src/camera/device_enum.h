#pragma once

#include "camera/camera_types.h"

#include <mfidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace ixc::camera {

// MFStartup/MFShutdown for the lifetime of the object. COM must already be initialized.
class MediaFoundationScope {
public:
    MediaFoundationScope();
    ~MediaFoundationScope();
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

// Lists video capture devices (physical and virtual). Does not open any camera.
HRESULT EnumerateCameras(std::vector<CameraInfo>& out);

// Creates (activates) the media source for a camera. The caller must call Shutdown() on it.
HRESULT CreateCameraSource(const std::wstring& symbolicLink, Microsoft::WRL::ComPtr<IMFMediaSource>& source);

// Native modes of stream 0, in the source's own order (nativeIndex filled in). Not normalized.
HRESULT EnumerateFormats(IMFMediaSource* source, std::vector<CaptureFormat>& out);

// Convenience: opens the camera briefly, enumerates, shuts the source down again.
HRESULT EnumerateFormats(const std::wstring& symbolicLink, std::vector<CaptureFormat>& out);

// Reads subtype, size and frame rate from a media type. Returns false for non-video types.
bool FormatFromMediaType(IMFMediaType* type, CaptureFormat& out);

// Case-insensitive comparison of the device-instance part of two symbolic links
// ("\\?\usb#vid_..&pid_..#instance#{interface-class}"), ignoring the interface class GUID so a
// KSCATEGORY_VIDEO_CAMERA link matches the KSCATEGORY_CAPTURE link of the same device.
bool SameDevice(const std::wstring& a, const std::wstring& b);

}  // namespace ixc::camera
