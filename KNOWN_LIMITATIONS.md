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

## Not yet implemented (later phases)
- Face tracking and effects (Phase 7–8).
- Packaged installer `IXC-Camera-Setup-x64.exe` with an Apps & Features entry (Phase 12). The development installer is `scripts/install-ixc.ps1`.
- Code signing.

## Low-end hardware
- No measurements exist yet on Ultra Low / Low hardware. All numbers come from the developer's Ryzen system.
