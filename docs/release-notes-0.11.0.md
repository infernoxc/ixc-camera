# IXC Camera 0.11.0

Snap Camera-style features: real background blur and replacement, face stickers, auto-framing and lens hotkeys. Everything runs on your PC; nothing is uploaded.

## Install
1. Download `IXC-Camera-Setup-x64.exe` and check it against `SHA256SUMS.txt`.
2. Run it and approve the administrator prompt. Upgrading from 0.10.0 keeps your profiles and settings.
3. Close every camera app, then reopen them, so Windows loads the new IXC Camera.
4. In any app (OBS, Zoom, Teams, Discord, a browser), choose **IXC Camera (Windows Virtual Camera)** as the camera.

## What's new
- **Background Blur** and **Studio Backdrop.** An on-device person-segmentation model (MediaPipe Selfie Segmentation, Apache 2.0) finds you in the picture:
  - Background Blur softens everything behind you without a halo around your outline;
  - Studio Backdrop replaces the background with a soft, neutral studio gradient.

  The model runs only while one of these effects is on (≈2.7 MB). If your PC is too slow for it, the effects switch themselves off and the status line says so.
- **Face stickers:** Shades, Heart Eyes, Crown and Puppy. They follow your eyes and nose, scale with your face and tilt with your head. You can combine them.
- **Auto-framing** (Camera features, off by default): zooms and pans smoothly to keep you framed, up to 2.5× digital zoom.
- **Lens hotkeys:** Ctrl+Alt+F7 (next) and Ctrl+Alt+F6 (previous) step through the effects one at a time, like Snap lenses. Effects you switched on yourself are left alone.

## Fixed
- Portrait now blurs the background's colour too, not only its brightness.
- Blush Tone's skin smoothing fades in at the intended rate.

## Verified
- Windows CI (MSVC, Debug and Release): build and unit tests, including a test that checks the segmentation network against Google's reference output.
- Portable unit tests on Linux with memory, undefined-behaviour and thread checkers.

## Still to verify on your side
- The new effects live with a webcam, in the app and in OBS, Zoom, Teams, Discord and browsers.
- Effect and segmentation cost on your PC (`ixc_probe --bench-effects`), and on low-end PCs.
- How auto-framing feels with a moving person.
