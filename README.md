# IXC Camera

A lightweight, local-first webcam enhancer for Windows 11. IXC Camera takes a normal webcam, applies image correction and optional face-aware effects, and exposes the result as a system camera called **IXC Camera**. Standard Windows camera apps can then select it like any hardware camera.

> **Status: v0.10.0 (all 14 build phases done, redesigned control panel; low-end hardware and Zoom/Teams/Discord still to be verified by users).** After installing, open **IXC Camera** from the desktop or Start Menu: it opens straight to the live preview with every feature on its own switch. Processing is CPU-first (SSE2); only digital zoom/crop moves to the GPU, and only when measured faster on your PC. **IXC Camera works as a selectable system camera** in Media Foundation and DirectShow apps and applies your picture settings (colour, tone, sharpness, mirror, digital zoom), adjusted live from the app. **Smooth motion** keeps the full frame rate in low light. Optional **face tracking** (libfacedetection) runs in the background within a CPU budget; built-in effects (Blush Tone, Basic Beauty, Portrait, colour and light looks) use it. Install with `IXC-Camera-Setup-x64.exe` (built by the release preset; see docs/installer.md). See [CHANGELOG.md](CHANGELOG.md), [docs/compatibility.md](docs/compatibility.md) and [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

## Design goals

- **Runs on weak PCs first.** The main target is dual-core laptops with 2–8 GB RAM and integrated graphics. Features degrade gracefully instead of failing.
- **Native.** C++, Media Foundation and Direct3D. No Electron, no browser runtime, no OBS required.
- **Nothing runs when nothing uses the camera.** The processing engine starts only when an app opens IXC Camera, and it releases the physical webcam when that app is done.
- **Local only.** No account, no cloud processing, no telemetry.
- **Honest.** It never advertises resolutions or frame rates the webcam can't deliver, and it doesn't claim every app will accept a software camera.

## Requirements

- Windows 11 x64 (build 22000 or newer). The Media Foundation virtual camera API doesn't exist on Windows 10.
- To build: Visual Studio 2022 or Build Tools with **Desktop development with C++**, the Windows 11 SDK, and C++ CMake tools.

## Build

```powershell
./scripts/build.ps1 -Preset debug -Test     # Debug build + unit tests
./scripts/build.ps1 -Preset release -Test   # Release build + unit tests
```

Details are in [docs/build.md](docs/build.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Build](docs/build.md)
- [Preflight report](docs/preflight-report.md)
- [Installer and exit codes](docs/installer.md)
- [Performance measurements](docs/performance.md)
- [Compatibility](docs/compatibility.md)
- [Face tracking design and benchmark](docs/face-tracking-design.md)
- [Security audit](docs/security-audit.md)
- [Release notes 0.9.0](docs/release-notes-0.9.0.md)
- [Known limitations](KNOWN_LIMITATIONS.md)
- [Security policy](SECURITY.md)

## License

MIT. See [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

IXC Camera is an independent project and is not affiliated with Snap Inc. or any camera vendor.
