# Contributing to SH3VR

SH3VR modifies a closed-source Direct3D 8 game through a proxy DLL, so small
changes can affect unrelated render passes. Keep changes isolated and include
evidence for every rendering change.

## Pull requests

- Explain the exact hook, draw path, or host path being changed.
- State whether the change affects mono fallback, stereo, UI, timing, input, or
  OpenXR presentation.
- Include the build configuration and SHA-256 hashes of tested binaries.
- Include relevant log excerpts and the exact game/PC Fix/OpenXR setup.
- Do not include game files, save data, logs containing personal paths, crash
  dumps, or debugger captures.

## Rendering changes

1. Change one risky path at a time.
2. Keep `EnablePerDrawReplay=0` as a documented rollback for GPU watchdog or
   scene-transition failures.
3. Test startup, a save loaded from the first area, scene transitions, the
   bakery, Sewer, Otherworld Hospital, menus, and a gunshot.
4. Verify both eyes, head rotation, 6-DOF translation, UI, actor visibility,
   shadows/effects, and the desktop fallback.

All code, comments, identifiers, and log messages must be in English.
