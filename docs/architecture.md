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
- **`IXCCamera.exe`** (skeleton exists): Win32 UI. It isn't needed for the camera to work once a profile is saved and the camera is registered.
- **IXC media source** (planned, Phase 4): a user-mode `IMFMediaSource` in a COM DLL, registered as a software camera via `MFCreateVirtualCamera` (Windows build 22000+). Windows Frame Server loads it on demand when a client opens IXC Camera, and it opens the physical webcam only for that session. No kernel driver.

## Rules the code follows

- **No work without a client.** No capture, inference or polling while no app streams IXC Camera.
- **Bounded everything.** Frame queues hold the newest frame only. Log files rotate with a fixed size cap. Parsers enforce input-size and nesting limits.
- **Untrusted input is data.** Profiles and effect packages are validated JSON. Effect IDs are restricted to `[a-z0-9._-]` and can never form a path. Nothing in a package is ever executed.
- **Never reset silently.** Profile loading clamps bad values and returns a warning for every change. An unreadable profile is reported and left on disk, never overwritten.
- **Actionable errors.** Failures carry an HRESULT, the system message and a stable stage name (`ixc::Error::Describe()`).

## Open design questions (to be settled in Phase 4)

1. **Where the media source reads its profile.** Frame Server hosts custom media sources in a service context, which may not be able to read the user's `%LOCALAPPDATA%`. Candidates are passing the active profile path or contents at registration time, or a per-user ACL'd location under `%ProgramData%\IXC Camera`. This must be verified on the machine, not assumed.
2. **Registration lifetime.** Session vs. system lifetime and current-user vs. all-users (`MFVirtualCameraLifetime`, `MFVirtualCameraAccess`). The goal is that the camera survives reboots without the UI running.
3. **Sharing the physical webcam.** How IXC behaves when another app already holds the Lenovo camera (Frame Server shared mode vs. exclusive control).
