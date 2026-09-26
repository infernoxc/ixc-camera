# IXC Camera — notes for Claude

Native Windows 11 webcam enhancer exposed as a Media Foundation software virtual camera. C++20 / MSVC / CMake. MIT license.

## Build and test
- `./scripts/build.ps1 -Preset debug -Test` and `./scripts/build.ps1 -Preset release -Test` (PowerShell). The script enters the VS dev environment itself.
- Tests: `build/<preset>/tests/ixc_unit_tests.exe [substring-filter]`.
- `vswhere` needs `-products *` to find Build Tools installs.
- The `asan` preset needs the optional VS "C++ AddressSanitizer" component, which isn't installed. Report ASan runs as NOT TESTED.

## Rules
- The master spec's hard rules: no Electron, no kernel driver, no cloud, no per-frame allocations, no unbounded queues, no polling, no silent profile resets, never execute effect-package content.
- Never report a test as passed unless it ran. Missing hardware or apps → "NOT TESTED — REQUIRES USER ENVIRONMENT".
- Warnings are errors (`/W4 /WX`). Fix the cause.
- Every dependency goes into THIRD_PARTY_LICENSES.md before use.
- Update CHANGELOG.md at the end of each phase.

## Layout
- `src/common` strings, JSON, file I/O · `src/diagnostics` log, errors · `src/profiles` profile model/store · `src/app` Win32 UI
- Phases 1–2 are done. Next is Phase 3 (camera capture: MF enumeration, format negotiation, preview, reconnect).
