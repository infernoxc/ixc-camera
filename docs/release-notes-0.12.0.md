# IXC Camera 0.12.0

Download **IXC-Camera-Setup-x64.exe** below and run it. Nothing else is needed: Windows 11, no extra runtimes. Close any app that uses the camera before you install.

## What's new
- **Background:** Blur (Low / Medium / High, depth-like), 9 built-in scenes, solid colour, or your own picture (JPG / PNG / WEBP / BMP) with fit, zoom and position. Sharper edges around hair, ears, shoulders and hands, and less flicker.
- **Low light:** new **Noise reduction** with no ghosting on movement. Smooth motion turns it on by itself when it brightens a dark room.
- **No more dark bands from room lights:** **Anti-flicker** (Auto / 50 Hz / 60 Hz / Off). Smooth motion now gives the exposure back to the camera if its fixed exposure would band.
- **Processing: Auto / GPU / CPU** really switches where the work runs. The person-segmentation network now runs on any Direct3D 11 GPU (NVIDIA, AMD, Intel), and falls back to the CPU automatically if the GPU fails.
- **Diagnostics** (Camera features): FPS, the app's CPU / RAM / GPU / VRAM use, and what runs where.
- **Fixes:**
  - Buttons and switches react to every click.
  - The app shows its own CPU and RAM use.
  - The camera opens at its best resolution and frame rate by default.
- **Removed:** the novelty stickers (Shades, Heart Eyes, Crown, Puppy). Old background effects in your profiles move to the new Background setting automatically.

## Tested
- Every build runs the unit tests in CI on Windows (Debug and Release). This includes the GPU segmentation network, checked against the CPU on Direct3D's software device.
- **NOT TESTED — REQUIRES USER ENVIRONMENT:**
  - real webcams
  - low light and flicker under real lamps
  - GPU speed and use on real graphics cards
  - long soak runs

  Please report what you see. `ixc_probe --bench-seg` prints the CPU vs GPU numbers for your PC.

See CHANGELOG.md for details and KNOWN_LIMITATIONS.md for what isn't covered yet.
