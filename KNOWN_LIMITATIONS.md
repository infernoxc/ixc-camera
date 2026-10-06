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

- **In-app updates** (0.14+) start the regular installer, so the usual UAC prompt appears. Releases before 0.14 have no updater: update them by downloading the installer once. The installer isn't code-signed yet, so SmartScreen may warn.

## Picture processing
- **Processing: Auto / GPU / CPU.**
  - What moves to the GPU: the picture pipeline (colour, sharpen, crop/zoom) and the segmentation network.
  - What always runs on the CPU (SSE2): temporal denoise, mask refinement, background compositing and the face effects. GPU mode lowers the CPU load but doesn't remove it.
  - The GPU path uses ~40–60 MB for the picture and ~3 MB of video memory for the network.
  - Auto never considers the GPU for the picture on PCs with under 4 GB RAM.
- **GPU paths:**
  - CI tests them on WARP, Direct3D's software device, against the CPU.
  - The picture pipeline was measured on an AMD RX 6600. The segmentation network on real GPUs (integrated Intel/AMD, NVIDIA) is **NOT TESTED — REQUIRES USER ENVIRONMENT**. Run `ixc_probe --bench-seg` to see the numbers on your PC.
- **Background:**
  - Segmentation runs at 256×144 (refined to 512×288). Very fine hair strands and fast hand motion can show soft or slightly late edges.
  - Objects you hold, and headphones, are kept or dropped as the network judges.
  - Custom WEBP pictures need the Windows WebP Image Extension.
- **Portrait matting:** the person mask comes from a 256×144 segmentation network refined to 512×288 (not a dedicated matting network such as MODNet). Very fine hair strands and objects you hold are judged by that network; Edge feather and Hair refinement tune the result.
- **Denoise is temporal.** Fast-moving areas keep their noise rather than smear.
- **Anti-flicker** depends on the camera's power-line control. Cameras without one are left unchanged.
- **Auto anti-flicker** uses your Windows region. Japan (both frequencies) leaves the camera's own setting.
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

## Auto-framing
- Auto-framing is a digital zoom: at its maximum (2.5×) a 720p camera shows a visibly softer picture. Use 1080p when possible.
- It follows the largest face only, and moves when face tracking does (a few detections per second, predicted in between). Very fast movements are followed with a short delay.
- While framing, the picture is processed on the CPU even if the profile is otherwise neutral (≈7 ms per 1080p frame on the reference Ryzen).

## Face tracking
- Off by default. In this version nothing visible uses it yet: face effects come in Phase 8. The app can draw the tracked faces over its preview. They're never drawn into what other apps receive.
- Five landmarks only: eyes, nose tip and mouth corners. Brow regions and head roll/yaw/pitch are estimates derived from those points. They're enough to anchor 2D effects, but they aren't a 3D head pose.
- Detection runs a few times per second, and positions in between are predicted. Very fast head movements can briefly leave effects behind (≤ one detection interval).
- Faces must be roughly frontal and at least ~10% of the frame width (smaller with the 320×180 input on fast CPUs). At the 160×90 fallback used on weak CPUs, landmarks are unreliable (~30% rejected): landmark-anchored effects then hold their last position.
- On a CPU too slow to detect twice a second within the budget, tracking switches itself off for the session ("off (CPU too slow)" in the app). The video is unaffected.
- While tracking, the camera service holds ~6–7 MB more memory (the detector's working buffers at 320×180; ~3 MB at 240×135), all released when tracking stops. Real low-end hardware hasn't been measured (docs/face-tracking-design.md).

## Not yet implemented (later phases)
- Effect packages from disk (built-in effects only so far); per-effect strength (one shared strength slider).
- Hotkeys work only while the IXC app is running (IXC Camera itself keeps working without it). They are fixed (Ctrl+Alt+F6–F11) and can be turned off, not remapped.
- The lens hotkeys (Ctrl+Alt+F7 next, F6 previous) cycle through every built-in effect, one at a time. They replace only the effect they added themselves, at 70% strength, and skip effects you switched on by hand.
- Profiles are listed by file name (lowercase, spaces become dashes).
- The installer isn't code-signed yet (SmartScreen warns about an unknown publisher). `scripts/install-ixc.ps1` remains the development installer.
- Code signing.

## Low-end hardware
- No measurements exist yet on Ultra Low / Low hardware. All numbers come from the developer's Ryzen system.

## Effects
- **Background Blur and Studio Backdrop** find the person with a small on-device network (MediaPipe Selfie Segmentation, 256×144). The mask is coarser than the video: edges are soft (≈7 px at 1080p), fine hair and fingers can be missed, and objects held close may count as background. People further than ~4 m away, or several people at different distances, may be segmented partly. The mask is a few frames behind fast movement (it's computed 10–30 times per second on a worker thread, not for every frame). On a CPU too slow for 4 masks per second within its budget, the background effects switch themselves off ("background: off (CPU too slow)" in the app). Measured cost: see docs/performance.md (not yet measured on Windows hardware).
- Portrait is an approximation without segmentation: the subject is an ellipse around the tracked head and shoulders, so hands, hair and objects outside it are blurred too. Prefer Background Blur. It is the most expensive effect (~6 ms per 1080p frame on the reference Ryzen; expect 2–4× that on low-end CPUs). Prefer 720p on weak PCs.
- Effects add their processing time to the latency (all effects at 1080p: +8–9 ms).
- Blush, Beauty and the stickers follow the largest face only.
- Stickers are flat (2D): they tilt with the head but don't turn with it in 3D. They're anchored to five landmarks, so when the head turns far to the side they slide a little. Hands in front of the face are drawn over.
