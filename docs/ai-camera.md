# Backgrounds, segmentation, low light and processing modes (0.12)

## Frame path (IXC Camera and the app preview share it)
```
camera NV12 ─┬─> face engine (thread, sampled)          face/
             ├─> segmentation engine (thread, sampled)   segmentation/
             └─> picture pipeline (CPU or GPU) ──> temporal denoise ──> background ──> face effects ──> colour looks ──> app
                 processing/ (+gpu/)               processing/temporal_denoise  effects/background_renderer  effects/
```
- No per-frame allocations: every buffer is sized on the first frame and reused. Each stage frees its memory when it's switched off (background modes, denoise, GPU runners).
- Threads never block the frame path. Face and segmentation results are snapshots, copied only when new.

## Segmentation
- **Network:** MediaPipe selfie segmenter (landscape, 256×144, Apache-2.0), compiled in as data (`third_party/mediapipe_selfie_segmenter`). It is run either by IXC's SSE2 kernels (`selfie_net.cpp`) or by Direct3D 11 compute kernels (`gpu/selfie_net.hlsl`), with the same op table, weights and summation order. The GPU result is checked against the CPU before use, and CI runs that check on WARP.
- **Input:** dark frames are brightened (up to 2.5×) so the network sees a normally exposed picture.
- **Refinement** (`mask_refine.cpp`):
  - bilinear upsampling to 512×288;
  - a guided filter (r = 4, ε = 0.004) on the luma, so edges snap to hair, ears and fingers;
  - a soft threshold;
  - motion-adaptive temporal smoothing: strong where nothing moved, none where it did.
- **Face guard:** a soft head-and-neck shape from the face tracker is merged into the mask. The face, ears and nose are never cut.
- **Cadence:** the worker runs below normal priority within a CPU budget (25% of a core; on the GPU only the refinement counts). It goes up to 30 masks/s and parks itself on CPUs too slow for 4/s.

## Background renderer (`effects/background_renderer.cpp`)
- **Blur:** the frame is reduced to 1/8 scale, weighted by "background-ness" so the person's colours don't bleed in, and blurred at two radii. These are mixed by a distance field from the person, so the blur grows with distance, like a lens focused on you.
- **Replace / Custom:** a picture plate at frame size (Fill or Fit, position, zoom), built once per picture and setting. Built-in scenes are drawn in code. Custom pictures are prepared by the app (`app/background_import.cpp`: WIC decode, at most 1920×1080, `.ixbg`) in `%ProgramData%\IXC Camera\backgrounds`. The service reads only that strict format, never image files.
- **Colour:** constant plate.
- **Compositing:** per pixel through the crop/zoom/mirror mapping; the effect fades in over 4 frames.

## Low light
- **Smooth motion** (`camera/exposure_governor`, `smooth_motion`) fixes the exposure so the camera keeps its frame rate, and brightens in software (at most +1.5 EV).
- **Temporal denoise** (`processing/temporal_denoise`):
  - a recursive average per pixel, weighted by a 1-2-1 smoothed frame difference;
  - the threshold follows the measured noise (25th percentile of the differences);
  - chroma follows luma motion;
  - 4 fractional bits of history.
  - Smooth motion's gain raises the strength automatically (`WithSmoothMotionGain`).
- **Anti-flicker** (`camera/power_line`): sets the UVC power-line frequency. Auto is resolved by the app from the user's Windows region; the service never guesses. The original value is restored at session end.
- **Flicker detector** (`camera/flicker_detector`):
  - It looks for mains-light bands under a fixed exposure: moving bands in the frame-to-frame row profile, and standing bands as the fixed/auto profile ratio.
  - The test is a DFT over 1.5–8 cycles per frame that must hold in every quarter of the frame, so a moving person isn't mistaken for flicker.
  - On bands, Smooth motion hands the exposure back to auto.

## Processing mode (Auto / GPU / CPU)
| | Picture pipeline (`AdaptiveNv12Processor`) | Segmentation network (`SegmentationEngine`) |
|---|---|---|
| CPU | never touches Direct3D | CPU runner only |
| GPU | GPU at once for any work, kept; failure → CPU until the mode changes | GPU runner; unavailable or failing → CPU, reason in diagnostics |
| Auto | GPU only for zoom ≥ 720p when measured ≥ 30% faster, RAM ≥ 4 GB | measures both, keeps the GPU when ≤ 1.1× the CPU time |

- Switching is live (between frames / masks), and the unused runner and device are released.
- Direct3D is delay-loaded, so CPU mode never loads it.
- Denoise, refinement, compositing and the face effects are CPU work in every mode.

## Diagnostics
The app's diagnostics view shows:
- preview FPS;
- the app's CPU % and private RAM, plus GPU % and dedicated VRAM from the Windows GPU performance counters (any vendor);
- picture backend and timings;
- segmentation state, device, network time, masks/s, memory and GPU notes.

IXC Camera inside the camera service is traced with `scripts/trace-vcam.ps1` (events include the segmentation backend, device and GPU note).
