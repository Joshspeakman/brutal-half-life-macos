# Brutal Half-Life v3 native recompilation

Local development of an Apple Silicon port using the original v3 modules.
**The complete conversion is still under validation.** A local Mac app now
launches the original client and server, renders the campaign hub and showcase,
plays decoded audio through CoreAudio, and supports native saves. All 134
shipped maps pass initial activation/simulation checks. Full campaign behavior,
physical mouse/fullscreen use, audible/visual comparison and performance need
further validation before this can be considered finished.

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

To build a local app, use the same owned inputs:

```sh
./install.sh --archive /path/to/bhl_v3.zip --valve /path/to/owned/Half-Life/valve
```

The default app is `~/Applications/Brutal Half-Life.app`; separate game data,
settings, saves and `Last Run.log` are under
`~/Library/Application Support/Brutal Half-Life`. `--app` and `--data-dir`
override these locations. Repeating the command replaces only this project's
app and preserves its existing user data. `--check` runs the development checks.
The bundle carries its native runtime libraries, prepared image data, icon and
menu artwork. It does not require Homebrew at launch. It is a local build from
owned inputs, not an artifact to distribute.

The launcher offers the game menu, campaign hub, weapon showcase, and latest
save, plus resolution, fullscreen and difficulty. The Controls button explains
v3's bindings; detailed input/audio/video settings remain in the game menu.

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
| Audio | Original FMOD decodes WAV and game MP3 data to nonzero PCM; pause/resume, seek, volume, pan and shutdown checks pass on dummy audio and the native CoreAudio device |
| VGUI | Original App, Label and Surface code produces text textures through CoreText, paints through the engine API and passes surface restart checks |
| Saving | Original serializer handles fields, entities, time and landmarks; packaged tests cover fresh-process reload and the first tram landmark transition |
| Combat smoke tests | All 36 exported weapon classes have been requested across scripted primary/secondary/reload and save/load batches; equipping variants and hit behavior need gameplay comparison |
| Map coverage | All 134 shipped maps pass activation, 35 initial frames, capture and normal shutdown; fourteen repeated loads plus save/reload pass in one process |
| Input | SDL queue events exercise original walking, jumping, crouching, menu return, F5 quicksave and F6 quickload; physical input remains unverified |
| Mac packaging | Eight packaged-launcher checks pass: menu, campaign hub, save, fresh-process reload, landmark transition, mounted-weapon combat, movement and repeated level loads; bundled libraries load without Homebrew dependencies |
| Complete conversion | Pending: full campaign/script playthrough, physical input and audible/visual comparison, remaining runtime edge cases and performance |

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

`tests/test_map_matrix.py` exercises all shipped BHL maps for activation,
initial simulation, capture and normal shutdown. `--resume` reuses passes only
for the same staged binaries. `tests/test_weapon_matrix.py` checks scripted
weapon batches; `--soak --seconds-per-weapon 100` allows complete draw, fire
and reload intervals. Neither constitutes a full playthrough.

`tests/test_packaged_app.py` validates a bundle with data under ignored `work/`.
`tests/test_input.py --session work/session` posts events only to that test
process's SDL queue, including the original quicksave/load bindings.
`tests/test_fmod.py --audio-driver coreaudio` verifies device-frame advancement
and nonzero decoded PCM at reduced test volume. It does not measure what a
listener hears. Tests accept separate sessions under `work/` to keep configs,
saves and report files isolated.

See [port status and acceptance criteria](docs/PORT.md) and
[dependency provenance](NOTICE.txt). Original assets and generated game code
must never be committed.
