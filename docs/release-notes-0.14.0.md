# IXC Camera 0.14.0

Download **IXC-Camera-Setup-x64.exe** below and run it. Close apps that use the camera first. Your settings are kept.

## What's new
- **New layout:**
  - navigation on the left (Camera, Effects, Background, Profiles, Settings, Updates); the inspector on the right shows only that section;
  - toolbar with camera, format, Refresh and Start/Stop;
  - quick background bar under the preview (None, Blur, Image, Colour) and an Effects switch;
  - LIVE badge, IXC Camera status and the version always visible.
- **Changes reach other apps right away:** while you drag a slider, IXC Camera in Teams, Zoom, OBS or the browser follows within about 40 ms (it used to wait until you let go). The profile file is saved once you stop.
- **Sliders behave:** the mouse wheel over a slider changes it, and scrolling the panel no longer grabs sliders you pass over (hold Shift to always scroll). Click the track to jump; arrows, Page Up/Down, Home/End work.
- **Steadier blur edges:** the mask eases between segmentation updates instead of popping, is smoothed before feathering, and survives a dropped mask briefly instead of flashing the sharp background.
- **In-app updates:** Settings › Updates checks the official GitHub releases (once a day and on request). An update is downloaded only after you confirm, checked against the release's SHA-256 checksum, and then installed.

## Tested
- Every build runs the unit tests on Windows in CI (Debug and Release), including new tests for the update check (SHA-256, versions, release validation), live publishing, slider wheel handling and the blur mask.
- **NOT TESTED — REQUIRES USER ENVIRONMENT:**
  - the new layout on your screen and DPI;
  - live slider changes in Teams/Zoom/OBS/Discord/browser;
  - blur edges on real webcam footage;
  - the update flow end to end (it activates once a release newer than 0.14.0 exists).
