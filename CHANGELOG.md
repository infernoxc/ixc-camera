# Changelog

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
