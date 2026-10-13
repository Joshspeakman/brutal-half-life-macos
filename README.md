# Brutal Half-Life v3 native recompilation

Local development of an Apple Silicon port using the original v3 modules.
**This is not a finished playable app yet.** The original client connects to
the native engine, renders the intro and showcase maps, and completes a
showcase save/load/quit test. Combat, campaign behavior, performance and the
Mac launcher still need work.

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
| Native services | Tests cover codepages, heap ownership, locks, Unicode file I/O, directory searches, time conversion and binary CRT streams |
| Server integration | GameInit, movement initialization and three delta encoders run through native callbacks |
| Headless map | `c0a0` activates and completes at least ten server frames in a bounded session |
| Client integration | Original movement, studio animation, entities and temporary effects cross the native boundary; OpenGL produces a coherent showcase scene and view model |
| Audio | Original FMOD decodes synthetic WAV and owned game MP3 data to nonzero PCM on the SDL dummy device; five mixer/locking tests pass |
| VGUI | Original App, Label and Surface code produces text textures through CoreText, paints through the engine API and passes surface restart checks |
| Saving | Original field serializer round-trips strings, entities, time and landmarks; the showcase saves, reloads, runs again and exits cleanly |
| Combat smoke test | A scripted pistol/shotgun/rocket session reaches original firing events, requests map music, saves, reloads and quits; this does not validate every weapon or enemy |
| Complete conversion | Pending: combat, remaining effects, audible/visual comparison, campaign transitions, performance and app packaging |

Startup comparisons share the native OS-service model. They validate the
translation, **not Windows API equivalence**. Null rendering and dummy audio
cannot validate graphics or audible output. Adapter counts are not gameplay
coverage.

Run the bounded client save/load test after building:

```sh
.venv/bin/python tests/test_client_engine.py \
  --renderer gl --video-driver cocoa --map _zoo \
  --save-load --capture-frames 25 --seconds 90
```

This uses a hidden native window and dummy audio. Its report is
`work/reports/client-engine.json`; success is a bounded integration result,
not complete gameplay coverage. `--capture` saves an engine-only PNG in the
ignored session. The default null renderer cannot exercise v3's studio/GL
paths. `tools/build_project.py --check-client` runs the OpenGL save/load test.

Native saves preserve v3 callbacks that lack exported names using identifiers
tied to the exact server binary. Windows save compatibility is not established.

See [port status and acceptance criteria](docs/PORT.md) and
[dependency provenance](NOTICE.txt). Original assets and generated game code
must never be committed.
