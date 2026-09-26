# IXC Camera 0.10.0

A redesigned control panel, a proper app icon and desktop shortcut, per-effect controls, and a cleaner Blush Tone.

## Install
1. Download `IXC-Camera-Setup-x64.exe` and check it against `SHA256SUMS.txt`.
2. Run it and approve the administrator prompt. Upgrading from 0.9.0 keeps your profiles and settings.
3. Open **IXC Camera** from the desktop or the Start Menu. The control panel opens straight to the live preview.
4. In any app (OBS, Zoom, Teams, Discord, a browser), choose **IXC Camera (Windows Virtual Camera)** as the camera.

## What's new
- **New dark control panel:**
  - a large preview with a camera/format toolbar and a one-line status: FPS, latency, drops, CPU/GPU and face tracking;
  - settings cards for Profile, Picture, Effects, Camera features and IXC Camera;
  - IXC Camera's system camera status in the header.

  It resizes cleanly down to 860×560.
- **A switch for every optional feature**, showing what's actually running:
  - each effect has its own switch and strength;
  - an Effects master switch (also Ctrl+Alt+F8), "All off", and Reset for the picture.

  Switched-off effects and face tracking use no CPU or memory.
- **App icon** for the app, window, taskbar, shortcuts, installer and Apps & features.
- **Installer:** a desktop shortcut and a Start Menu entry. Upgrades don't duplicate them, and uninstall removes both.
- **Blush Tone:** the lip colour is gone. The rest of the look (rosy grade, soft glowing skin, warm under-eyes and nose) is unchanged.

## Verified on the developer PC (Ryzen 5 5600G, Lenovo FHD Webcam)
- 123 unit tests, and all 12 unit + hardware/integration suites in both Debug and Release: virtual camera via Media Foundation and DirectShow, open/close cycles, live face tracking, headless Chromium getUserMedia.
- A UI test that drives the app:
  - every slider and switch, Reset, All off and the master switch;
  - layout overlap checks at two window sizes;
  - settings surviving a restart;
  - minimize/restore.
- An installer test: install, upgrade, shortcuts, launch from the desktop shortcut, uninstall, reinstall.
- OBS format (DirectShow 1280×960 NV12): 29.7 FPS with Smooth motion on.
- Release audit: ASLR, DEP, CFG and CET on; only system DLLs; no network APIs.

## Still to verify on your side
- OBS, Discord, Zoom and Teams themselves (their camera paths are covered by the DirectShow, Media Foundation and Chromium tests).
- Low-end PCs (Pentium/Celeron, 2 GB RAM).
- Unplugging the camera mid-use, and sleep/wake.
- The installer isn't code-signed: SmartScreen may show "unknown publisher".

Licence: MIT. Includes libfacedetection (BSD 3-Clause), see THIRD_PARTY_LICENSES.md.
