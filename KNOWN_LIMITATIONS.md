# Known limitations

## Platform
- **Windows 11 only (build 22000+).** The Media Foundation virtual camera API doesn't exist on Windows 10.
- Windows adds " (Windows Virtual Camera)" to the name, so apps show **"IXC Camera (Windows Virtual Camera)"**. IXC can't change this.
- Installing, uninstalling or changing which webcam IXC Camera uses needs **administrator approval**, because the camera is registered for all users. Everyday use doesn't.

## Sharing the webcam
- With Windows' default settings, **one app at a time** can stream a physical webcam. The same applies to IXC Camera, which reads that webcam. While one app uses IXC Camera, another app can't open the physical webcam directly (and vice versa) until the first one stops. This matches the hardware baseline measured with two ordinary apps; IXC doesn't make it worse. A future version may attach to a webcam another app controls (Windows shared mode) and process that stream.
- IXC Camera offers the webcam's **NV12** modes only (what Windows decodes from the camera). It never offers MJPG, so that every mode can be processed by IXC's effects. It never invents modes the camera doesn't have.

## Upgrades
- If the Windows camera service still has the previous IXC DLL loaded during an upgrade or uninstall, the old file is renamed `*.old-*` and deleted at the next reboot. Until the service unloads it (it stops by itself when idle, or on reboot), apps may still get the old version.

## Picture processing
- Processing runs on the CPU (SSE2). Only crop/zoom may move to the GPU, and only when measured faster on your PC. That costs ~40–60 MB of RAM while zoom is active and is never done on PCs with under 4 GB RAM. Turn it off with the app's "Use the graphics card for zoom" checkbox (or `"gpu": "off"`). Without a usable GPU, digital zoom costs ≈7 ms per 1080p frame on the reference Ryzen (more on weak CPUs), so prefer 720p when zooming.
- GPU acceleration has only been measured on an AMD RX 6600. Integrated Intel/AMD graphics (the low-end target) haven't been measured yet. The on-machine selection is designed to decide correctly there, but that's unverified.
- **Denoise isn't implemented yet.** "Low-light boost" lifts dark tones but doesn't reduce noise.
- Crop can be set in a profile file but has no editor in the app yet. Digital zoom (centre crop) is in the app.
- The app's preview and IXC Camera can't run at the same time (one app per webcam, see below). Adjust settings with the preview, close it, then use IXC Camera. Changes made in the app while another app uses IXC Camera still apply live.
- IXC Camera settings are machine-wide: any user on this PC who opens the app changes what IXC Camera shows.

## Frame rate in low light (Smooth motion)
- Many webcams lower their frame rate in dim light (automatic exposure lengthens each frame). The Lenovo FHD Webcam delivers an uneven 14–20 FPS on its "30 FPS" modes in normal room light. **Smooth motion** (on by default) fixes the exposure so the full rate returns, then brightens the picture in software. The trade-offs:
  - The first ~4 s of the first session run at the camera's own rate while IXC measures. Later sessions start smooth in ~1 s.
  - Compensation is capped at +1.5 EV. In very dark rooms the picture is darker than with automatic exposure, and a little noisier. Add light, or turn Smooth motion off to prefer brightness over frame rate.
  - While a session runs, the camera's exposure is fixed. Automatic exposure is restored when the session ends. If IXC is killed abruptly (crash, forced service stop), the camera may stay on a fixed exposure until it's unplugged or another app resets it.
  - Needs the camera's UVC exposure control. Cameras without one are left unchanged.
- OBS: add **one** source per camera. Two sources on the same camera (or IXC Camera at two resolutions, or the webcam and IXC Camera together) can't both stream: the second shows a frozen picture (`0x800705AA` in the OBS log).

## Face tracking
- Off by default. In this version nothing visible uses it yet: face effects come in Phase 8. The app can draw the tracked faces over its preview. They're never drawn into what other apps receive.
- Five landmarks only: eyes, nose tip and mouth corners. Brow regions and head roll/yaw/pitch are estimates derived from those points. They're enough to anchor 2D effects, but they aren't a 3D head pose.
- Detection runs a few times per second, and positions in between are predicted. Very fast head movements can briefly leave effects behind (≤ one detection interval).
- Faces must be roughly frontal and at least ~10% of the frame width (smaller with the 320×180 input on fast CPUs). At the 160×90 fallback used on weak CPUs, landmarks are unreliable (~30% rejected): landmark-anchored effects then hold their last position.
- On a CPU too slow to detect twice a second within the budget, tracking switches itself off for the session ("off (CPU too slow)" in the app). The video is unaffected.
- While tracking, the camera service holds ~6–7 MB more memory (the detector's working buffers at 320×180; ~3 MB at 240×135), all released when tracking stops. Real low-end hardware hasn't been measured (docs/face-tracking-design.md).

## Not yet implemented (later phases)
- Effects (Phase 8).
- Packaged installer `IXC-Camera-Setup-x64.exe` with an Apps & Features entry (Phase 12). The development installer is `scripts/install-ixc.ps1`.
- Code signing.

## Low-end hardware
- No measurements exist yet on Ultra Low / Low hardware. All numbers come from the developer's Ryzen system.
