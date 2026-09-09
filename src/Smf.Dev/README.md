# Smf.Dev

Opt-in HTTP API for driving a running RimWorld instance from scripts. It is a separate assembly
that never ships in the release package and is only loaded when asked for.

Enable it: build with `python tools/build.py --dev` (output in `dist/dev/SimplyMoreFPS`,
dev assemblies under `Dev/`), then launch RimWorld with `-smf-devapi=PORT` (1024 to 65535). SmfMod
loads `Dev/Smf.Dev.dll` by reflection and the server listens on `http://127.0.0.1:PORT/`, loopback
only. `-smf-test-map-size=N` (100 to 250) optionally sets the quick-test map size.

Routes:

- `GET /` lists the routes.
- `GET /ping` returns `{ ok, pong, utc }`.
- `GET /status` returns the last main-thread sample: frames, ticks, program state, map, camera, screen and renderer state.
- `GET /logs` returns the last 400 captured Unity log entries and the error count.
- `POST /eval` compiles the raw C# body with Roslyn and runs it synchronously on the main thread; scripts get `Print(x)` and `V[...]`.
- `GET /screenshot` returns an end-of-frame PNG of the game window.
- `POST /stall` with an integer body blocks the main thread for that many milliseconds (1 to 60000).
- `POST /quit` quits the game.

Errors come back as `{ ok: false, error, errorType }` with status 400, 501, 503, 504 or 500 depending on the cause.
