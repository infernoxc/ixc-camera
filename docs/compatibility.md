# Compatibility matrix

A result is **PASS** only when the test actually ran here. Everything else is marked **NOT TESTED** with the reason.

Machine: Windows 11 Pro build 26200, Ryzen 5 5600G, RX 6600. Camera: Lenovo FHD Webcam (USB 2.0, inbox UVC driver). IXC Camera 0.3.0, installed with `scripts/install-ixc.ps1`. Date: 2026-09-26.

## Camera APIs (how apps read cameras)

| Path | Represents | Result | Evidence |
|---|---|---|---|
| Media Foundation Source Reader | Chrome, Edge, Discord (Chromium), Teams, most modern apps | **PASS** | `ixc_probe --capture --camera "IXC Camera"`: 1080p NV12, 0 drops, 37 ms latency |
| DirectShow capture graph | OBS "Video Capture Device", older Zoom/Skype-style apps | **PASS** | `ixc_probe --dshow-capture`: frames delivered. The DirectShow bridge offered 1280×960 YUY2 by default |
| DirectShow device list | same | **PASS** | "IXC Camera (Windows Virtual Camera)" listed |
| IXC source loaded in-process (no Windows service) | validation harness | **PASS** | `ixc_probe --source-test` |
| Picture settings applied through IXC Camera (Media Foundation client) | all apps | **PASS** | brightness +60: mean luma 113.7 → 148.0 as seen by a separate app (v0.4.0) |
| Live settings change while an app streams IXC Camera | all apps | **PASS** | in-process test; the service reloads on file change |

## Applications

| App | Result | Notes |
|---|---|---|
| OBS Studio 32.2.2 | **PASS (user-verified, v0.4.0)** | Detects "IXC Camera (Windows Virtual Camera)" as a Video Capture Device; live picture settings apply correctly |
| Discord 1.0.9259 (installed) | **NOT TESTED — REQUIRES USER** | Chromium/Media Foundation path passes. It needs a manual check in Settings → Voice & Video → Camera |
| Windows Camera app | NOT TESTED — not installed | |
| Brave 1.x (Chromium) | **PASS (automated, `tests/browser_camera.ps1`)** | getUserMedia: 1280×720 → 29.8 FPS, 1920×1080 → 30.0 FPS; camera listed as "IXC Camera (Windows Virtual Camera)" |
| Chrome / Edge | NOT TESTED — not installed | Same Chromium capture path as Brave (PASS); the test script uses them automatically when present |
| Zoom, Microsoft Teams | NOT TESTED — not installed | |

## Behaviour

| Scenario | Result | Notes |
|---|---|---|
| Physical webcam still listed and usable while IXC Camera is installed | **PASS** | |
| Alternating apps: webcam → IXC → webcam → IXC | **PASS** | 6 alternations, 0 reconnects |
| Physical webcam usable immediately after an IXC session | **PASS** | |
| Two apps at once: webcam direct + IXC Camera, or two IXC clients | **Same as the hardware baseline:** the second app can't start | Two apps opening the physical webcam directly fail the same way (`MF_E_HW_MFT_FAILED_START_STREAMING`). With default settings, Windows lets one app stream a camera at a time. The first app is never disturbed |
| 30 open/close cycles on IXC Camera | **PASS** | App process: +0.1 MB, +2 handles |
| No IXC work when unused | **PASS** | Camera service returns to 0.00% CPU and 7.6 MB after the app closes; physical camera stops |
| Uninstall removes the camera, COM class and files | **PASS** | Gone from Media Foundation and DirectShow lists; webcam works afterwards |
| Reinstall | **PASS** | Same device identity as before (apps keep their selection) |
| Upgrade while the camera service holds the old DLL | **PASS** | Old DLL renamed and deleted at reboot; no service restart needed |
| Reboot persistence (System lifetime) | **NOT TESTED — REQUIRES REBOOT** | Documented behaviour: persists across reboots |
| Camera unplug during an IXC session | **NOT TESTED — REQUIRES USER** | |
| Sleep / wake | **NOT TESTED** | |
| Camera privacy "off" | **NOT TESTED** (would change your settings) | |
| "Use this webcam for IXC Camera" button (UAC prompt) | **NOT TESTED — REQUIRES USER** | UAC runs on the secure desktop and can't be automated |
| Second physical camera | NOT TESTED — only one webcam connected | |
