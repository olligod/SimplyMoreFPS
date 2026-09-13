# Changelog

## 0.3.14 - 2026-09-13

- Added smooth camera movement on space maps, including the planet background and orbital debris.
- Fixed stale depth textures on Linux when their source changes.
- Fixed a frozen new-colony setup screen on Linux.
- Fixed missing map labels on space maps on macOS.
- Fixed automatic fallback and recovery for unsupported camera effects on space maps.
- Removed long shader-compilation waits when entering or reloading a space map on Windows.

Restart RimWorld after updating.

## 0.3.13 - 2026-09-11

- Fixed new-game loading errors when the starting dialog opens on the loading thread.
- Fixed a renderer error when leaving a colony for the main menu.
- Fixed camera zoom while scrolling resource lists and other scroll views.
- Added compatibility with Nivarian Race, including correct effect positioning during pan and zoom.
- Fixed captured visuals not refreshing when camera effects or visible layers change while paused.
- Added automatic fallback and recovery for unsupported camera effects, with an explanatory popup. SSAA is unsupported, falls back to normal rendering.
- Fixed a Linux startup failure with GLVND graphics drivers.
- Removed a Mac renderer limit that could trigger unnecessary fallback.
- Cleaned up shared rendering code and moved mod-specific handling into compatibility adapters.

Restart RimWorld after updating.

## 0.3.12 - 2026-09-11

- Fix stale map rendering when hosting with Multiplayer.
- Clarify load order and compatibility notes.

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
