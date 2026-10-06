# IXC Camera 0.13.0

Download **IXC-Camera-Setup-x64.exe** below and run it. Close apps that use the camera first. Your settings are kept.

## What's new
- **Better background blur:**
  - smooth and even, with no blocky patches;
  - Style menu: Soft Blur, Standard Blur, **DSLR Bokeh** (bright spots bloom, the blur grows with distance), Strong Bokeh, Custom;
  - **Blur strength 0–100%**.
- **Cleaner edges around you:**
  - less half-sharp background around hair and shoulders;
  - steadier edges that also keep up faster when you move.
- **Advanced blur settings** (optional): Focus falloff, Edge feather, Edge protection, Temporal stability, Hair refinement.

## Tested
- Every build runs the unit tests on Windows in CI (Debug and Release), including the new blur, edge and profile tests.
- **NOT TESTED — REQUIRES USER ENVIRONMENT:**
  - real webcam footage (hair, glasses, hands, chair edges, fast movement);
  - frame rate at 1080p on your PC.

  Please send a screenshot if something looks off.

## Not in this version
A dedicated portrait-matting network (MODNet) is planned. This release improves the existing segmentation and rendering instead.
