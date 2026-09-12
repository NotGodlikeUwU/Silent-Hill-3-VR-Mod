
<img width="1201" height="233" alt="SH3VR LOGO WHITE" src="https://github.com/user-attachments/assets/61613126-f076-46d6-b200-966a48ea1418" />

A mod for *Silent Hill 3* that brings fully working VR mode into the game.

**WARNING: You must install Silent Hill 3 PC Fix by Steam006 and ZealotTormund's Camera Mod to play this mod.**

### Current status
Tested on **Meta Quest 3** through **Virtual Desktop**. The mod also includes
SteamVR/OpenXR support with automatic runtime detection and Touch/Index/Vive
controller bindings.

Use `SH3VRConfig.exe` next to `sh3.exe` to configure VR resolution, refresh
rate, input, graphics, safe performance options, and motion-weapon calibration.
The tool also includes an About tab with the project logo, version information,
credits, and project/dependency links.

### Installing
You MUST install [Silent Hill 3 PC Fix by Steam006](https://community.pcgamingwiki.com/files/file/1331-silent-hill-3-pc-fix-by-steam006/) and [Silent Hill 3 Camera Mod](https://github.com/zealottormunds/sh3cammod). Download the [Release](https://github.com/NotGodlikeUwU/Silent-Hill-3-VR-Mod/releases/tag/Beta_0.1.6), then copy its contents next to `sh3.exe`. The package includes `dinput8.dll`, `sh3vr_host64.exe`, `SH3VRConfig.exe`, `sh3vr.ini`, `sh3vr_weapons.ini`, and the `sh3vr_assets` folder. If you see gray screen in VR, copy contents from `GRAY SCREEN FIX` folder to the game .exe with OVERWRITE. Run `SH3VRConfig.exe` to adjust the mod or `sh3.exe` to play.

**Features in the current build:**
- Pre-Release 1.0.1 configuration tool (09.13.2026)
- Unlocked 90 FPS
- 6-DOF stereo rendering
- Controller input
- First Person using ZealotTormund's Camera mod
- Roomscale
- Player Height adaptation to Heather's height
- Motion-attached melee and firearm weapons with an in-world guide dot
- SteamVR/OpenXR runtime support with automatic runtime selection
- Per-weapon position, rotation, scale, and firearm aim offsets
- Working UI and subtitles
- Fully playable in the first person
- Smooth Turn and Snap Turn camera
- Optional experimental shooting while walking or running

### Known Issues & Limitations
- Some textures or models might be invisible in certain locations of the game
- Sewer location has glitched water reflections
- Missing shadows and enemy blood effects
- Knife hit registration is still unreliable with motion strikes
- Not the entire game has been thoroughly tested — unexpected issues may appear during a full playthrough

Please report any problems in the **Flat2VR Discord** or open an **Issue** on this repository.

### Configuration
The repository contains `sh3vr.ini.example` and `sh3vr_weapons.ini.example` templates. Copy them next to `sh3.exe` and rename them to `sh3vr.ini` and `sh3vr_weapons.ini`.

`sh3vr.ini` controls OpenXR/stereo timing, Camera Mod integration, roomscale, height calibration, and Quest 3 input. `sh3vr_weapons.ini` controls tracked weapon and left-hand pose, scale, and rotation. Weapon values are reloaded while the game is running; firearm `AimPitchDegrees` and `AimYawDegrees` align the reticle and bullet ray with the model.

### Controls (Quest 3)

| Input | Action |
|-------|--------|
| **Left Stick** | Movement (Up = Forward, Down = Backward, Left/Right = Strafe) |
| **Right Stick** (Left/Right) | Camera turn |
| **Right Stick Click** | Tab (SH3 camera control) |
| **Left Stick Click** | Run / Defend (Left Shift) |
| **Left Grip** | Run / Defend (Left Shift) |
| **Left Trigger** | Aim (Right Mouse Button) |
| **Right Trigger** | Fire (Left Mouse Button) |
| **A** | Action / Confirm |
| **B** | Cancel / Pause |
| **X** | Inventory |
| **Y** | Map |
| **Left Menu Button** | Cancel / Pause |

### Roadmap
- Various bug fixes

### Building from source

- Build the Win32 proxy with `sh3vr/sh3vr.sln` (`Release | x86`).
- Build the 64-bit host with `sh3vr_host64/sh3vr_host64.sln` (`Release | x64`).
- Build the configuration tool with `sh3vr_config/sh3vr_config.csproj` (`Release | Any CPU`).
- Stage a distributable package with `scripts/package.ps1 -Configuration Release`.

### Configuration tool

The bundled `SH3VRConfig.exe` is **Pre-Release 1.0.1** (09.13.2026). It edits
only the supported, safe subset of `sh3vr.ini` plus the motion calibration
values in `sh3vr_weapons.ini`; unknown settings and comments are preserved.

The `ShootWhileRunning` option is disabled by default. When enabled, it allows
firearm attacks during locomotion. When disabled, the mod also blocks the
game's contextual A-button attack path while aiming and moving.

The source tree also contains the controller integration, tracked weapon implementation, and left-hand assets used by the release package.

### Credits

ZealotTormunds for [Silent Hill 3 Camera Mod](https://github.com/zealottormunds/sh3cammod)
