# SteamVR (OpenXR)

SH3VR uses SteamVR's OpenXR runtime, not a second OpenVR rendering/combat
implementation. Stereo rendering, post-processing, hands, weapon transforms,
roomscale, camera integration, melee damage, firearm rays and reticles continue
through the existing game proxy and shared-frame protocol. Weapon calibration
is not rewritten. Changing controller hardware can still require calibration.

## Selecting the runtime

In `sh3vr.ini`:

```ini
[OpenXR]
Runtime=Auto
SteamVRRuntimePath=
```

`Auto` selects once, before the first OpenXR call:

1. An inherited `XR_RUNTIME_JSON` from the launcher/environment takes precedence.
2. SteamVR's server together with its compositor or monitor in this Windows
   session selects SteamVR. Steam alone is not a signal.
3. Otherwise, a running Virtual Desktop Streamer selects VDXR, using its installed
   OpenXR manifest (or its registered manifest).
4. Without either signal, the system OpenXR runtime is used.

When both SteamVR and Virtual Desktop are running, SteamVR wins unless the
launcher supplied an explicit runtime. This supports Virtual Desktop transporting
SteamVR. There is no reliable launch-intent signal for an ordinary desktop launch
while both remain open: close SteamVR before launching directly with VDXR.
There is no mid-game runtime switching and no setting is written back by Auto.
Selection and detected processes are recorded in `sh3vr_host64.log`.

For SteamVR discovery the host checks the 64-bit OpenXR active/available runtime registration and then
Steam's default library. If SteamVR is in a different library and not registered,
set `SteamVRRuntimePath` to the absolute path of its `steamxr_win64.json`.
The selection uses process-local `XR_RUNTIME_JSON`; it does **not** change the
Windows runtime registration or the runtime used by other games.

Manual `Runtime=SteamVR` still forces SteamVR. Use `Runtime=System` to select the
system OpenXR runtime (for example VDXR), bypassing automatic detection.
System also honors an inherited `XR_RUNTIME_JSON`. Restart the game after a
runtime change. Do not run the game as administrator: elevated OpenXR loaders
may ignore environment overrides. A SteamVR request never silently falls back
to another runtime; inspect `sh3vr_host64.log` if initialization fails.

Connect the headset to SteamVR before launching the game. On Quest, connect
through Steam Link or Virtual Desktop's SteamVR path, not only a VDXR session.
Check that SteamVR sees both controllers. Desktop Steam Input/gamepad emulation
is not required by this implementation.

Leave existing timing settings intact. The optional refresh-rate extension is
requested only when available; otherwise select the refresh rate in the runtime.
`EyeWidth=0` and `EyeHeight=0` use the runtime recommendation; existing explicit
resolution settings are preserved and still take precedence.

## Bindings

Quest Touch retains the existing mapping in the main README. Index uses the same
sticks/triggers/grips; left A/B substitute Quest X/Y (inventory/map), and right
A/B remain confirm/cancel. Index system buttons remain reserved for SteamVR.

Vive wands use:

| Control | Action |
|---|---|
| Left pad | Movement / menu navigation |
| Right pad side touch (without click) | Snap turn |
| Right pad upper / lower click | Confirm / map |
| Left / right menu | Inventory / cancel |
| Left / right trigger | Aim / fire |
| Left grip or left pad click | Run / defend |
| Right pad center click | R3 camera control |
| Both pad center clicks | Camera Mod F1 shortcut |

Both aim and grip poses are bound for every profile. Profile rejection is logged
individually and does not disable the other profiles. Active per-hand profiles
are logged after connection changes. Input is cleared on loss of focus (including
SteamVR Dashboard), and inactive tracked poses are not forwarded to combat.

## Validation status and test checklist

This implementation is **not yet headset-validated on SteamVR**. Loading the
installed runtime and building successfully do not prove visual or combat parity.
Particularly, the existing 64 mm parallel render-camera convention has not been
redesigned for arbitrary canted headsets or every physical IPD.

Test on the intended headset:

- New game, both intro segments, skip intro, load another save.
- Stereo near hands and distant geometry; no per-eye color discrepancy.
- All buttons, menus, snap turn, roomscale, height calibration, F1 shortcut.
- Hand/weapon alignment, lighting and mutual depth, including near the face.
- Every supported weapon, melee contact damage, firearm direction and reticles.
- Dashboard open/close, controller sleep/reconnect: no stuck movement or fire.
- Indoor/outdoor performance, fog, water, subtitles and post-processing.

`sh3vr_host64.exe --probe-runtime` checks runtime discovery, headset availability
and binding suggestions without creating a graphics session or starting the
game. It needs SteamVR to see a headset; `XR_ERROR_FORM_FACTOR_UNAVAILABLE` (-35)
means that part of the test cannot proceed. Its log is next to the executable;
use a separate test directory if preserving the previous gameplay log matters.

The standalone selector tests can be built from an x64 MSVC developer prompt
(create `out/steamvr-probe` first):

```bat
cl /nologo /std:c++17 /EHsc /W4 /WX tests\runtime_selection_tests.cpp /Foout\steamvr-probe\runtime_selection_tests.obj /Feout\steamvr-probe\runtime_selection_tests.exe /link advapi32.lib
out\steamvr-probe\runtime_selection_tests.exe
```

They test all 32 process/launcher priority combinations, selection and error
handling with disposable fixtures, not rendering. `--check-runtime-selection`
logs the current decision and exits without initializing a runtime or headset.

Sources: [OpenXR loader runtime selection](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/specification/loader/runtime.adoc)
and [OpenXR interaction profiles](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#semantic-paths).
