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

## Pending (not measured)
- Ultra Low (2 cores / 2–4 GB) and Low (dual-core / 4 GB) targets: NOT TESTED — REQUIRES USER ENVIRONMENT.
- 30-minute burn-in: scheduled for Phase 10.
- GPU utilization: no GPU work exists yet (Phase 6).
