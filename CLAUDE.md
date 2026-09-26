# IXC Camera — notes for Claude

Native Windows 11 webcam enhancer exposed as a Media Foundation software virtual camera. C++20 / MSVC / CMake. MIT license.

## Build and test
- `./scripts/build.ps1 -Preset debug -Test` and `./scripts/build.ps1 -Preset release -Test` (PowerShell). The script enters the VS dev environment itself.
- Tests: `build/<preset>/tests/ixc_unit_tests.exe [substring-filter]`.
- `vswhere` needs `-products *` to find Build Tools installs.
- Never edit source files with Windows PowerShell `Get-Content`/`Set-Content`. PS 5.1 reads BOM-less UTF-8 as Windows-1252 and corrupts non-ASCII characters (•, …, —). Use the Edit tool.
- PowerShell passes `$null` to a .NET `string` parameter as `""`. Use `[NullString]::Value` in P/Invoke calls.
- The `asan` preset needs the optional VS "C++ AddressSanitizer" component, which isn't installed. Report ASan runs as NOT TESTED.

## Rules
- The master spec's hard rules: no Electron, no kernel driver, no cloud, no per-frame allocations, no unbounded queues, no polling, no silent profile resets, never execute effect-package content.
- Never report a test as passed unless it ran. Missing hardware or apps → "NOT TESTED — REQUIRES USER ENVIRONMENT".
- Warnings are errors (`/W4 /WX`). Fix the cause.
- Every dependency goes into THIRD_PARTY_LICENSES.md before use.
- Update CHANGELOG.md at the end of each phase.

## Testing
- `ctest --preset release` runs the unit tests plus the `hardware` label, which needs a camera (exit 77 = skipped).
- `tests/ui_smoke.ps1 -Exe build/release/src/app/IXCCamera.exe` drives the UI. It checks preview pixels numerically and never saves camera images (they may show the user).
- `ixc_probe --capture N` / `--cycles N` for measurements. Record results in docs/performance.md with the machine name.

## IXC Camera system camera (Phase 4)
- Install/upgrade: `scripts/install-ixc.ps1 -BuildDir build/release` (elevated). Uninstall: `-Uninstall`. Status: `ixc_vcam status`.
- Always test the source DLL in-process first (`ixc_probe --source-test <dll>`). A crash inside Frame Server takes down every camera on the machine.
- After an upgrade, Frame Server may still run the old DLL. On the dev machine, restart it (`Restart-Service FrameServer`) only when no camera app is running. The installer never does this.
- Trace the source: `scripts/trace-vcam.ps1 -Start` … `-Stop`.
- PowerShell 5.1: `R` is an alias (Invoke-History), and `Start-Process -ArgumentList @(...)` doesn't quote arguments containing spaces. Pass one quoted string.

## Layout
- `src/common` strings, JSON, file I/O · `src/diagnostics` log, errors · `src/profiles` profile model/store
- `src/camera` enumeration, format selection, capture session, stats, reconnect · `src/processing` colour conversion
- `src/app` Win32 UI + preview · `src/tools/ixc_probe` diagnostics · `src/tools/ixc_vcam` registration
- `src/virtual_camera/source` IXCCameraSource.dll (Activate → MediaSource → MediaStream::ProcessSample hook) · `src/virtual_camera/registration.*`
- `src/processing/image_pipeline` CPU pipeline (reference for the GPU path) · `src/profiles/active_profile` settings file for the service · `src/app/adjustments_panel` sliders
- `src/processing/gpu` D3D11 path (byte-identical to CPU; `test_gpu_pipeline.cpp`) · `backend_selector` (when to use the GPU) · `adaptive_processor`
- Any change to `Nv12Processor` math must be mirrored in `gpu/pipeline.hlsl`. The exactness tests fail otherwise.
- Benchmarks: `ixc_probe --bench-pipeline`, `--bench-gpu`, `--bench-gpu-memory`. Record results in docs/performance.md.
- Phases 1–6 are done. Phase 7 (Smooth motion + face tracking, `src/face`, vendored `third_party/libfacedetection`) is implemented but not committed until the user says so. Don't start Phase 8 before Phase 7 is fully tested.
- `src/face`: tracker/cadence are pure logic (`test_face.cpp`). The detector is compiled twice (lfd_scalar/lfd_avx2, no `/arch:AVX2`). Benchmark: `ixc_probe --bench-face N` (needs a person in view for detection numbers).
- Frame Server stops a client with `SetStreamState(STOPPED)`, not an MEStreamStopped event. Free per-session resources there.

## Facts learned on the dev machine
- The Lenovo FHD Webcam sends 1080p30/720p30 as MJPG. Its "NV12 30 FPS" modes are decoded by Windows Frame Server.
- The camera drops to ~14–20 FPS in low light because of auto exposure: the root cause of "choppy" reports. Smooth motion (`camera/exposure_governor`) fixes it. Always confirm afterwards that exposure is back to "mode auto" (`ixc_probe --camera-controls`).
- The Lenovo rejects UVC control changes after the capture reader is flushed (`0xC00D36B6`), and rejects value 0 with the AUTO flag. Restore exposure before flushing, using the original value.
- `tests/bench_baseline.ps1` starts the app, which republishes the user's saved profile. Use `-Profile` (applied after the app step) to benchmark other settings.
- Writing files with PowerShell `UTF8Encoding($true)` adds a BOM: repo files have none.
- The Snap Camera legacy virtual camera is installed (root-enumerated, RGB24 720p). IXC lists it as virtual.
