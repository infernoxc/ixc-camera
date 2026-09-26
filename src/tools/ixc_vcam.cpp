// ixc_vcam — manage the IXC Camera system camera (used by the installer and for diagnostics).
//
//   ixc_vcam status
//   ixc_vcam register [--camera <index|name-substring|symbolic-link>]   (administrator)
//   ixc_vcam unregister                                    (administrator)
//
// Exit codes: 0 success, 1 failure, 2 usage error, 3 administrator rights required.

#include "camera/device_enum.h"
#include "common/strings.h"
#include "diagnostics/error.h"
#include "virtual_camera/registration.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ixc;

namespace {

constexpr int kExitNeedsAdmin = 3;

int PrintStatus() {
    const vcam::Status s = vcam::QueryStatus();
    std::printf("media source registered: %s\n", s.comRegistered ? "yes" : "no");
    if (s.comRegistered) {
        std::printf("  dll: %s (%s)\n", WideToUtf8(s.registeredDllPath).c_str(), s.dllFileExists ? "present" : "MISSING");
        if (s.dllFileExists && !s.dllReadableByService) {
            std::printf("  WARNING: the DLL is inside a user profile; the Windows camera service cannot load it from there.\n");
        }
    }
    std::printf("system camera present:  %s\n", s.cameraPresent ? "yes" : "no");
    if (s.cameraPresent) {
        std::printf("  name: %s\n  link: %s\n", s.cameraName.c_str(), WideToUtf8(s.cameraLink).c_str());
        if (!s.wrappedCameraName.empty()) std::printf("  wraps: %s\n", s.wrappedCameraName.c_str());
        if (!s.wrappedCameraLink.empty()) std::printf("         %s\n", WideToUtf8(s.wrappedCameraLink).c_str());
    }
    return 0;
}

bool PickPhysical(const std::string& selector, camera::CameraInfo& out) {
    std::vector<camera::CameraInfo> cams;
    if (FAILED(camera::EnumerateCameras(cams))) return false;
    std::vector<camera::CameraInfo> physical;
    for (const auto& c : cams) {
        if (!c.isSoftwareDevice && !vcam::IsIxcCameraLink(c.symbolicLink, c.name)) physical.push_back(c);
    }
    if (physical.empty()) {
        std::printf("error: no physical camera found. Connect a webcam and try again.\n");
        return false;
    }
    if (selector.empty()) {
        out = physical.front();
        return true;
    }
    char* end = nullptr;
    const long idx = std::strtol(selector.c_str(), &end, 10);
    if (end && *end == '\0' && idx >= 0 && static_cast<size_t>(idx) < physical.size()) {
        out = physical[static_cast<size_t>(idx)];
        return true;
    }
    for (const auto& c : physical) {
        if (camera::SameDevice(c.symbolicLink, Utf8ToWide(selector))) {
            out = c;
            return true;
        }
    }
    for (const auto& c : physical) {
        if (c.name.find(selector) != std::string::npos) {
            out = c;
            return true;
        }
    }
    std::printf("error: no physical camera matches \"%s\"\n", selector.c_str());
    return false;
}

int Fail(const Error& e) {
    std::printf("error: %s\n", e.Describe().c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: ixc_vcam status | register [--camera X] | unregister\n");
        return 2;
    }
    const std::string cmd = argv[1];
    std::string selector;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--camera" && i + 1 < argc) selector = argv[++i];
        else {
            std::printf("unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    if (cmd != "status" && cmd != "register" && cmd != "unregister") {
        std::printf("unknown command: %s\n", cmd.c_str());
        return 2;
    }
    if (cmd != "status" && !vcam::IsProcessElevated()) {
        std::printf("error: '%s' needs administrator rights (run from an elevated terminal or the installer).\n", cmd.c_str());
        return kExitNeedsAdmin;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    int rc = 1;
    {
        camera::MediaFoundationScope mf;
        if (FAILED(mf.hr())) {
            rc = Fail({mf.hr(), "MFStartup", "Media Foundation is unavailable."});
        } else if (cmd == "status") {
            rc = PrintStatus();
        } else if (cmd == "unregister") {
            const Error e = vcam::Unregister();
            rc = FAILED(e.hr) ? Fail(e) : (std::printf("IXC Camera system camera removed.\n"), 0);
        } else {
            camera::CameraInfo cam;
            if (PickPhysical(selector, cam)) {
                const Error e = vcam::Register(cam.symbolicLink, cam.name);
                if (FAILED(e.hr)) {
                    rc = Fail(e);
                } else {
                    std::printf("IXC Camera registered, using \"%s\".\n", cam.name.c_str());
                    rc = 0;
                }
            }
        }
    }
    CoUninitialize();
    return rc;
}
