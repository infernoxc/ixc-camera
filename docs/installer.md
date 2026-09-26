# Installer: IXC-Camera-Setup-x64.exe

Native installer and uninstaller (`src/installer`), built by the `release` preset at `build/release/src/installer/IXC-Camera-Setup-x64.exe`. The program files are embedded in it; no installer framework or runtime is needed.

## Usage

| Command | Action |
|---|---|
| `IXC-Camera-Setup-x64.exe` | Install, or upgrade an existing installation. Asks for administrator approval. Offers to open the app when it finishes. |
| `IXC-Camera-Setup-x64.exe /quiet` | Same, without any window. |
| `…\IXC Camera\IXC-Camera-Setup-x64.exe /uninstall` | Uninstall; asks whether to delete your profiles too (default: keep). This is what **Settings › Apps › Installed apps › IXC Camera › Uninstall** runs. |
| `… /uninstall /quiet [/removeuserdata]` | Silent uninstall; `/removeuserdata` also deletes `%LOCALAPPDATA%\IXC Camera` (profiles, logs). |

## What it does

1. Checks Windows 11 (build 22000+) and an x64 CPU.
2. Upgrades in place if IXC Camera is already installed (profiles and settings are kept).
3. Copies `IXCCameraSource.dll`, `IXCCamera.exe`, `ixc_vcam.exe`, the licences and itself (as the uninstaller) to `%ProgramFiles%\IXC Camera`.
4. Creates `%ProgramData%\IXC Camera` with exact permissions: Users modify, LOCAL SERVICE read (the camera service), SYSTEM/Administrators full.
5. Registers the media source (COM) and verifies the registration.
6. Registers the "IXC Camera" system camera and verifies that Windows lists it.
7. Adds "IXC Camera" shortcuts to the Start Menu and the desktop (all users, with the app icon), and the Apps & features entry. Upgrades overwrite the same shortcuts, so there are never duplicates.
8. Offers to open IXC Camera. The control panel opens straight to the live preview; the camera side needs no service of its own (Windows starts it when an app opens IXC Camera).

It installs nothing else: no drivers, services, startup entries, browser extensions or bundled software.

A failed step is never reported as success. The dialog names the stage and offers **Retry**. On Cancel (or with `/quiet`), everything done so far is removed again, and setup exits with the stage's code.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 3010 | Success; a file still loaded by the Windows camera service is removed at the next restart |
| 1 | Cancelled by the user |
| 2 | Unsupported Windows version (needs Windows 11, build 22000+) |
| 3 | Unsupported CPU architecture (needs x64) |
| 4 | Files or settings folder couldn't be written (disk, permissions) |
| 5 | The camera component (COM) couldn't be registered |
| 6 | Windows didn't accept or list the IXC Camera system camera |
| 7 | Uninstall incomplete (details in the log) |
| 8 | Unknown command-line option |
| 9 | A Start Menu or desktop shortcut couldn't be created |

## Log

`%TEMP%\IXC-Camera-Setup.log` (appended; for an elevated run this is the approving administrator's temp folder). Every step, file, registration result and exit code is recorded.

## Uninstall

Removes the system camera, the COM registration, `HKLM\SOFTWARE\IXC Camera`, the program folder, the Start Menu and desktop shortcuts, the Apps & features entry and `%ProgramData%\IXC Camera`. It deletes `%LOCALAPPDATA%\IXC Camera` only when you choose it. Other cameras and apps are never touched. Files still loaded by the camera service are deleted at the next restart (exit code 3010).

## Tested (2026-09-26, Windows 11 build 26200)
- Silent install over a development install: exit 3010 (the old DLL was held by the camera service). Camera registered and verified, Apps & features entry (0.9.0, 2.6 MB), shortcut, ACL as listed. IXC Camera streamed afterwards, and all 12 ctest suites and the UI smoke test passed against the installed app.
- Silent uninstall through the installed uninstaller: key, shortcut, settings key and folder, and the system camera removed; profiles kept; exit 3010.
- Reinstall: exit 3010, camera present. Unknown option: error dialog, exit code 8.
- Interactive dialogs (confirmation, Retry/Cancel, launch-on-finish): NOT TESTED automatically (UAC and dialogs need a person).
- Code signing: not signed yet. Windows SmartScreen will warn about an unknown publisher.

## Tested (0.10.0, 2026-09-26, `tests/installer_test.ps1`)
All 16 checks pass:
- silent install/upgrade (exit 3010: the old camera DLL is held by the camera service until restart);
- Start Menu and desktop shortcuts pointing to `IXCCamera.exe` with its icon;
- Apps & features entry 0.10.0; camera registered;
- user profiles untouched by the upgrade;
- a second install leaves exactly one shortcut each;
- the desktop shortcut launches the control panel;
- uninstall removes both shortcuts, the entry and the COM registration but keeps profiles;
- reinstall works.
