# IXC Camera 0.15.0

Download **IXC-Camera-Setup-x64.exe** below and run it. Close apps that use the camera first. Your settings are kept. After installing, **restart the PC once** so Windows loads the new camera service.

## What's new
- **Frame editor, like Elgato's:**
  - click **Frame** above the preview: you see the whole camera picture and the frame other apps receive;
  - drag to move it, drag a corner or scroll to resize it, double-click for the whole picture;
  - Teams, Zoom, OBS, Discord and browsers follow while you drag.
- **Framing section:** field-of-view presets, a rule-of-thirds grid, and auto-framing controls (how tight, how fast, max zoom).
- **Faster, more precise face tracking** for auto-framing and face effects (uses a little more CPU).
- **Sharper background edges** for blur, pictures and solid colours: the cut follows your real outline.

## Fixed
- **Auto-framing, face tracking and background effects now work on webcams that deliver YUY2.** Before, the camera service only processed NV12 cameras, so on many webcams (a friend's, for example) other apps got the plain picture.
- Cameras with padded rows, cameras that only offer other formats, and cameras opened at a new size are handled.

## Tested
- Unit tests on Windows in CI (Debug and Release), including new tests for the framing settings, precise tracking, edge refinement and YUY2 conversion, and the release audit.
- **NOT TESTED — REQUIRES USER ENVIRONMENT:**
  - the frame editor on your screen and DPI;
  - tracking and framing in Teams/Zoom/OBS/Discord, on YUY2 webcams and mobile-phone camera apps;
  - edge quality on real webcam footage.
- **Not included:** camera hardware controls such as focus, exposure and white balance sliders (use the picture sliders, or your camera's own app), and anything Elgato's hardware does beyond the camera picture (Stream Deck, light control).

Please send a screenshot if something looks off.
