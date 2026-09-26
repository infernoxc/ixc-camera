# IXC Camera 0.9.0 (release candidate)

IXC Camera turns your webcam into an enhanced camera called **"IXC Camera"** that any Windows 11 app can select: Zoom, Teams, Discord, OBS, browsers. It needs no OBS plugin, no driver, no account and no internet.

## Install
1. Download `IXC-Camera-Setup-x64.exe` and check it against `SHA256SUMS.txt`.
2. Run it and approve the administrator prompt (the camera is registered for all users).
3. In your app's camera settings, choose **IXC Camera (Windows Virtual Camera)**. Open **IXC Camera** from the Start menu to adjust the picture.

Uninstall from **Settings › Apps › Installed apps**. Silent install/uninstall and exit codes are in docs/installer.md.

## What's in it
- **Picture:** brightness, contrast, saturation, warmth, tint, exposure, highlights, shadows, low-light boost, gamma, sharpness, mirror and digital zoom, applied live.
- **Smooth motion:** keeps the full frame rate in low light, where many webcams drop to 14–20 FPS.
- **Effects:** Blush Tone, Basic Beauty, Portrait (soft background), Warm Glow, Cool Breeze, Mono, Vivid and Soft Light, with one strength slider.
- **Face tracking** (libfacedetection): runs in the background within a CPU budget. It slows down, uses a smaller image, or switches off on weak PCs rather than slowing the video.
- **Profiles:** save, switch, import and export. **Hotkeys:** Ctrl+Alt+F8 effects on/off, F9/F10 next/previous profile, F11 mirror (while the app runs).
- **Low resource use:** nothing runs while no app uses IXC Camera. Measured on a Ryzen 5 5600G at 1080p30: 30.0 FPS, 0 dropped, 37 ms latency, camera service ~15% of one core and ~65 MB (all effects on: ~46% and 69 MB). A 30-minute soak test showed no memory growth.

## Requirements
Windows 11 x64 (build 22000+) and a webcam.

## Known limitations
See KNOWN_LIMITATIONS.md. The main ones:
- One app at a time can use a physical webcam, and the same applies to IXC Camera.
- The installer isn't code-signed yet, so SmartScreen may warn.
- Portrait is an approximation (subject ellipse, no segmentation).
- Not yet tested on real low-end hardware (Pentium/Celeron, 2 GB RAM), or in Zoom, Teams or Discord. The Chromium browser path, OBS (DirectShow) and Media Foundation apps are verified.

## Verification
- 123 unit tests, 12 hardware/integration test suites and a 29-check UI test pass.
- Release audit: ASLR, DEP, CFG and CET enabled; only system DLLs imported; no network APIs (docs/security-audit.md).
- The installer's install, upgrade, uninstall and reinstall cycle was tested silently (docs/installer.md).

Licence: MIT. Includes libfacedetection (BSD 3-Clause), see THIRD_PARTY_LICENSES.md.
