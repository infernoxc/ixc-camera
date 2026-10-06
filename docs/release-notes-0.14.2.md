# IXC Camera 0.14.2

Download **IXC-Camera-Setup-x64.exe** below and run it. Close apps that use the camera first. Your settings are kept.

## Fixed
- **Settings now reach OBS, Teams, Zoom, Discord and browsers.** Before, changing blur strength (or other settings) often changed only the preview in the IXC Camera app, while other apps kept the old look. The settings file could be briefly locked by the Windows camera service at the moment the app updated it, and that update was silently lost. Updates are now retried and can no longer be blocked that way.

## After installing
Windows keeps the previous IXC Camera version loaded in its camera service until that service stops (or the PC restarts). If another app still doesn't follow your changes right after updating, **restart the PC once**.

## Tested
- Unit tests on Windows in CI (Debug and Release), including a new test that publishes settings while the file is held open, and the release audit.
- **NOT TESTED — REQUIRES USER ENVIRONMENT:** live changes in OBS/Teams/Zoom/browsers on your PC.
