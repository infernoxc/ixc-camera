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

## Layout
- `src/common` strings, JSON, file I/O · `src/diagnostics` log, errors · `src/profiles` profile model/store
- `src/camera` enumeration, format selection, capture session, stats, reconnect · `src/processing` colour conversion
- `src/app` Win32 UI + preview · `src/tools/ixc_probe` diagnostics
- Phases 1–3 are done. Next is Phase 4 (Windows 11 software virtual camera via MFCreateVirtualCamera). The open questions are in docs/architecture.md.

## Facts learned on the dev machine
- The Lenovo FHD Webcam sends 1080p30/720p30 as MJPG. Its "NV12 30 FPS" modes are decoded by Windows Frame Server.
- The camera drops to ~20 FPS in low light (auto exposure). That's expected, not an IXC bug.
- The Snap Camera legacy virtual camera is installed (root-enumerated, RGB24 720p). IXC lists it as virtual.
