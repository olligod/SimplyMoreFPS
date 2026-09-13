# Working on SMF

Keep changes focused. Follow `.editorconfig`, use the surrounding code's layout, and leave
blank lines between logical steps. Short comments should explain why something is needed.
Put player-facing text in `Languages/English/Keyed/SimplyMoreFPS.xml`.

## Build

Install .NET 10 and Python 3.12 or newer, plus the tools for your platform:

| Platform | Native tools |
| --- | --- |
| Windows x64 | Visual Studio 2022 Build Tools: Desktop development with C++ and Windows SDK |
| Linux x64 | clang, X11, Xext, Xi, OpenGL and GLX development headers |
| macOS Intel or ARM | Xcode command-line tools |

On Ubuntu: `sudo apt-get install clang zlib1g-dev libx11-dev libxext-dev libxi-dev libgl-dev libglx-dev mesa-common-dev`.
On Fedora: `sudo dnf install clang zlib-ng-compat-devel libX11-devel libXext-devel libXi-devel libglvnd-devel`.
On macOS: `xcode-select --install`.

Run `python tools/build.py --force` on Windows, or `python3 tools/build.py --force` on Linux/macOS.
It builds the mod and your platform's native renderer into `dist/SimplyMoreFPS`.
`--force` replaces the previous output. Add `--dev` to include the development tools.

The Windows build compiles the fixed space shaders from
`src/Smf.Renderer/windows/scene_shaders.h` and embeds their bytecode in the renderer.
The generated header stays in the build output; edit the shader source and rebuild.

The Steam install is found automatically; set `RIMWORLD_PATH` for another location.
Add `--reference-assemblies` to build without installing the game. Those references stay out
of the mod. A RimWorld install is still needed to test in game.

Open `SimplyMoreFPS.sln` in your editor, or use VS Code's default build task.
For a solution build without the game: `dotnet build SimplyMoreFPS.sln -p:UseRimWorldReferenceAssemblies=true`.

To package a local build, run `python tools/package-release.py --mod dist/SimplyMoreFPS --runtimes win-x64 --output dist/release`.
Use `linux-x64` or `osx-universal` for those platforms, and a fresh output folder each time.

## Changes

`dev` is for development. `main` is the release branch.

- Open normal PRs against `dev`. Say what changed and how you checked it.
- Move a release from `dev` to `main` with a PR after **Build checks** passes.
- Use a merge commit for `dev -> main`, then fast-forward `dev` to `main`.
- Keep both branches. Do not force-push them.

Use the version as the release title, short bullets for changes, and a final restart
reminder. Keep the wording consistent on GitHub and Workshop. Use Markdown on GitHub;
on Workshop, use a bold version label and plain hyphen bullets so the text stays
inside the change-note box.

Run checks relevant to the change. `dotnet run --project tests/Smf.Tests` runs the managed tests.
Native build scripts accept `--tests`. CI builds Windows, Linux, Intel Mac and ARM Mac and
checks the release ZIP contents. Game tests use a fresh Core colony with Harmony and SMF;
other mods belong in separate compatibility runs.

Measure TPS under saturated load, with TPS boost off. Profiler percentages cover the
functions being timed; they are not the mod's total TPS cost.

## Architecture

The camera must keep moving while the game thread is blocked. Keep these boundaries intact:

- Camera smoothing must not change tick scheduling, time accumulation or simulation speed.
- Only Unity's main thread may touch Unity or RimWorld state. The native worker never calls Unity.
- Pass no managed references across the camera kernel's C ABI in `src/Smf.Camera/camera_kernel.h`.
  Keep its struct-size assertions and the public modder API's existing type and member names.
- Never unload a native library while the game is running.
- Build each platform's native renderer separately: D3D11 on Windows, OpenGL on Linux and
  Metal on macOS. A managed build alone does not qualify a platform.
- Keep the opt-in development tools in `src/Smf.Dev` out of release archives.
