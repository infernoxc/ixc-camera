# Changelog

## Unreleased — Phase 10 performance

### Measured
- **30-minute soak** (`tests/soak.ps1`), IXC Camera 1080p30 with Portrait + Beauty + Blush + Warm Glow and face tracking, Ryzen 5 5600G: 54,520 frames, 30.02 FPS, 0 dropped, jitter 4.3 ms, max gap 48 ms, latency 45.6 ms. Camera service memory flat at 67.9–68.8 MB for the whole run (no leak), back to 14 MB when streaming stopped; handles stable (1066 → 1025); CPU steady at 43–47% of one core.

### Changed
- Face tracking never uses the 320×180 input on PCs with under 4 GB RAM (saves ~3 MB of detector memory in the camera service there).

### Added
- `tests/soak.ps1` long-duration test (CPU, memory, handles every 30 s; frame statistics).

## Phase 9 — Profiles and hotkeys (2026-09-26)

### Added
- **Named profiles** in the app: the Profile box lists saved profiles (select to switch); type a new name and select Save to create one; Delete (with confirmation, never the last profile); Import… (validated, clamped with a warning count, never overwrites an existing profile) and Export… for backup. Switching keeps the camera and format in use and applies everything else live, including in IXC Camera. The active profile is remembered in `app-settings.json`.
- **Effects master switch** (`effectsEnabled` in profiles): turns all effects off without losing the list.
- **Global hotkeys** (while the IXC app runs, even minimized; `RegisterHotKey`, no keyboard hook or polling): Ctrl+Alt+F8 effects on/off, Ctrl+Alt+F9 / F10 next / previous profile, Ctrl+Alt+F11 mirror. Ctrl+Alt combinations leave plain F-keys to games. A checkbox turns them off; a hotkey another app already owns is reported.
- Tests: app settings parsing (never a path), master switch, UI smoke checks for profiles and hotkeys (29 UI checks).

## Phase 8 — Effects (2026-09-26)

### Added
- **Effect framework** (`src/effects`): built-in, original effects described by metadata (category, face needs, cost class, fallback), compiled once per settings change and rendered in place on the processed frame by a renderer with fixed, reused scratch memory. Nothing external is loaded or executed.
- **Effects:** Blush Tone (landmark-anchored cheeks, box fallback, fades without a face), Basic Beauty (edge-preserving skin smoothing inside the face), Portrait (soft blurred, muted, vignetted background around the tracked head and shoulders; centred subject without a face), Warm Glow, Cool Breeze, Mono, Vivid, Soft Light (lookup tables).
- The IXC Camera source and the app preview apply the same effects (WYSIWYG). Face-aware effects start face tracking automatically.
- App: an Effects section (8 checkboxes + a strength slider). The panel is re-laid out compactly so it fits a 768-pixel-high screen (UI test checks it).
- `ixc_probe --bench-effects`; 6 unit tests (120 total); UI smoke checks for effects.

### Measured (Ryzen 5 5600G, `--bench-effects`, per frame)
- 1080p: Blush 0.04 ms, Beauty 1.0 ms, Portrait 6.2 ms, each colour/light effect 1.3 ms, all 8 together 8.2 ms. 720p: all 3.7 ms.
- IXC Camera, 1080p, Portrait + Beauty + Blush + Warm: 9.4 ms per frame in the service, 28.9 FPS, latency 45.8 ms (37 ms without effects), 63 MB.

### Fixed during this phase
- Portrait first took 39 ms per 1080p frame in the service (per-pixel floating-point blending). It was rewritten with per-column tables, span splitting and integer blends: 6.2 ms.

## Phase 7 — Smooth motion and face tracking (2026-09-26)

### Fixed
- **Choppy video in low light** (reported in OBS). The root cause was the webcam's automatic exposure, which dropped the Lenovo FHD Webcam to an uneven 14–20 FPS on "30 FPS" modes. It was the same with the webcam direct; IXC's pipeline, queues and negotiation were measured clean. New **Smooth motion** (on by default, app checkbox, profile `smoothMotion`): fixes the exposure to one that fits a frame, verifies that the frame rate recovers, compensates brightness in software (≤ +1.5 EV) and restores automatic exposure when the session ends. Result: IXC in OBS's format went from ~20 FPS / 64 ms gaps to 30.0 FPS, 4.4 ms jitter and 0 long gaps. See docs/performance.md.

### Added
- **Face tracking** (optional, off by default; app checkbox "Face tracking", profile `faceTracking.enabled`):
  - libfacedetection (YuNet, BSD-3-Clause) vendored unmodified at a pinned commit, with build-time SHA-256 checks;
  - compiled as portable + AVX2 builds with runtime selection;
  - detection on a background thread at an adaptive 2–8 Hz within a per-tier CPU budget; input 240×135 (320×180 for small faces on fast CPUs, 160×90 fallback); turns itself off if the CPU can't keep up;
  - tracker with stable IDs, adaptive smoothing and prediction between detections; box, 5 landmarks, brow regions, mouth centre, head roll/yaw/pitch estimates, confidence; up to 4 faces;
  - runs inside IXC Camera while streaming (trace "FaceTracking") and in the app preview, with optional face markers (preview only);
  - no thread and no memory when off;
  - CMake option `IXC_WITH_FACE_TRACKING`;
  - 15 unit tests (114 total), a `face_engine_live` hardware test, `ixc_probe --bench-face`, and UI smoke checks.
- `ixc_probe`: `--dshow-capture` timing analysis and histogram, `--camera-controls`, `--set-exposure`, `--set-ae-priority`, `--smooth-motion`. There are also "StreamTiming" and "SmoothMotion" traces in the camera source and `tests/bench_baseline.ps1`.
- 11 exposure-governor unit tests (99 total). The UI smoke test covers the Smooth motion checkbox.

## 0.5.0 — Phase 6 GPU pipeline, benchmark-driven (2026-09-26)

### Added
- **Direct3D 11 compute implementation** of the whole pipeline (`processing/gpu`), byte-identical to the CPU path. Shaders are compiled at build time; no runtime compiler or extra DLLs.
- **Benchmarks**: `ixc_probe --bench-gpu` (CPU vs GPU per setting and size: wall time, CPU time consumed, upload/GPU/readback breakdown, output identity) and `--bench-gpu-memory`.
- **Adaptive CPU/GPU selection** (`BackendSelector` + `AdaptiveNv12Processor`), used by the IXC Camera source and the app preview. Only crop/zoom at ≥ 720p may move to the GPU, only when measured faster on the user's PC, never under 4 GB RAM. There's a byte-exact self-check, a sticky CPU fallback on any error, and immediate release when unused.
- Profile setting `gpu` (`auto` default, `off`) and an app checkbox. Direct3D is delay-loaded.
- Tests: GPU-vs-CPU exactness on WARP and hardware, 7 selector cases, gpu profile field (95 unit tests).

### Measured
- Zoom 1.5 + sharpen at 1080p in the camera service: 28.3% → 13.3% of one core with the GPU, at +57 MB while active. Colour/sharpen stay on the CPU (see docs/performance.md).

### Fixed during this phase
- GPU uploads through `UpdateSubresource` made driver memory grow to +200 MB, not returned on release. Replaced with fixed upload textures (flat at +41 MB, released when unused).

### Unchanged
- The CPU path is the default for all colour/tone/sharpen/mirror work and the fallback for everything.

## 0.4.0 — Phase 5 basic processing (2026-09-26)

### Added
- **Image pipeline** (`processing/image_pipeline`): brightness, contrast, saturation, warmth, tint, exposure, highlights, shadows, low-light boost, gamma, sharpness (SSE2), crop, digital zoom and mirror on NV12. Settings compile to lookup tables and geometry; neutral settings pass frames through; no per-frame allocation.
- **IXC Camera applies the user's settings** inside the Windows camera service, reloaded live while streaming (event-driven file watch), using a bounded pool of output frames.
- **Settings delivery**: the app publishes the active profile to `%ProgramData%\IXC Camera\active-profile.json`. The installer creates the folder with exact permissions (Users modify, LOCAL SERVICE read) and keeps it across upgrades.
- **App**: picture panel with 12 sliders, mirror and reset. The live preview runs the same pipeline (WYSIWYG). Settings are saved and published 250 ms after the last change.
- `ixc_probe --bench-pipeline` and `--source-effect-test`; CTest `vcam_source_effects`; UI smoke test covers the sliders, publishing and reset.
- 18 new unit tests (91 total), including SSE2-vs-scalar bit-exactness and hostile settings files.

### Fixed during this phase
- Sharpening cost 20 ms per 1080p frame; it's now 2 ms (SSE2, bit-identical).
- The source kept its settings watch and frame pool alive after an app stopped (Windows stops streams via `SetStreamState`, not a stop event).
- The frame pool pointer wasn't consistently lock-protected between the frame path and session end.
- A settings file caught mid-write briefly reset the picture to neutral; now the last good settings are kept.
- Installer upgrades deleted the published settings.

### Not yet
- Denoise (the profile field exists; no algorithm yet), a crop editor in the UI (crop works from the profile), GPU processing (Phase 6).

## 0.3.0 — Phase 4 IXC Camera system camera (2026-09-26)

### Added
- **`IXCCameraSource.dll`**: Media Foundation virtual camera source, loaded by Windows Frame Server only while an app uses IXC Camera. It wraps the physical webcam that Frame Server hands over, exposes the webcam's real NV12 modes, forwards camera controls, and has a single per-frame hook for the future image pipeline. User mode only, no driver.
- `ixc_vcam` tool: `status`, `register [--camera …]`, `unregister`.
- `scripts/install-ixc.ps1`: development installer and uninstaller. It checks prerequisites, installs to Program Files, rolls back on failure, records a SHA-256 manifest, and deletes in-use DLLs at reboot instead of restarting the camera service.
- App: IXC Camera status line and a **"Use this webcam for IXC Camera"** button (asks for administrator approval).
- `ixc_probe`: `--source-test` (loads the DLL in-process), `--list-dshow`, `--dshow-capture`.
- `scripts/trace-vcam.ps1`: captures the source's TraceLogging events.
- CTest: 4 IXC Camera tests (in-process source, Media Foundation capture, DirectShow capture, open/close cycles); skipped when not installed.
- `docs/compatibility.md`, `KNOWN_LIMITATIONS.md`.

### Fixed
- **Capture reconnect retried forever** when a camera opened but failed every read (e.g. busy with another app). The backoff now resets only after a real frame arrives.
- `MF_E_HW_MFT_FAILED_START_STREAMING` is now classified as "camera busy" (transient), not "device lost".
- Build: SDK headers are now treated as external (`/external:W0`), so `/W4 /WX` applies to IXC code only.

### Found and fixed during Phase 4 testing (before release)
- The source published its stream deselected, so Source-Reader apps got `E_INVALIDARG` on start.
- A second session subscribed twice to the physical stream's events (`MF_E_MULTIPLE_SUBSCRIBERS`).
- The installer's reboot-deletion call silently did nothing (PowerShell passed `""` for null), and its rollback stopped at the first failing step.

### Verified on the developer machine
- 59/59 unit tests; 9/9 CTest suites (4 physical-camera, 4 IXC Camera); UI smoke test 13/13.
- IXC Camera works through Media Foundation and DirectShow; install, uninstall, reinstall and in-use upgrade all verified. See `docs/compatibility.md` and `docs/performance.md`.

### Not verified
- OBS and Discord UI (installed, need a manual check), reboot persistence, unplug during an IXC session, sleep/wake, the UAC button flow, low-end hardware.

## 0.2.0 — Phase 3 camera capture (2026-09-26)

### Added
- `ixc_camera` library:
  - camera enumeration (physical cameras listed before virtual ones) and native-format enumeration;
  - format negotiation (`SelectFormat`) that never invents modes. It prefers the requested frame rate over uncompressed formats, and the per-tier defaults are 720p30 for Ultra Low/Low and 1080p30 otherwise;
  - asynchronous Source Reader capture with a newest-frame mailbox (zero-copy `IMFSample` handoff, drops counted), `MF_LOW_LATENCY`, and the physical camera held only while streaming;
  - frame statistics with fixed memory: FPS, jitter, stalls, timestamp jumps, capture→app latency, and an under-speed flag;
  - bounded reconnect: backoff retries (250 ms → 4 s, 6 attempts), then a zero-CPU wait for a device-arrival notification (`CM_Register_Notification`). Privacy denials are never retried.
- `ixc_processing` library: NV12 → BGRA conversion at display size (BT.601/BT.709, video/full range, mirror) with no per-frame allocation.
- `IXCCamera.exe` Phase 3 UI: camera and native-format pickers (Auto recommended), live preview, plain-language status and hints (under-speed / low light, privacy denial with a button that opens Settings, busy camera), list refresh on hot-plug, preview stops when minimized, selection saved to the default profile, per-monitor DPI, keyboard navigation.
- `ixc_probe` diagnostics tool (`--list`, `--capture`, `--cycles`) with machine-wide CPU and leak gates.
- Hardware integration tests in CTest (label `hardware`, skipped with code 77 when no camera exists) and `tests/ui_smoke.ps1`.
- 29 new unit tests (59 total).
- `docs/performance.md` with measured Phase 3 numbers.

### Fixed during this phase
- The preview first used Windows' RGB32 conversion (38.7 MB private, 25 threads). It now converts NV12 at display size (6.9 MB, 13 threads).

### Verified on the developer machine (Lenovo FHD Webcam)
- Debug and Release builds; 59/59 unit tests; 4/4 hardware tests; UI smoke test 11/11 checks.

### Not verified
- **Physical unplug/replug recovery: NOT TESTED — REQUIRES USER ACTION.** The logic is unit-tested (backoff policy, device matching), but the camera hasn't been unplugged during a live session yet.
- Windows sleep/wake during capture: not tested.
- Camera privacy "denied" path: code path exists but hasn't been exercised (it requires changing the user's privacy settings).
- Low-end hardware: not tested.

## 0.1.0 — Phase 2 skeleton (2026-09-26)

### Added
- CMake build (presets `debug`, `release`, `asan`) with MSVC `/W4 /WX`, Control Flow Guard, CET, ASLR/DEP and a static CRT.
- `scripts/build.ps1`: one-command configure/build/test from any PowerShell prompt.
- `ixc_core` library:
  - strict JSON reader/writer with size and depth limits, UTF-8 validation and duplicate-key rejection;
  - atomic file writes and size-limited reads;
  - synchronous rotating log (Normal/Debug modes, bounded disk use, newline injection stripped);
  - HRESULT diagnostics (`Error::Describe()`, error classification for recovery decisions);
  - versioned profile schema v1 with range clamping, change warnings and rejection of newer schemas;
  - profile store with path-safe file names.
- `IXCCamera.exe` skeleton window: single instance, creates its log and default profile under `%LOCALAPPDATA%\IXC Camera`.
- Dependency-free unit test harness with 30 tests.
- GitHub Actions CI skeleton (Debug + Release build/test, repository hygiene check).
- MIT license, security policy, contributing guide, third-party notice.

### Verified on the developer machine (Ryzen 5 5600G / RX 6600 / Windows 11 26200)
- Debug build + 30/30 unit tests: pass.
- Release build + 30/30 unit tests: pass.
- `IXCCamera.exe` Release: launches, rejects a second instance, exits cleanly. Idle footprint is 3.1 MB private bytes and 16 MB working set (mostly shared system DLLs).
- The Release exe has DYNAMICBASE, HIGHENTROPYVA, NXCOMPAT, CET and CF Guard. It imports only core Windows DLLs.

### Not verified
- `asan` preset: NOT TESTED. The C++ AddressSanitizer component isn't installed on the developer machine.
- CI workflow: not yet run on GitHub (the repository isn't published).
