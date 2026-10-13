# Brutal Half-Life v3 native recompilation

Local development of an Apple Silicon port using the original v3 modules.
**This is not a playable app yet.** The server loads the intro map and runs
frames. The client initializes the original HUD, VGUI and FMOD, then begins
joining the local server. Model conversion still blocks a player session.

Original x86 instructions are translated to C and compiled to ARM64. The
runtime uses a 32-bit guest address space and explicit adapters to native
Xash3D. Wine and Rosetta are not used. Unicorn executes original instructions
only for development comparisons.

## Reproduce the development build

Requirements: Apple Silicon, Xcode Command Line Tools, Python 3, Git,
`pkg-config`, SDL2 development files and UPX. For Homebrew dependencies:

```sh
brew install pkg-config sdl2 upx
./bootstrap.sh /path/to/bhl_v3.zip
.venv/bin/python tools/build_project.py \
  --archive /path/to/bhl_v3.zip \
  --valve /path/to/owned/Half-Life/valve \
  --check
```

Bootstrap creates the pinned Python environment and runs the small interface
proof. The project builder verifies the archive, fetches pinned engine/SDK
dependencies, unpacks FMOD, relocates and translates all four modules, builds
both bridges and stages an isolated session. First-time translation takes
longer than incremental builds.

All input copies, third-party checkouts, generated game code, binaries and
reports stay in ignored `work/`. Source game folders and saves are unchanged.
A build does not install or publish an application.

## Verified progress

| Area | Evidence |
|---|---|
| Translation | 10,387 discovered functions and 1,009 imports link as ARM64; untested paths can still trap |
| Server interface | Four exports pass 1,000 deterministic x86/ARM64 comparisons |
| DLL startup | Server, client, FMOD and SDK VGUI complete process attach |
| Startup comparisons | 990 API boundaries match integer registers and writable memory |
| Native services | Tests cover codepages, heap ownership, recursive locks, file I/O and explicit missing-call failures |
| Server integration | GameInit, movement initialization and three delta encoders run through native callbacks |
| Headless map | `c0a0` activates and completes at least ten server frames in a bounded session |
| Client integration | Initialize, weapon-event registration and movement initialization pass in the full engine with a null renderer |
| Audio | Original FMOD initializes, runs native worker threads and shuts down through a native PCM adapter; sample mixing tests pass |
| VGUI | Original App, Label and Surface code produces text textures through CoreText, paints through the engine API and passes surface restart checks |
| Complete conversion | Pending: audible/visual comparison, model and rendering integration, player movement, save/load and campaign QA |

Startup comparisons share the native OS-service model. They validate the
translation, **not Windows API equivalence**. Null rendering and dummy audio
cannot validate graphics or audible output. Adapter counts are not gameplay
coverage.

Reproduce the current client failure after building:

```sh
.venv/bin/python tests/test_client_engine.py --trace
```

This acceptance test currently fails at client model/BSP conversion. Its report is
`work/reports/client-engine.json`. Faults must be resolved before claiming
client integration is complete.

See [port status and acceptance criteria](docs/PORT.md) and
[dependency provenance](NOTICE.txt). Original assets and generated game code
must never be committed.
