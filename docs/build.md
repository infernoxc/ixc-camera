# Building IXC Camera

## Prerequisites

| Tool | Notes |
|---|---|
| Visual Studio 2022 17.10+ or Build Tools 2022 | Workload **Desktop development with C++** |
| Windows 11 SDK 10.0.22000+ (10.0.26100 tested) | Provides `mfvirtualcamera.h` |
| C++ CMake tools for Windows | Provides CMake ≥ 3.28 and Ninja |
| C++ AddressSanitizer (optional) | Needed only for the `asan` preset |

No other dependencies are downloaded. The build uses the static CRT, so the outputs don't need the VC++ redistributable.

## Commands

`scripts/build.ps1` finds Visual Studio with `vswhere`, enters the x64 developer environment for that process only, and runs a CMake preset:

```powershell
./scripts/build.ps1 -Preset debug -Test
./scripts/build.ps1 -Preset release -Test
./scripts/build.ps1 -Preset asan -Test      # requires the ASan component
./scripts/build.ps1 -Preset release -Clean  # fresh configure
```

From a "Developer PowerShell for VS 2022" you can use CMake directly:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## Presets

| Preset | Configuration | Purpose |
|---|---|---|
| `debug` | Debug | Day-to-day development |
| `release` | RelWithDebInfo (`/O2`, PDBs kept separate) | Shipping binaries |
| `asan` | Debug + `/fsanitize=address` | Memory-safety test runs |

Build output goes to `build/<preset>/`. The app is `src/app/IXCCamera.exe` and the tests are `tests/ixc_unit_tests.exe`.

## Compiler policy

All targets link the `ixc_settings` interface target (see [cmake/CompilerSettings.cmake](../cmake/CompilerSettings.cmake)):

- `/W4 /permissive- /WX` plus extra off-by-default warnings. Warnings fail the build.
- `/sdl`, Control Flow Guard, CET shadow-stack compatibility, ASLR (high-entropy) and DEP.
- UTF-8 source and execution charset.

## Running a subset of tests

```powershell
build/debug/tests/ixc_unit_tests.exe Profile_   # substring filter
```
