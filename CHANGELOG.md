# Changelog

## 0.3.11 - 2026-09-10

- Wait for a usable game window during renderer startup.
- Fix Camera+ edge indicators drifting while panning.
- Fix rendering getting stuck when a map changes during preparation.
- Use normal rendering during Perspective Shift pawn control and Follow Me; resume SMF afterward.
- Reclaim unused Mac render textures when the memory budget is full.
- Share common camera and build code, and add a geometry provider API for mod compatibility.
- Clarify the Game FPS target: RimWorld defaults to 22; lower values allow more simulation time.

## 0.3.10 - 2026-09-10

- Fix mouse-wheel zoom while a building or zoning tool is selected.
- Support AntiAliasing's FXAA, SMAA and sharpening effects.
- Show a clear fallback error for unsupported AntiAliasing SSAA mode, including while paused.

## 0.3.9 - 2026-09-10

- Fix rendering getting stuck after resizing the Mac game window.
- Fix Linux startup on Ubuntu 22.04.
- Support older Simple Camera Setting versions.
- Wait for map loading to finish before reading camera compatibility settings.
- Show an error popup with copyable details if the renderer falls back.

## 0.3.8 - 2026-09-09

- Initial release.
