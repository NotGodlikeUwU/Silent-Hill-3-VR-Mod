**Silent Hill 3 VR Mod**

A mod for *Silent Hill 3* that brings fully working VR mode into the game.

**WARNING: You must install Silent Hill 3 PC Fix by Steam006 to play this mod.**

### Current status
Tested on **Meta Quest 3** through **Virtual Desktop**. Other headsets and runtimes are not yet validated.

You can adjust the in-game VR resolution and FPS lock (not tested, i recommend leaving 90 FPS) in `sh3vr.ini` to better match different headsets.

### Installing
You MUST install [Silent Hill 3 PC Fix by Steam006](https://community.pcgamingwiki.com/files/file/1331-silent-hill-3-pc-fix-by-steam006/) and [Silent Hill 3 Camera Mod](https://github.com/zealottormunds/sh3cammod). Download the [Release](https://github.com/NotGodlikeUwU/Silent-Hill-3-VR-Mod/releases/tag/Beta_0.1), then copy its contents next to `sh3.exe`. The package includes `dinput8.dll`, `sh3vr_host64.exe`, `sh3vr.ini`, `sh3vr_weapons.ini`, and the `sh3vr_assets` folder.

**Features in the current build:**
- Unlocked 90 FPS
- 6-DOF stereo rendering
- Controller input (mapped for Quest 3 controllers)
- First Person using ZealotTormund's Camera mod
- Roomscale
- Player Height adaptation
- Motion-attached melee and firearm weapons
- Controller-relative firearm aiming, native damage, and an in-world guide dot
- Per-weapon position, rotation, scale, and firearm aim offsets
- Working UI and subtitles
- Fully playable in the default camera mode

### Known Issues & Limitations
- Some textures or models might be invisible in certain locations of the game
- Intro cutscene has invisible environment
- Sewer location has glitched water reflections
- Missing shadows and enemy blood effects
- Missing muzzle flash effects
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
- Fixes for missing shadows and blood effects
- SteamVR support
- Various bug fixes

### Building from source

- Build the Win32 proxy with `sh3vr/sh3vr.sln` (`Release | x86`).
- Build the 64-bit host with `sh3vr_host64/sh3vr_host64.sln` (`Release | x64`).
- Stage a distributable package with `scripts/package.ps1 -Configuration Release`.

The source tree also contains the controller integration, tracked weapon implementation, and left-hand assets used by the release package.

### Credits

ZealotTormunds for [Silent Hill 3 Camera Mod](https://github.com/zealottormunds/sh3cammod)
