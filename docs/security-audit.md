# Security and release audit (Phase 13, 2026-09-26, v0.9.0)

Re-run with `scripts/audit-release.ps1 -BuildDir build/release` (exit 0 = pass).

## Shipped binaries (IXCCameraSource.dll, IXCCamera.exe, ixc_vcam.exe, IXC-Camera-Setup-x64.exe)

| Check | Result |
|---|---|
| ASLR, high-entropy ASLR, DEP | PASS (all four) |
| Control Flow Guard (instrumented) | PASS |
| CET shadow-stack compatible | PASS |
| Imports only Windows system DLLs | PASS (5–16 each; 0.12 adds pdh (GPU/CPU readout) and windowscodecs (background pictures), both part of Windows) |
| Static CRT: no VC++ redistributable | PASS |
| No network APIs imported (winhttp, wininet, ws2_32, urlmon, webio): no downloads, no telemetry | PASS |

## Installer package
- The embedded payload is byte-identical to the build outputs (SHA-256): PASS.
- Elevation is requested explicitly (`requireAdministrator`); nothing else is installed (docs/installer.md).
- Not code-signed yet: SmartScreen shows "unknown publisher". Signing needs a certificate, which only the maintainer can supply.

## Source
- **Secret scan** of all tracked files (AWS/GitHub/Slack tokens, private keys, password/API-key assignments): no findings. No key, env or log files are tracked.
- **Vendored code:** libfacedetection, pinned; SHA-256 is verified at configure time; licence in THIRD_PARTY_LICENSES.md and shipped with the installer.
- **Vendored data:** the MediaPipe Selfie Segmentation weights, converted to a C++ include (`third_party/mediapipe_selfie_segmenter`); SHA-256 verified at configure time. It's a fixed operation list run by IXC's kernels. No model file, runtime or external data is read.
- **Static analysis** (MSVC `/analyze`, all IXC code, vendored code excluded): 13 warnings, none an actual defect:
  - 3 null-pointer warnings after a successful `GetAllocatedString` / `GetModuleHandle` (not null by API contract). Explicit guards were added anyway: `device_enum.cpp`, `activate.cpp` (camera service), `setup.cpp`.
  - 4 ignored return values of best-effort cleanup (`UnregisterWaitEx`, `CoInitializeEx`): intentional.
  - 4 SAL annotation mismatches on entry points (`wWinMain`, `DllGetClassObject`, `DllCanUnloadNow`): cosmetic.
  - 1 constant comparison (the `IXC_FACE_TRACKING` build switch): intentional.
  - 1 in the `ixc_probe` diagnostic tool (DirectShow moniker enumeration), not shipped.
- **Untrusted input:** profiles, `active-profile.json` and imported files are size-limited, strictly parsed, clamped, and never executed. Hostile-file unit tests cover this (test_active_profile, test_profile, app settings).
- **Memory safety runtime check (AddressSanitizer):** NOT TESTED; the VS component isn't installed. The 30-minute soak (Phase 10) showed flat memory and handles.

## Privacy
- Camera frames stay in memory only. No component writes images, and tests check preview pixels numerically.
- Logs hold no personal content. The installer log holds paths and results only.
