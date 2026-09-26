# Tool status

Last checked: 2026-09-26 (after the user installed Build Tools).

| Tool | Required? | Status | Purpose | Official source |
|---|---|---|---|---|
| Visual Studio Build Tools 2022 (17.14), C++ workload | Required | ✅ Installed | MSVC 19.44 compiler/linker | https://visualstudio.microsoft.com/downloads/ |
| Windows 11 SDK 10.0.26100 | Required | ✅ Installed (`mfvirtualcamera.h` present) | Media Foundation virtual camera, D3D, DXGI | Visual Studio Installer |
| CMake + Ninja (VS component) | Required | ✅ Installed | Build generation | Visual Studio Installer |
| C++ ATL | Required later (COM media source) | ✅ Installed | COM helpers for the virtual camera DLL | Visual Studio Installer |
| Git 2.55 | Required | ✅ Installed | Version control | https://git-scm.com |
| C++ AddressSanitizer | Optional | ❌ Missing | `asan` test preset (memory-safety test runs) | Visual Studio Installer → Modify → Individual components → "C++ AddressSanitizer" |
| LLVM / clang-cl | Optional | ❌ Missing | Second compiler for extra warnings | https://github.com/llvm/llvm-project/releases |
| OBS Studio, Discord, Zoom, Teams | Optional (Phase 11 testing) | Not checked | Compatibility matrix | Vendor sites |

Nothing blocking remains. The missing items have documented fallbacks: Debug and Release tests run without ASan, and MSVC is the only compiler for now.
