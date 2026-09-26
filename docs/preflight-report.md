# IXC Camera — Phase 1 Preflight Report

Date: 2026-09-26
Machine role: developer reference system (benchmark profile "Target C" — not representative of the low-end target).

## Operating system

| Item | Value | Notes |
|---|---|---|
| Edition | Windows 11 Pro | |
| Version / build | 10.0.26200 | ≥ 22000, so `MFCreateVirtualCamera` / `IMFVirtualCamera` is available |
| Architecture | x64 | Build target: **x64** (ARM64 deferred) |
| Developer Mode | not enabled | Not required for the user-mode virtual camera path |

## Hardware

| Item | Value |
|---|---|
| CPU | AMD Ryzen 5 5600G, 6 cores / 12 threads |
| RAM | 15.85 GB total, ~9.2 GB free at preflight time |
| GPU (discrete) | AMD Radeon RX 6600, 8 GB dedicated, WDDM 3.2 |
| Direct3D | DirectX 12, feature levels up to 12_2 (D3D11 FL 11_1 available) |
| HW-accelerated GPU scheduling | Off |
| Disk | C: 307 GB free, D: 236 GB free, E: 206 GB free |

## Cameras

| Device | Driver provider | Instance | Notes |
|---|---|---|---|
| Lenovo FHD Webcam (VID 17EF / PID 4831) | Microsoft (inbox UVC) | `USB\VID_17EF&PID_4831&MI_00\...` | Primary validation camera. Format list gets enumerated in Phase 3 via Media Foundation. |
| Snap Camera | Snap Inc. | `ROOT\CAMERA\0000` | A root-enumerated legacy virtual camera. IXC won't touch it, but it gets recorded in the compatibility matrix because it can confuse apps that auto-pick a camera. |

## Toolchain

| Tool | Status |
|---|---|
| Git | 2.55.0 — OK |
| GitHub CLI | 2.101.0 — OK (optional) |
| winget | 1.29.380 — available |
| MSVC (`cl.exe`) | **MISSING** — blocking |
| Visual Studio / Build Tools | **MISSING** (no `vswhere`, no VS install dirs) — blocking |
| Windows SDK | **MISSING** (no `Windows Kits\10\Include`) — blocking |
| CMake | **MISSING** — blocking (ships with the VS C++ CMake component) |
| Ninja | missing — optional (ships with VS) |
| clang-cl / LLVM | missing — optional |

## Blockers

1. No C++ compiler, Windows SDK or CMake. None of the phases after this one can build without them. See `MISSING_TOOLS.md`.

## Decisions taken without asking (safe defaults)

- Build architecture: x64 only, Windows 11 (build ≥ 22000) only.
- Virtual camera: user-mode Media Foundation virtual camera (`MFCreateVirtualCamera`) with an in-proc COM media source. No kernel driver.
- UI: native Win32 + Direct2D/DirectComposition. No Electron, WinUI or XAML runtime dependency.
- Build system: CMake + MSVC. Tests use CTest with a tiny in-repo test harness, so there's no test-framework dependency to begin with.
