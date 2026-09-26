# IXC Camera

A lightweight, local-first webcam enhancer for Windows 11. IXC Camera takes a normal webcam, applies image correction and optional face-aware effects, and exposes the result as a system camera called **IXC Camera**. Standard Windows camera apps can then select it like any hardware camera.

> **Status: early development (v0.2.0, Phase 3 of 14).** Camera enumeration, format selection, capture with automatic reconnect, and a live preview work. The **IXC Camera system camera doesn't exist yet** (Phase 4), and there are no effects and no installer. See [CHANGELOG.md](CHANGELOG.md) and [docs/performance.md](docs/performance.md).

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
- [Security policy](SECURITY.md)

## License

MIT. See [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

IXC Camera is an independent project and is not affiliated with Snap Inc. or any camera vendor.
