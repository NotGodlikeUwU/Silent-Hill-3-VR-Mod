# SH3VR

An unofficial experimental VR bridge for the PC version of Silent Hill 3.
The project combines a 32-bit Direct3D 8 proxy with a separate 64-bit OpenXR
host so that the original game and PC Fix can continue to run in their native
process.

This repository contains source code only. It does not contain Silent Hill 3,
game data, PC Fix files, or any other copyrighted game assets.

## Current architecture

```text
sh3.exe (32-bit, Direct3D 8)
    |
    +-- dinput8.dll (Win32 proxy)
    |     D3D8 hooks, camera/6-DOF, stereo draw replay, UI, input bridge
    |
    +-- shared frame and pose IPC
          |
          +-- sh3vr_host64.exe (64-bit)
                OpenXR session, D3D11 eye swapchains, controller actions
```

The proxy is loaded by the game as `dinput8.dll`. It starts
`sh3vr_host64.exe` from the same directory and keeps a flat-rendering fallback
when OpenXR is unavailable.

## Features in the current development build

- OpenXR presentation through a hidden 64-bit companion process.
- Head orientation and 6-DOF translation.
- Per-eye native targets and asymmetric OpenXR view FOV handling.
- D3D8 world/actor stereo replay used by the current VR build.
- Quest Touch controller actions for menu, inventory, aim, fire, and camera
  control.
- VR UI transfer and configurable image/color correction.
- Runtime resolution and display-refresh-rate settings in `sh3vr.ini`.

The renderer is still experimental. Water reflections in the Sewer and some
screen-space effects are known limitations and must be tested on every release.
Do not describe this repository as a finished or universally compatible port.

## Requirements

- A legal PC installation of Silent Hill 3.
- Silent Hill 3 PC Fix, installed and configured separately.
- Windows 10 or later on an x64 CPU.
- A working OpenXR runtime and a compatible headset or runtime bridge.
- Visual Studio with Desktop C++ tools (both Win32 and x64) and CMake 3.25 or
  newer. The checked-in Visual Studio projects use the v145 toolset; retarget
  them in Visual Studio if a different toolset is installed.

The game remains a 32-bit process. The host must be built for x64. Disable
third-party overlays while diagnosing crashes because they may install their
own D3D hooks.

## Build

Open a Developer PowerShell for Visual Studio from the repository root.

Build the x86 proxy:

```powershell
msbuild .\sh3vr\sh3vr.sln /t:Build /p:Configuration=Release /p:Platform=x86 /m
```

Build the x64 host with the vendored OpenXR SDK:

```powershell
cmake -S .\sh3vr_host64 -B .\out\host -G "Visual Studio 17 2022" -A x64
cmake --build .\out\host --config Release --target sh3vr_host64
```

If Visual Studio uses a different generator, replace only the generator name.

The proxy output is written to `sh3vr\Win32\Release\dinput8.dll`. The host
output is written to `out\host\Release\sh3vr_host64.exe`.

## Installation for testing

1. Back up the game directory and make sure `sh3.exe` is not running.
2. Copy the Release `dinput8.dll` and `sh3vr_host64.exe` beside `sh3.exe`.
3. Copy `sh3vr.ini.example` beside `sh3.exe` as `sh3vr.ini`.
4. Select the intended OpenXR runtime and connect the headset.
5. Start `sh3.exe`. The proxy starts the host automatically.
6. Keep `sh3vr.log` and `sh3vr_host64.log` when reporting a problem.

For a reproducible local package after building, run:

```powershell
.\scripts\package.ps1
```

The script creates a staging directory containing only the two mod binaries,
the configuration template, and SHA-256 hashes. It never copies game files.

## Configuration

`sh3vr.ini.example` is deliberately conservative. The `Image` section contains
the current color-correction and post-process controls. The `Timing` section
keeps the stable PC Fix path and marks experimental proxy timing switches;
change those switches only when testing a matching development build.
`EnableFullPassStereo` is kept disabled because it has previously caused
scene-transition instability.

The OpenXR eye size is a per-eye target and is independent of the desktop game
window resolution. `EyeWidth=0` and `EyeHeight=0` let the runtime choose its
recommended size.

## Development rules

- Source code, comments, identifiers, and log messages are written in English.
- Make one risky rendering change per test build.
- Preserve the mono fallback and never ship alternating stereo as the default.
- Record the exact game/PC Fix/runtime versions for bug reports.
- Compare SHA-256 hashes of build and installed binaries after every install.

See `CONTRIBUTING.md` for the test and review checklist.

## Legal notice

SH3VR is an unofficial fan project and is not affiliated with Konami, Silent
Hill 3, Steam006, or ThirteenAG. Silent Hill 3 and PC Fix remain the property
of their respective authors. The vendored MinHook and OpenXR SDK directories
retain their upstream license files. No game assets are redistributed here.

No license has been selected yet for original SH3VR source code. Until the
maintainer adds one, viewing the source does not grant permission to reuse it.
