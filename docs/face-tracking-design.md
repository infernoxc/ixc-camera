# Phase 7 face tracking: design notes

Status: **implemented (approved: libfacedetection, 240×135 default, AVX2 + portable builds). Final benchmark complete, including detection with the user in view; low-end hardware not tested.** Architecture: docs/architecture.md, "Face tracking".

## Baseline before face tracking (2026-09-26, `tests/bench_baseline.ps1`, default profile)

| Measurement | Value |
|---|---|
| App startup | 120 ms; 4.3 MB idle |
| Camera service, idle | 0.00% CPU, 10.5 MB |
| IXC Camera open → first frame | 203 ms mean, 211 ms max |
| Streaming | 24.6 FPS, 37.6 ms latency, 0 dropped |
| Camera service while streaming | 16.2% of one core, 60.6 MB |

## Detector benchmark with the user in view (Ryzen 5 5600G, one thread, Lenovo FHD Webcam)

10 frames per source resolution, each detected 3×. Frames were kept in memory only. The face sat left of centre and low in the frame: box centre ≈ (0.30, 0.72), about 22% of the frame width. "lmOK" = all five landmarks inside the face box, eyes above nose, nose above mouth, left/right in order. "Scale" = area-average downscale from the full frame.

### 1280×720 source

| Detector | Input | Found | Confidence | Landmarks OK | Jitter | Scale + detect | Max FPS | CPU @ 5 Hz |
|---|---|---|---|---|---|---|---|---|
| libfacedetection, no SIMD | 320×180 | 10/10 | 65 | 10/10 | 0.7% | 1.3 + 22.4 ms | 41 | 11.2% |
| libfacedetection, AVX2 | 320×180 | 10/10 | 65 | 10/10 | 0.7% | 1.3 + 8.9 ms | 98 | 4.4% |
| libfacedetection, no SIMD | 240×135 | 10/10 | 72 | 10/10 | 1.4% | 0.9 + 14.6 ms | 63 | 7.3% |
| libfacedetection, AVX2 | 240×135 | 10/10 | 72 | 10/10 | 1.4% | 0.9 + 4.7 ms | 165 | 2.3% |
| libfacedetection, no SIMD | 160×90 | 10/10 | 50 | **7/10** | 0.6% | 0.7 + 5.2 ms | 172 | 2.6% |
| libfacedetection, AVX2 | 160×90 | 10/10 | 50 | **7/10** | 0.6% | 0.7 + 1.6 ms | 432 | 0.8% |
| Windows FaceDetector | 320×180 | **4/10** | n/a | none | – | 5.5 ms wall | – | – |
| Windows FaceDetector | 240×135 / 160×90 | **0/10** | n/a | none | – | – | – | – |

### 1920×1080 source

| Detector | Input | Found | Confidence | Landmarks OK | Jitter | Scale + detect | Max FPS | CPU @ 5 Hz |
|---|---|---|---|---|---|---|---|---|
| libfacedetection, no SIMD | 320×180 | 10/10 | 64 | 10/10 | 1.6% | 1.8 + 21.9 ms | 43 | 10.9% |
| libfacedetection, AVX2 | 320×180 | 10/10 | 64 | 10/10 | 1.6% | 1.8 + 8.3 ms | 101 | 4.2% |
| libfacedetection, no SIMD | 240×135 | 10/10 | 34 | 10/10 | 2.3% | 1.4 + 14.1 ms | 64 | 7.0% |
| libfacedetection, AVX2 | 240×135 | 10/10 | 34 | 10/10 | 2.3% | 1.4 + 4.7 ms | 159 | 2.3% |
| libfacedetection, no SIMD | 160×90 | 10/10 | 43 | 10/10 | 4.2% | 1.2 + 4.7 ms | 156 | 2.3% |
| libfacedetection, AVX2 | 160×90 | 10/10 | 43 | 10/10 | 4.2% | 1.2 + 1.6 ms | 335 | 0.8% |
| Windows FaceDetector | all sizes | **0/10** | n/a | none | – | – | – | – |

The no-SIMD and AVX2 builds returned **identical** boxes and landmarks. The no-SIMD build is fully functional, just slower.

### Memory (fresh process per case)

| Detector | Input | Peak private | Steady private |
|---|---|---|---|
| libfacedetection (either build) | 320×180 | +3.5–4.0 MB | +0.2–1.2 MB |
| | 240×135 | +3.1 MB | +0.9 MB |
| | 160×90 | +1.2 MB | +0.7 MB |
| Windows FaceDetector | – | +12.2 MB after creation | – |

Binary size for both builds plus the model: ~0.77 MB. There's no model file and no runtime.

## Recommendation

**libfacedetection (YuNet) at a pinned commit, BSD-3-Clause, compiled twice (no-SIMD + AVX2) and chosen at runtime by CPU check.**

- It's the only candidate that found the face in every frame and gives eye, nose and mouth landmarks.
- Windows FaceDetector found the face in 4 of 60 attempts, gives no landmarks, and costs 3× the memory. Rejected.
- Default detector input: **240×135**. Landmarks were plausible in 10/10 frames at both source resolutions, detection costs 4.7 ms with AVX2 and 14.6 ms without, and peak memory is about 3 MB.
- **320×180** only on fast CPUs, when a smaller face needs detecting. **160×90** as a last-resort fallback on the weakest machines (face box reliable; landmarks failed 3/10 at 720p, so landmark-dependent effects would be held or faded there).
- The effects must never wait for the detector: detection runs in the background at a reduced rate, with smoothing/prediction in between.
- Old Pentium/Celeron dual-cores (no AVX2) run the no-SIMD build. Estimated at 2–3× slower per core than this Ryzen: about 30–45 ms per detection at 240×135, so 2–3 detections per second would cost ~6–13% of one core. **This is an estimate, not a measurement;** it must be verified on real low-end hardware.

## Implementation (Phase 7)

As approved:
- libfacedetection at the pinned commit, vendored unmodified with SHA-256 checks;
- portable and AVX2 builds with runtime dispatch;
- 240×135 default input, 320×180 for small or undetected faces on fast CPUs, 160×90 fallback.

Additions made during implementation:
- **Sparse input sampler** instead of a full area average on the frame thread: 0.5–0.8 ms instead of 0.9–1.8 ms. `ixc_probe --bench-face` compares both on live frames.
- **Dark-frame normalization** of the detector input (gain up to 4×). With Smooth motion in a dim room, raw frames measured a mean luma of ~20 (auto exposure: 35–58), which is darker than anything in the detector benchmark.
- **Budget-driven cadence** (see architecture.md): the detector never runs more than the tier's share of one core. It degrades to 160×90, then switches itself off, rather than slowing the camera.
- **Tracker:** IDs, adaptive smoothing (still → strong, moving → light), prediction up to 200 ms, confidence decay and a drop after 2 misses. It holds landmarks when a detection's landmarks are implausible. It also derives brow regions, mouth centre and head roll/yaw/pitch estimates.

## Final benchmark (2026-09-26, Ryzen 5 5600G, Lenovo FHD Webcam at 30 FPS)

Measured with `ixc_probe --bench-face 12` (engine on live frames, app-side path) and `tests/bench_baseline.ps1` with face tracking off vs on (IXC Camera inside the Windows camera service). **Nobody was in view during this run.** The engine was searching at 2–3.3 detections/s, which is the idle rate. With a face in view it runs at 3–8/s, and the cost scales with that rate (see the detector table above).

### Engine (app capture path)

| Source | Build | Input | Det/s | Detect ms avg / max | Frame-thread cost (staging avg / max) | Worker CPU (% of one core) | RAM peak / after stop | Warm-up |
|---|---|---|---|---|---|---|---|---|
| 720p | AVX2 | 320×180 | 2.7 | 7.3 / 9.4 | 0.72 / 1.43 ms; average per frame 0.07 ms | 2.0% | +0.3 / +0.0 MB | 2.8 ms |
| 720p | portable | 240×135 | 2.7 | 15.1 / 16.8 | 0.54 / 0.69 ms; average per frame 0.05 ms | 3.4% | +1.2 / +0.3 MB | 6.0 ms |
| 1080p | AVX2 | 320×180 | 2.7 | 7.9 / 9.1 | 0.84 / 1.02 ms; average per frame 0.08 ms | 1.8% | +0.5 / +0.0 MB | 2.5 ms |
| 1080p | portable | 240×135 | 2.7 | 15.0 / 19.5 | 0.54 / 0.61 ms; average per frame 0.05 ms | 3.9% | +1.1 / +0.3 MB | 5.7 ms |

- The camera stayed at 30.0–30.2 FPS in every run. `Start()` returns in 0.1–0.2 ms, and the warm-up runs on the worker.
- The AVX2 build chose 320×180 because no face had been found for 2 s on a fast CPU. The portable build stays at 240×135 (its 240×135 cost is above the 6 ms threshold).

### IXC Camera in the camera service (1080p30, default profile)

| | Face tracking off | Face tracking on |
|---|---|---|
| Streaming | 29.71 FPS, 0 dropped | 29.69 FPS, 0 dropped |
| Latency (capture → app) | 37.30 ms | 37.20 ms (tracking is asynchronous: the video never waits for it) |
| Open → first frame | 179 ms | 181 ms |
| Camera service CPU | 20.08% of one core | 20.09% of one core |
| Camera service RAM | 64.8 MB | 64.8 MB |
| Tracking (trace) | – | AVX2, 320×180, 2.3 det/s, 7.2 ms each, worker 1.7% of one core, staging 0.80 / 1.10 ms, warm-up 1.9 ms |

- **Startup cost:** 2–6 ms of one-time warm-up on the worker thread, and 0.1–0.2 ms on the caller.
- **Binary size:** +0.59 MB per binary.
- **Off:** no thread and no memory. The frame path checks one flag. Unit tests assert the engine thread exits when tracking is turned off, and the UI smoke test checks the thread count.

### With the user in view (2026-09-26, same PC, normal room light)

The face was roughly centred and small in frame: box width 11–12% of the frame.

**Detector input check** (same AVX2 detector, 10 frames each, 240×135):

| Source | Input | Found | Confidence | Landmarks plausible | Box IoU vs area average |
|---|---|---|---|---|---|
| 720p (raw luma 64) | engine (sampler + normalization ×1.7) | 10/10 | 0.79 | 10/10 | 0.92 |
| | sampler, raw | 10/10 | 0.81 | 10/10 | 0.97 |
| | area average, raw | 10/10 | 0.81 | 10/10 | – |
| 1080p (raw luma 59) | engine (sampler + normalization ×1.9) | 10/10 | 0.90 | 10/10 | 0.91 |
| | sampler, raw | 10/10 | 0.89 | 10/10 | 0.97 |
| | area average, raw | 10/10 | 0.89 | 10/10 | – |

The cheaper sampler matches the full area average. The normalization doesn't cost detections at normal brightness; its box differs slightly (IoU 0.91–0.92), within what the tracker smooths.

**Engine on live frames** (`ixc_probe --bench-face 12`):

| Source | Build | Input chosen | Detections/s | Detect ms avg / max | Frames with a tracked face | Landmarks valid | Worker CPU (% of one core) | Frame-thread staging avg / max | Per-frame average | RAM (probe process) | Camera FPS |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 720p | AVX2 | 320×180 (small face) | 7.0 | 6.5 / 9.2 | 99% | 100% | 4.3% | 0.58 / 0.95 ms | 0.14 ms | +0.3 MB | 30.1 |
| 720p | portable | 240×135 | 5.2 | 14.8 / 15.8 | 100% | 100% | 7.7% | 0.53 / 0.64 ms | 0.09 ms | +1.0 MB | 30.0 |
| 1080p | AVX2 | 240×135 | 3.5 | 6.2 / 6.6 | 99% | 100% | 2.1% | 0.54 / 0.69 ms | 0.06 ms | +0.3 MB | 30.1 |
| 1080p | portable | 240×135 | 4.7 | 14.7 / 15.7 | 100% | 100% | 6.1% | 0.54 / 0.68 ms | 0.09 ms | +1.1 MB | 30.0 |

"Frames with a tracked face" includes the first frames before the first detection. Detections/s varies with movement: 3 Hz when still, up to 8 Hz while moving, capped by the budget. The portable build stayed within its 10% budget.

**IXC Camera in the camera service** (1080p30, `bench_baseline.ps1 -ProfileFile`, off then on, face in view):

| | Tracking off | Tracking on |
|---|---|---|
| Streaming | 29.81 FPS, 0 dropped | 29.79 FPS, 0 dropped |
| Latency | 37.04 ms | 37.22 ms |
| Open → first frame | 176 ms | 182 ms |
| Camera service CPU | 14.8% of one core | 19.9% of one core (+5.1: worker 3.6% + staging) |
| Camera service RAM (sampled every 100 ms, 2 runs each) | 66.2 MB average | **72.0 MB average, 73.2 MB max (+6–7 MB)** |
| Tracking (trace) | – | AVX2, 320×180, 6.3 detections/s; face in 129/129 detections, landmarks valid 129/129; 6.2 ms each (max 9.0) |

The service holds more memory than the probe process: the detector allocates its working buffers on each run (~6 MB at 320×180), and the camera service's heap keeps them committed between detections. Everything is returned when tracking stops (service back to 16.7 MB idle).

### Still to measure
- Real low-end hardware (Pentium/Celeron, 2 GB RAM): NOT TESTED — REQUIRES USER ENVIRONMENT. The portable build and the budget/fallback logic are what that hardware runs.
- Possible later optimization: cap the service's detector memory at 240×135 (~3 MB) on PCs with < 4 GB RAM, or give the detector a private heap that's released between detections.
