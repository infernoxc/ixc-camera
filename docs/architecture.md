# IXC Camera architecture

This document describes the target architecture. Items marked **(planned)** don't exist yet.

## Components

```
┌──────────────────────┐        profiles (JSON)       ┌──────────────────────────────┐
│ IXCCamera.exe (UI)   │ ───────────────────────────▶ │ Profile directory            │
│ • configure/preview  │                              └──────────────┬───────────────┘
│ • register camera    │                                             │ read on stream start
│ • can be closed      │                                             ▼
└──────────┬───────────┘                              ┌──────────────────────────────┐
           │ MFCreateVirtualCamera (planned)          │ IXC media source (planned)   │
           ▼                                          │ in-proc COM DLL, loaded by   │
┌──────────────────────┐   app opens "IXC Camera"     │ Windows Frame Server only    │
│ Windows camera stack │ ───────────────────────────▶ │ while a client streams       │
│ (Frame Server)       │ ◀─────────────────────────── │ webcam → GPU pipeline → out  │
└──────────────────────┘        processed frames      └──────────────────────────────┘
```

- **`ixc_core`** (static lib, exists now): strings, a strict JSON reader/writer, atomic file I/O, the rotating log, HRESULT diagnostics, and the profile model and store. It's shared by the UI and the media source.
- **`ixc_camera`** (static lib, exists now): enumerates cameras and their native modes, negotiates a format, and captures through the asynchronous Source Reader. Frames are handed off zero-copy through a single-slot newest-frame mailbox. A lost camera triggers a bounded backoff, then a zero-CPU wait for device arrival. The Phase 4 media source reuses this for the physical side.
- **`ixc_processing`** (static lib, started): CPU colour conversion (NV12 → BGRA at display size). The GPU pipeline arrives in Phase 6.
- **`IXCCamera.exe`** (Phase 3 UI exists): Win32 UI. It isn't needed for the camera to work once a profile is saved and the camera is registered.
- **IXC media source** (planned, Phase 4): a user-mode `IMFMediaSource` in a COM DLL, registered as a software camera via `MFCreateVirtualCamera` (Windows build 22000+). Windows Frame Server loads it on demand when a client opens IXC Camera, and it opens the physical webcam only for that session. No kernel driver.

## Rules the code follows

- **No work without a client.** No capture, inference or polling while no app streams IXC Camera.
- **Bounded everything.** Frame queues hold the newest frame only. Log files rotate with a fixed size cap. Parsers enforce input-size and nesting limits.
- **Untrusted input is data.** Profiles and effect packages are validated JSON. Effect IDs are restricted to `[a-z0-9._-]` and can never form a path. Nothing in a package is ever executed.
- **Never reset silently.** Profile loading clamps bad values and returns a warning for every change. An unreadable profile is reported and left on disk, never overwritten.
- **Actionable errors.** Failures carry an HRESULT, the system message and a stable stage name (`ixc::Error::Describe()`).

## IXC Camera system camera (Phase 4, implemented)

`IXCCameraSource.dll` (190 KB) is an in-process COM server. Its registered class is an `IMFActivate`. Windows Frame Server (the camera service) creates it when an app opens IXC Camera.

```
app opens "IXC Camera (Windows Virtual Camera)"
  → Frame Server loads IXCCameraSource.dll, creates Activate, sets attributes
  → Activate::ActivateObject
       asks for MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES (build 22621+): Frame Server hands over
       the physical camera it already manages (fallback: open by stored symbolic link)
  → MediaSource wraps it: one colour stream, NV12 types only, camera controls (IKsControl) forwarded
  → app selects a mode → Start → physical stream starts → each frame passes MediaStream::ProcessSample
       (identity today; the image pipeline plugs in here)
  → app stops → physical stream stops; the service drops back to 0% CPU
```

Decisions, verified on the dev machine:
- **Registration**: `MFCreateVirtualCamera(SoftwareCameraSource, Lifetime_System, Access_AllUsers, "IXC Camera", CLSID)`, then `AddDeviceSourceInfo(<physical link>)` and `Start`. This is done once by the installer (admin). No IXC process runs afterwards. Removal calls `Remove()`.
- **Why wrap through Frame Server**: the physical webcam stays managed by Windows, and switching between the physical camera and IXC Camera is instant and reliable (tested). IXC never opens the device exclusively on its own.
- **Why NV12 only**: every exposed mode must be processable by IXC effects. MJPG is decoded by Windows anyway.
- **Stream published as selected**: apps relying on the default selection (Source Reader) otherwise start with no stream. This was found and fixed during testing.
- **No file logging in the service**: TraceLogging provider `IXC.Camera.Source` ({6880FEE1-8B41-4322-ACF6-E6C1BF585B50}), captured with `scripts/trace-vcam.ps1`.
- **Footprint**: `%ProgramFiles%\IXC Camera\`, `HKLM\Software\Classes\CLSID\{3011A045-BC7A-469D-86D0-2800938E32BF}`, `HKLM\Software\IXC Camera` (records the wrapped webcam, since Windows doesn't expose it). Nothing else.

## Image pipeline and settings delivery (Phase 5, implemented)

```
IXCCamera.exe (user)                          Frame Server service (LOCAL SERVICE)
  profile in %LOCALAPPDATA%\IXC Camera          IXCCameraSource.dll
      │ slider change (debounced 250 ms)            │ stream started → BeginSession: load + watch
      ▼                                             │ file change → reload, validate, compile
  %ProgramData%\IXC Camera\active-profile.json ─────┘ (thread-pool wait; only while streaming)
  (atomic write; Users: modify, LOCAL SERVICE: read)  every frame → Nv12Processor → pooled sample
```

- **One pipeline everywhere.** `processing::Nv12Processor` runs in the source and in the app preview, so the preview shows exactly what apps receive.
- **Compiled settings.** Tone and colour become three 256-entry lookup tables. Sharpening is an SSE2 unsharp mask with a noise threshold and halo clamp. Crop, zoom and mirror are one bilinear rescale that preserves the output aspect ratio. Neutral settings pass frames through without copying.
- **Hostile-input handling.** The settings file crosses a privilege boundary (a user writes it, a service reads it). It's read with a 64 KB limit, parsed by the strict JSON reader, and every value is clamped. A missing file means neutral settings; a present-but-invalid file keeps the last good settings. Nothing in it is executed.
- **Idle means idle.** Windows stops a client via `SetStreamState(STOPPED)`. That ends the session: the settings watch stops and the frame pool (≤ 6 frames) is released.

## CPU / GPU processing (Phase 6, implemented)

- `processing::Nv12Processor` (CPU, SSE2) is the reference and the default. `processing::GpuNv12Processor` (Direct3D 11 compute, `cs_5_0`, shaders compiled at build time with `fxc`) reproduces it byte for byte.
- `processing::AdaptiveNv12Processor` wraps both. `BackendSelector` (pure logic, unit-tested) moves only crop/zoom work at ≥ 720p to the GPU, and only when measured faster on this PC (see docs/performance.md). GPU start-up happens on a thread-pool work item, the first GPU frame is checked against the CPU byte for byte, and any GPU error returns to the CPU for the session.
- The profile field `gpu` (`auto` | `off`) and the app checkbox let users force CPU-only.
- Software adapters (Microsoft Basic Render Driver) are refused. Direct3D DLLs are delay-loaded.

## Open design questions

1. **Processing a webcam another app controls** (Windows shared mode, `IMFSensorDevice::SetSensorDeviceMode(Shared)`). This would let IXC Camera run while another app uses the physical webcam, at that app's format. Deferred.
