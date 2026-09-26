# Changelog

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
