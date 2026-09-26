# Performance measurements

Every number here was measured, and each table names the machine it came from. Numbers from the developer's reference system (Target C) are **not** evidence of low-end performance. Ultra Low / Low tier measurements (Targets A and B) are still **pending hardware**.

## How to reproduce

```powershell
./scripts/build.ps1 -Preset release
build/release/src/tools/ixc_probe.exe --list
build/release/src/tools/ixc_probe.exe --capture 15 --subtype NV12 --output nv12
build/release/src/tools/ixc_probe.exe --cycles 40 --output nv12
tests/ui_smoke.ps1 -Exe build/release/src/app/IXCCamera.exe -PreviewSeconds 10
```

`ixc_probe` reports machine-wide CPU next to a 3-second camera-closed baseline. That matters because Windows Frame Server can decode MJPG **in its own service process**, which a per-process counter never sees.

## Phase 3: capture only, no effects (2026-09-26)

Machine: Ryzen 5 5600G (6C/12T), 16 GB, RX 6600, Windows 11 26200. Camera: Lenovo FHD Webcam (USB 2.0, inbox UVC driver). Release build.

### What the camera really offers
- 106 distinct native modes. At 1080p30 and 720p30 it sends **MJPG**. Uncompressed YUY2 reaches only **5 FPS at 1080p** and 10 FPS at 720p (USB 2.0 bandwidth).
- The NV12 modes listed at 1080p30 come from Windows Frame Server decoding the camera's MJPG. They are not uncompressed sensor output.
- In the test room the camera delivered **~19.9 FPS in every 30 FPS mode**, even 640×480, because auto exposure lengthens the frame time in low light. With more light it delivered 30 FPS. IXC now detects this and tells the user (under-speed diagnostic).

### Capture paths at 1920×1080 "30 FPS" (camera delivering ~20 FPS due to light)

| Path | Latency (capture → app) | IXC process CPU | Machine CPU (baseline) | IXC private memory |
|---|---|---|---|---|
| MJPG, no decode (`--subtype MJPG --output native`) | 32.6 ms | 1.0% of one core | 5.1% (5.0%) | 3.5 MB |
| MJPG decoded to NV12 inside IXC | 37.1 ms | 2.2% of one core | 5.3% (4.0%) | 3.6 MB |
| NV12 from Frame Server | 37.5 ms | 2.3% of one core | 6.0% (3.7%) | 3.6 MB |
| RGB32 converted by Windows (`--output rgb32`) | — | — | — | 29.3 MB |

On this machine the CPU differences sit within background noise. Decoding adds about 4–5 ms of latency. The RGB32 conversion path costs ~26 MB of extra private memory, so IXC doesn't use it.

### Stability
- 40 open→stream→close cycles (NV12): 40/40 succeeded; private bytes grew −0.04 MB; handles +2. Open → first frame: 227 ms mean, 245 ms max.
- 25 cycles (RGB32): 25/25; +0.17 MB; handles +14 (within gate).
- 15 s streams: 0 dropped frames, 0 stalls, 0 timestamp discontinuities.

### UI preview (`IXCCamera.exe`, NV12 → display-size BGRA, GDI blit)

| Measurement | Value |
|---|---|
| Private memory, idle (no camera open) | 4.9 MB |
| Private memory, previewing 1080p | 6.9–7.0 MB (was 38.7 MB with the RGB32 path) |
| Threads previewing | 13–14 (was 25) |
| UI process CPU previewing (~20 FPS) | 3.7% of one core |
| Minimize | stops capture and releases the camera |

## Phase 4: IXC Camera system camera, pass-through (2026-09-26)

Same machine and camera. IXC's source runs inside the Windows camera service (Frame Server), so its cost is measured on that service process. Each streaming row is 8 s at 1080p30 NV12, with the camera delivering ~20–25 FPS in room light.

| Frame Server process | CPU (one core) | Private memory | Threads |
|---|---|---|---|
| Idle, no app using any camera | 0.00% | 7.4 MB | 17 |
| App reads the **Lenovo directly** as NV12 (Windows decodes MJPG) | 9.72% | 51.5 MB | 26 |
| App reads **IXC Camera** (NV12) | 11.25% | 51.6 MB | 24 |
| App reads the Lenovo as MJPG (no decode) | 1.34% | 48.1 MB | 22 |
| Right after the app closed | 0.19% | 7.6 MB | 18 |
| ~1 minute after the app closed | 0.00% | 7.6 MB | 18 |

**IXC's pass-through costs about 1.5% of one core and no extra memory** over reading the webcam directly in the same format. The rest is Windows' MJPG → NV12 decode, which any app asking for NV12 pays for.

| Measurement | Value |
|---|---|
| Latency, capture → app, through IXC Camera | 37.4 ms mean (37.5 ms direct: no measurable added latency) |
| Open → first frame, IXC Camera | 202 ms mean, 209 ms max (30 cycles) |
| App process growth over 30 IXC cycles | +0.10 MB, +2 handles |
| Frame Server growth over 30 cycles | +99 handles / +1.1 MB via IXC vs **+97 handles / +0.7 MB direct**. This is Windows bookkeeping, not IXC |
| IXCCameraSource.dll size | 190 KB (static CRT, no dependencies beyond Windows) |

## Phase 5: image pipeline (CPU, 2026-09-26)

### Pipeline cost per frame (`ixc_probe --bench-pipeline`, single thread, synthetic noisy frames)

| Settings | 720p | 1080p |
|---|---|---|
| Neutral (pass-through, no copy) | 0 ms | 0 ms |
| Colour/tone only (lookup tables) | 0.43 ms | 0.98 ms |
| Colour + mirror | 0.62 ms | 1.38 ms |
| Colour + sharpen (the **default** profile sharpens subtly) | 0.87 ms | 1.95 ms |
| Colour + sharpen + digital zoom 1.5× | 3.02 ms | 6.80 ms |

At 30 FPS the frame budget is 33.3 ms. Before optimization, sharpening cost **20.4 ms** at 1080p (scalar); the SSE2 version is ~10× faster and bit-identical to the scalar reference (unit-tested). The zoom path's vertical-then-horizontal restructuring took the total from 9.4 to 6.8 ms. The noisy synthetic frames are a worst case for sharpening: real camera frames have large flat areas that the noise threshold skips.

### In the real source

| Measurement | Value |
|---|---|
| Brightness +60 through IXC Camera, seen by a separate app | mean luma 113.7 → 148.0 |
| Live settings change mid-stream | applied within the next frames, no restart |
| Frame pool | 0 exhaustions, 0 errors over all runs; at most 6 frames in flight |
| Idle after the app stops | processing session, settings watch and frame pool released immediately (`SetStreamState(STOPPED)`) |
| IXCCameraSource.dll | 468 KB; imports only MF, MFPlat, MFSensorGroup, ole32, advapi32, kernel32 |

### App preview (default profile: subtle sharpening, 1080p)

| Measurement | Value |
|---|---|
| Private memory previewing | 8.8 MB (full-size processed frame buffer: +1.9 MB) |
| UI process CPU | 5.9% of one core (~20 FPS) |

## Phase 6: CPU vs GPU (Direct3D 11) — measured, then decided (2026-09-26)

Same machine (RX 6600; the Ryzen's integrated GPU is disabled, so it's the only adapter). The GPU path is byte-identical to the CPU path: 8 setting combinations × several sizes on the RX 6600 and on WARP, plus a live self-check on real camera frames.

**Where frames live.** In the Frame Server pipeline, IXC receives frames in **system memory** (`gpuInput=0`). So any GPU processing pays an upload and a readback per frame.

### Synthetic benchmark (`ixc_probe --bench-gpu`, per frame)

| Settings | Size | CPU path | GPU wall | GPU path CPU time | GPU execution |
|---|---|---|---|---|---|
| Colour only | 1080p | 1.02 ms | 1.59 ms | ~0.6 ms | 0.05 ms |
| Default (subtle sharpen) | 1080p | 1.01 ms | 1.63 ms | ~0.3 ms | 0.09 ms |
| Colour + sharpen 40 | 1080p | 1.91 ms | 1.62 ms | ~0.4 ms | 0.09 ms |
| Zoom 1.5 + sharpen 40 | 720p | 2.97 ms | 0.70 ms | ~0.2 ms | 0.05 ms |
| **Zoom 1.5 + sharpen 40** | **1080p** | **6.80 ms** | **1.65 ms** | **~0.3 ms** | 0.11 ms |

The GPU's own work is tiny (≤0.11 ms). Its wall time is dominated by the readback (~1.5 ms at 1080p).

### Memory (`ixc_probe --bench-gpu-memory`, 1080p)

| Step | Private bytes |
|---|---|
| Direct3D 11 device + shaders | +25 MB |
| First 1080p frame (textures) | +17 MB |
| 600 frames later | no growth |

The first implementation used `UpdateSubresource`, and the driver's hidden upload buffers grew to **+200 MB** and weren't returned on release. It now uses fixed upload textures (bounded).

### In the real camera service (1080p, ~25 FPS)

| Settings | Backend chosen | Frame Server CPU | Frame Server memory |
|---|---|---|---|
| Colour + sharpen 40 | CPU (not eligible) | 12.6% of one core | 55 MB |
| Zoom 1.5 + sharpen 40, `gpu: auto` | **GPU** (measured 7.0 → 2.2 ms/frame) | **13.3%** | 114 MB |
| Zoom 1.5 + sharpen 40, `gpu: off` | CPU | 28.3% | 57 MB |
| After the sessions | GPU released | 0% | 8.3 MB |

### Decision
- **Colour, tone, sharpen and mirror stay on the CPU.** The GPU would save at most ~1.5 ms per frame while adding ~41–57 MB and ~0.6 ms of latency.
- **Crop/zoom at ≥ 720p may use the GPU, but only after measuring it on the user's PC.** The CPU is measured first; the GPU is tried only if that's ≥ 3 ms/frame, and kept only if it's < 70% of the CPU time. There's a one-time byte-exact self-check, and it's released as soon as it's not in use.
- **Never on PCs with < 4 GB RAM or < 1 GB free**, where memory matters more. Direct3D is delay-loaded: it isn't even loaded unless a GPU attempt happens.
- The CPU path is always present and is the fallback for any GPU failure.

## Phase 7 prerequisite: choppy video, root cause and fix (2026-09-26)

Reported: IXC Camera looked extremely laggy/choppy in OBS. Measured before changing anything, with the Lenovo FHD Webcam direct and IXC Camera at the same formats (MF and DirectShow, 720p/1080p/1280×960 NV12) and the user's OBS logs.

### What it was not
- **Not IXC's pipeline.** Lenovo direct and IXC Camera measured the same: ~19.9 FPS, 5.6 ms jitter, 64 ms max gap. Inside the source, at most 1 sample is outstanding (no queue), and processing takes 0.6–2 ms per frame.
- **Not buffering or negotiation.** The format is negotiated exactly (1280×960 NV12 @ 30 for OBS), the latency is 36–37 ms, and 0 frames are dropped.
- **OBS scene setup made it worse but didn't cause it.** OBS canvas at 60 FPS. The scenes had two DirectShow sources on the same camera, sometimes IXC at 1280×720 and at 1280×960 at once, or the Lenovo and IXC together. The second source can't start (`0x800705AA`) and shows a frozen frame. The first one is unaffected.

### Root cause
The camera's **automatic exposure**. In normal room light it chose ~1/16 s exposures, so a "30 FPS" mode delivered an **uneven ~20 FPS** (a mix of 48 ms and 64 ms frames; in a darker room 14 FPS at 70–80 ms). On a 60 FPS canvas that's visibly choppy. The same happens with the Lenovo direct. Turning off "auto-exposure priority" didn't help. A fixed exposure did:

| Lenovo, 1080p "30 FPS" | FPS | Jitter | Luma |
|---|---|---|---|
| Auto exposure (room light) | 19.9 | 5.6 ms (max 64 ms) | 69 |
| Auto exposure (darker room) | 13.8–14.1 | 7.7 ms (max 80 ms) | 35–50 |
| Fixed 1/32 s (UVC −5) | **30.0** | 4.3 ms (max 48 ms) | 18–44 (darker) |
| Fixed 1/64 s (−6) | 30.0 | | darker still |
| Fixed 1/16 s (−4) | 15.6 | | |

### Fix: Smooth motion (on by default, `"smoothMotion"`, app checkbox)
`camera/exposure_governor` (pure logic, 11 unit tests) plus `camera/smooth_motion` (UVC `IKsControl`). It's used by the IXC Camera source and the app's capture:
1. Skip 8 frames (auto exposure converging), then measure 20 frames. If the rate is below 85% of nominal, set the longest fixed exposure that fits one frame (`floor(log2(1/fps))`: −5 at 30 FPS).
2. Skip 8 frames, then verify 20. If the camera didn't speed up, give it back its auto exposure and stop trying.
3. Compensate the lost brightness in software: `log2(lumaBefore/lumaAfter)`, capped at +1.5 EV, added to the exposure lookup table (no extra per-frame cost). It's re-measured 3× over the next ~3 s because the camera's gain keeps settling.
4. Once locked: a timing-only watchdog (30-frame windows) re-applies the fixed exposure if something else gives the camera its auto exposure back.
5. At session end, the camera's original exposure value is restored in auto mode. That happens before the capture reader is flushed, because the Lenovo rejects control changes after a flush (`0xC00D36B6`). The successful decision is remembered in memory for the process's next session, which then starts smooth in about 1 s and is still re-verified.

A camera the user set to manual exposure is left alone. Cost: one QPC read per frame, plus a 1-in-64-pixel luma sample while deciding (not once locked).

### After the fix (same PC, same scene)
| Path | FPS | Jitter | Max gap | Long gaps | Notes |
|---|---|---|---|---|---|
| IXC, DirectShow 1280×960 NV12 (OBS format), 15 s | **30.02** | 4.4 ms | 48 ms | 0 | client 1.1% CPU, 24.7 MB |
| IXC, MF 1080p30, first session | 30.1 from t≈4 s | 4.3 ms | 48 ms (after lock) | | first ~4 s at the camera's auto rate |
| IXC, MF 1080p30, next session | 30.1 from t≈2 s | 4.4 ms | | | remembered decision |
| App capture path, Lenovo 1080p | 30.1 | 4.3 ms | 48 ms | | +0.78–1.0 EV applied |
| Another app flips the camera back to auto mid-stream | recovers | | | | watchdog re-asserted once |

- Brightness after compensation stays within ~0.1–0.3 EV of auto exposure. Image noise is a little higher (it's software gain).
- The camera was confirmed back in auto exposure after every session.
- Resource baseline (`tests/bench_baseline.ps1`, default profile): 29.77 FPS (was 24.6), 36.5 ms latency, 0 drops. Camera service 14.4% of one core and 61.3 MB while streaming (was 16.2% / 60.6 MB). App startup 121 ms / 4.3 MB.

## Phase 7: face tracking

The full results are in [face-tracking-design.md](face-tracking-design.md) ("Final benchmark"). In short:
- With face tracking on, IXC Camera streaming is unchanged: 29.79 vs 29.81 FPS, 37.2 vs 37.0 ms latency, 182 vs 176 ms to the first frame, 0 dropped.
- The detector worker used 1.7–2.0% of one core (AVX2) or 3.4–3.9% (portable) at the idle/search rate.
- The frame thread spends 0.5–0.8 ms only on the few frames that feed a detection, 0.05–0.08 ms per frame on average.
- With a face in view: found in 100% of detections with valid landmarks, 3.5–7 detections/s. In the camera service tracking costs +5 % of one core and +6–7 MB while on (released when off). Warm-up takes 2–6 ms.

## Pending (not measured)
- Ultra Low (2 cores / 2–4 GB) and Low (dual-core / 4 GB) targets: NOT TESTED — REQUIRES USER ENVIRONMENT.
- 30-minute burn-in: done in Phase 10 (CHANGELOG, `tests/soak.ps1`): no drift.
- GPU utilization: no GPU work exists yet (Phase 6).
