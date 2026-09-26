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

## Pending (not measured)
- Ultra Low (2 cores / 2–4 GB) and Low (dual-core / 4 GB) targets: NOT TESTED — REQUIRES USER ENVIRONMENT.
- 30-minute burn-in: scheduled for Phase 10.
- GPU utilization: no GPU work exists yet (Phase 6).
