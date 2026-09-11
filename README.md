# Simply More FPS

**Feels like infinite FPS.** Your camera stays perfectly smooth, even when the game lags.

Near-zero overhead in Dubs profiling (**under 0.2%**). With **TPS boost**, you can even gain TPS.

## Install

For **RimWorld 1.6** on Windows, Linux and macOS. Requires **Harmony**.

Put `SimplyMoreFPS` in RimWorld's `Mods` folder. Enable Harmony and Simply More FPS, then restart.

**Load order:** place SMF after **Harmony**. It does not need to be at the bottom.

Safe to add or remove mid-save.

## Settings

Open **Options -> Mod settings -> Simply More FPS**.

- **Enabled** turns the mod on or off.
- **TPS boost**: turn down the **Game FPS target** to give simulation more time. RimWorld's default is **22**; try **15**. Camera FPS stays unchanged, but menus and animations may update less often.
- **Show FPS/TPS** adds a small counter below the date.

## Mod compatibility

Works with most mods. Mods that replace camera controls or rendering may need a compatibility patch.

Replaces Butter++. With **TPS boost**, you usually won't need FPS Stabilizer or Adaptive TPS either.

Checked on Windows, 9-10 September 2026:

- Camera+
- SimpleCameraSetting
- Perspective Shift
- Dubs Mint Minimap
- Dubs Performance Analyzer
- Interaction Bubbles
- Labels on Floor (continued)
- Vehicle Framework + Vehicle Map Framework
- As above, So below II
- Free UI Scaling
- Follow Me 1.6
- FPS Stabilizer
- Adaptive TPS
- AntiAliasing: FXAA, SMAA and sharpening. SSAA is unsupported.

### Pawn following

SMF and TPS boost pause during Perspective Shift pawn control or Follow Me. Both resume automatically afterward.

### Don't mix

- SMF and Butter++. Use SMF instead.
- Camera+ and SimpleCameraSetting/Perspective Shift. Those mods conflict with each other.

Version 0.3.11. Last updated: 10 September 2026.

## More from me

Join my [Discord](https://discord.gg/G9MWfC67mc) for updates on **RimWorld Online**, my upcoming multiplayer mod.

## Building

Needs the .NET 10 SDK and Python 3.12 or newer.
The native renderer also needs Visual Studio 2022 Build Tools on Windows, clang with X11 and OpenGL dev headers on Linux, and Xcode on macOS.

- Build: `python tools/build.py --force` (`python3` on Linux/macOS). Output: `dist/SimplyMoreFPS`.
- No RimWorld install? Add `--reference-assemblies`. Otherwise the Steam default location is used, or set `RIMWORLD_PATH`.
- Release ZIPs for every platform: download **SimplyMoreFPS-packages** from [CI](https://github.com/olligod/SimplyMoreFPS/actions/workflows/ci.yml).
- Managed tests: `dotnet run --project tests/Smf.Tests`. Native tests: add `--tests` to the renderer build.
- VS Code: the default build task builds the whole mod for the current platform.

Development: [CONTRIBUTING.md](https://github.com/olligod/SimplyMoreFPS/blob/main/CONTRIBUTING.md).
Modders: the camera API is in `src/Smf.Mod/API`.
