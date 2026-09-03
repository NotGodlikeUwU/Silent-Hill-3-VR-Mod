# SH3VR x64 host

This executable owns the 64-bit OpenXR session and consumes BGRA8 frames
published by the 32-bit `dinput8.dll` proxy.

## Build with CMake

From the repository root, use a Developer PowerShell for Visual Studio:

```powershell
cmake -S .\sh3vr_host64 -B .\out\host -G "Visual Studio 17 2022" -A x64
cmake --build .\out\host --config Release --target sh3vr_host64
```

The OpenXR SDK is configured as a vendored subdirectory and the loader is
linked statically. The executable is written to
`out/host/Release/sh3vr_host64.exe`.

## Build with the Visual Studio solution

Open `sh3vr_host64/sh3vr_host64.sln`, select `Release | x64`, and build the
solution. The checked-in project uses the v145 toolset; retarget it if your
Visual Studio installation provides a different toolset. This route expects
the OpenXR loader library to have been generated under
`sh3vr_host64/build-vs18`; CMake is the recommended reproducible route.

## Runtime

For SteamVR selection, supported controller bindings and validation status, see
[SteamVR setup](../docs/steamvr.md). The existing OpenXR rendering path is shared
by SteamVR and the system runtime; no separate OpenVR game hooks are installed.

1. Make the intended headset runtime active before starting the game.
2. Copy the host next to `sh3.exe` and the Win32 proxy.
3. Start Silent Hill 3. The proxy starts the host without a console window and
   passes the game PID so the host can exit with the game.
4. Inspect `sh3vr_host64.log` and `sh3vr.log` next to the game executable.

The host initially supports mono/bring-up presentation and the current native
eye transport. It is not a standalone game executable and must not be started
without the proxy's shared-frame producer.

## Refresh rate

The OpenXR runtime refresh-rate request is configured in the game-side
`sh3vr.ini`:

```ini
[OpenXR]
TargetRefreshRate=90
RequestRefreshRate=1

[Timing]
UseThirteenAGFrameRateFix=1
FixedStep90Test=0
```

This requests headset presentation timing; Silent Hill 3 simulation timing is
controlled by PC Fix. Keep `EnableFullPassStereo=0` unless a matching test
build explicitly enables it.

All source code, identifiers, comments, and log messages are in English.
