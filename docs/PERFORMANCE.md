# Performance work, October 2026

The local app now combines ARM64 CPU code with Metal rendering through ANGLE.
A 120 FPS launcher option sets a ceiling; it does not promise that all scenes
can reach it. The initial Metal build's roughly 48 FPS combat result was limited
by CPU synchronization. Access tracking now brings the same bounded combat
workload close to 120 FPS. Short drops remain; this is not a locked 120 FPS build.

## Presentation pacing and combat frame-time tail (second pass)

Earlier figures measured when `SDL_GL_SwapWindow` returned. They did not show
when frames reached the display. A new benchmark probe (`--present-trace`,
`tests/present_probe.m`) also records time blocked in `-[CAMetalLayer
nextDrawable]`, each drawable's actual `presentedTime` from Metal and each
command buffer's GPU interval. All figures below come from visible fullscreen
runs of the scripted `c2a2d` mounted-weapon combat scene: 3,600 frames,
3456 × 2234 or 3456 × 2168 physical pixels, CoreAudio, normal elapsed
simulation time, M1 Pro on AC power. They are bounded scripted runs, not a
physical-input playthrough.

What the probe showed with the previous build:

- **VSync off** (the configuration in the user's last play log) ran at about
  166 FPS. Frames reached the panel at uneven 5–6 ms intervals with tearing,
  and about 4% of frames were never displayed.
- **VSync on** gave a steady cadence: 99% of frames held exactly one
  8.33 ms refresh. However, the swap chain let the game run two frames ahead,
  so swap-to-glass was 20.1 ms p50 and 29.0 ms p95.
- The GPU was not the limit (about 1.1 ms busy per frame at full resolution).
  The 116–120 FPS ceiling seen in earlier "uncapped" hidden runs came from
  presentation, not from GPU throughput.
- Even with no frame queued, a presented frame takes about 10 ms from swap to
  glass: GPU work plus about one refresh of compositing. Waiting for each
  frame's own presentation therefore halves the rate. One queued frame is the
  minimum for 120 FPS.

Changes:

- **Fullscreen around the notch** (launcher option, on by default). A macOS
  fullscreen Space always keeps the picture below the camera housing, so the
  window covers 3456 × 2168 of the 3456 × 2234 panel. SDL's non-Space
  fullscreen covers the whole display. This alone lowered swap-to-glass from
  20.1/29.0/29.2 ms to 13.5/21.1/25.2 ms (p50/p95/p99). `gamepolicyd` still
  logged "Game mode status is now on". The camera hides a small strip at the
  top centre of the picture.
- **Closed-loop VSync pacing** (`host_vsync_pacing`, enabled by the launcher
  only for fullscreen around the notch with VSync). The engine counts drawables
  acquired and presented through Metal's presentation callbacks. Before each
  frame samples input, it waits until at most `host_vsync_queue` (default 1)
  finished frames await the display. A frame limit at or above the measured
  refresh rate is then left to the pacer, because its timer otherwise drifts
  against the display. A first open-loop design based on CVDisplayLink
  predictions was measured and rejected: presentation is FIFO, so one late
  frame delayed later ones and its schedule drifted up to 44 ms ahead.
- **Server bridge scopes.** Entity class factories called by the engine now
  enter the same demand-access scope as other game entry points. Previously,
  each spawned entity (for example a gib) synchronized the whole world at every
  engine call its spawn code made. A borrowed-pointer fallback inside a game
  callback no longer leaves the rest of that callback on full synchronization:
  after the next full pull, the bridge publishes native writes made through the
  borrowed pointer and resumes demand access. AI local-move checks call
  `WalkMove` once per step, which previously meant 745 whole-world scans in a
  2,400-frame run. `pfnFindEntityInSphere` publishes only touched records'
  bounds inside an access scope.
- **Import and studio boundaries.** Calls into the other translated modules
  (VGUI, FMOD), allocator/TLS/last-error services and scalar
  `glEnable`/`glDisable`/`glFog*` no longer materialize all entities. Time
  queries touch only their output. Precache calls no longer synchronize
  entities. The renderer and the original client now share one copy of the
  studio bone and light matrices (`Xash_StudioSetTransformStorageV1`). That
  removes four 6 KiB copies around each of about 650 studio calls per frame.

Engine-thread CPU time per frame in the same combat scene (VSync off,
uncapped, three runs for the final build):

| | Before | After |
|---|---:|---:|
| p50 | 5.52 ms | 4.51–4.67 ms |
| p95 | 8.38 ms | 6.73–6.90 ms |
| p99 | 10.2 ms | 8.23–8.38 ms |
| max | 39.2 ms | 15.5–21.1 ms |

Presentation with the launcher defaults (fullscreen around the notch, VSync,
120 FPS limit), two runs:

| | VSync before (native Space) | After |
|---|---:|---:|
| Frames held exactly one refresh | 99.3% | 99.0–99.2% |
| Swap-to-glass p50 / p95 | 20.1 / 29.0 ms | 11.4 / 12.4–12.5 ms |
| Swap-to-glass p99 | 29.2 ms | 18.0–18.6 ms |

Input is sampled at the start of a frame, so input-to-glass is roughly the
frame's CPU time plus swap-to-glass: about 16 ms now, versus about 28 ms with
VSync before. The remaining two-refresh frames (about 1%) are CPU frames over
roughly 7 ms. Further CPU work reduces them in any presentation mode.

Validation on the final build: boundary fixtures (including new sphere-scope
and demand re-arm cases), startup differentials, 17 render contracts on both
renderers, client save/load, SDL input, the 12-batch weapon soak, all 134
maps, all ten packaged stages, and 6,000-frame combat runs with
`--verify-entities` (no untracked writes). The packaged movement stage's
frame-counted waits only passed at about 10 FPS. They now allow the jump to
land at 120 FPS, and the stage passes on both the new and the previous build.

Remaining limits of this pass:

- About 1% of combat frames still take two refreshes when their CPU time
  exceeds roughly 7 ms. Remaining CPU targets are client boundaries (per-call
  studio entity sync, event API inputs such as `EV_PlayerTrace`, fallbacks
  after `GetCurrentEntity`), per-return server cvar synchronization and ANGLE
  (about 15% of the engine thread).
- Pacing applies only to fullscreen around the notch. Windowed VSync and a
  fullscreen Space keep the normal swap chain, where pacing measured worse.
  A 60 FPS limit with VSync alternates between one and three refreshes on a
  120 Hz display; swap interval 2 would be the proper fix.
- Not verified by these tests: Cmd-Tab, Command-Escape Game Overlay and the
  in-game fullscreen toggle in fullscreen around the notch. Physical-input feel
  in real play.
- One 15 ms frame in 6,000 was spent inside SDL event pumping
  (`SDL_WaitEventTimeout`); its cause is unexplained.

Diagnostics added for this work, all off by default: `BHL_SYNC_STATS=1`
(full-synchronization causes by boundary), `BHL_IMPORT_STATS=1`,
`BHL_MOUSE_TRACE=1` (cursor, warp and relative-motion calls) and
`BHL_PACING_TRACE=1`. `tools/benchmark_app.py` gained `--present-trace`,
`--sample` (in-process engine-thread sampler; `tools/analyze_samples.py`
summarizes it), `--no-spaces`, `--mouse`, `--cvar` and `--env`.

## Updated Metal measurements

The October 13 comparison uses the same M1 Pro, Metal renderer, scene scripts,
2560 × 1440 physical render size, dummy audio, fixed simulation step, VSync off
and 1,000 FPS ceiling. The baseline is `cb0ac3d`, before access tracking.
Short runs measure 1,199 real presentation intervals from 1,200 frames.

| Workload | Previous FPS | Updated FPS | Previous 1% low | Updated 1% low |
|---|---:|---:|---:|---:|
| Campaign hub (`start`) | 92.22 | 118.63 | 71.21 | 97.31 |
| Mounted-weapon combat (`c2a2d`, assault rifle), run A | 47.50 | 116.33 | 28.26 | 77.71 |
| Same combat, run B | 47.50 | 116.42 | 28.26 | 79.33 |
| Early campaign (`c1a0`) | 116.19 | 117.86 | 57.72 | 73.73 |

A longer 12,000-frame combat run averaged 118.19 FPS with a 68.73 FPS 1% low.
This extends the same scripted scene; it does not represent sustained firing
for the entire run or a full campaign playthrough. A separate 6,000-frame run
with entity-access auditing found no untracked writes. Audit timings are
excluded from performance claims. The full-scan comparison control on the
updated runtime averaged 57.76 FPS, confirming the cost of repeated scans.

A separate visible fullscreen run used normal elapsed simulation time,
CoreAudio output, the 120 FPS limit and VSync, at 3456 × 2168 physical pixels.
Across 3,600 frames it averaged **118.75 FPS**, with a **69.02 FPS 1% low**.
A repeat from the installed bundle averaged **118.48 FPS**, with a
**59.84 FPS 1% low**. The repeat's raw report is
`work/reports/combat120-installed-fullscreen.json`; the slower tail varies
between runs even though average throughput stays close to 120 FPS.
Both engine and original FMOD output verified CoreAudio, and `gamepolicyd`
confirmed Game Mode activation for the game process. This is near the target
on average, but the slower frames prevent a locked 120 FPS result.
Its reports are `combat120-final-fullscreen.json` and
`combat120-final-fullscreen-gamemode.json` under `work/reports/`.

Reports are `work/reports/combat120-final-{combat-a,combat-b,hub,early}.json`,
`combat120-soak.json`, `combat120-soak-audit.json` and
`combat120-eager-control.json`. They record exact loaded library hashes.
The first attempted long run did not execute because its test script exceeded
Xash's 32 KiB command buffer. The harness now uses Xash's counted `wait` command
for more than 4,096 frames; original short-run scripts remain unchanged.

The corrected build passed all 134 map activation/capture checks, twelve
sustained weapon batches requesting all 36 exported classes, and SDL input
checks with access auditing enabled. All nine packaged-launcher stages passed,
including fresh-process save/reload, a landmark transition, combat, fourteen
level loads in one process, and fullscreen/window resizing. Runtime service,
audio, worker cancellation and memory-barrier checks passed; all 990 startup
comparison boundaries still match. Native boundary fixtures additionally cover
cached pointers, cross-page and bulk accesses, pending writes, overlapping
entity pools, original-created entities and retired temporary effects.
These are bounded checks, not full campaign or physical-input acceptance.
The local evidence index is `work/reports/combat120-release-verification.json`.

## Combat CPU synchronization changes

The October 13 follow-up replaces repeated whole-world mirror scans with
materialization at the first translated memory access in a callback. Cached
entity pointers remain valid: scalar, stack, x87, SSE, atomic and bulk memory
helpers observe accesses before executing the original read or write. The
barrier covers all records that overlap a touched page, including unaligned
accesses spanning two pages. Audio workers have independent thread state.

Both client entities and server edicts keep stable guest addresses. Native
boundaries publish changed records and start a fresh generation after the
native callback. Bounded, synchronous vector inputs synchronize their exact
range. Unbounded pointer escapes and native CRT/OS imports restore complete
synchronization, retaining pending original writes. Pure scalar CRT math stays
inside the access scope because it cannot borrow entity pointers. Client entities
created by original code publish their initial guest contents before the first
native read. Temporary-effect headers share the access barrier with their entity
bodies; final active-list membership determines which bodies are published at
callback completion, including newly allocated effects that were never drawn.
Retired bodies discard their comparison snapshots: edits made while unlinking
an effect cannot leak into a later reuse or appear as writes in a later scope.
The sustained weapon audit exposed this case, and a regression reproduces the
failure before the fix and verifies fresh native contents after slot reuse.

Additional reductions preserve original gameplay and renderer settings:

- Hash guest entity identities and avoid repeatedly walking every temporary
  entity for each visible submission.
- Validate shared model descriptors and server string IDs once per synchronous
  scan, with fresh validation on the next native boundary.
- Read diagnostic environment options once at library load. Keep cancellable
  worker checkpoints and import observers while shortening normal engine-thread
  callback dispatch.
- Use sorted export lookup and avoid saving native signal masks around explicit
  translated fault checkpoints. Floating-point state restoration remains intact.
- Cache absent model names only while `Xash_GetModelLookupRevisionV1` agrees.
  The engine invalidates this revision on model creation, loading, freeing,
  client precache edits and client-state clearing. Engines without the optional
  query continue searching on every miss. Positive lookups still validate the
  live index, pointer, name and level/video epoch.

`BHL_EAGER_ENTITIES=1` selects full mirror scans as a comparison control.
`BHL_VERIFY_ENTITY_ACCESS=1` audits untouched mirrors for untracked writes;
this extra work must not be enabled for performance measurements. The benchmark
exposes these as `--eager-entities` and `--verify-entities` and records them.
Neither option changes effects, enemy counts or simulation frequency.

## Earlier changes

- Compile all four translated modules at `-O2` with ThinLTO, retaining
  `-ffp-contract=off` and `-fno-strict-aliasing`. The runtime builder's
  `--no-lto` option disables link-time optimization for comparison builds.
- Narrow the pinned original `HUD_AddEntity` boundary for nonspectator calls,
  including its custom sky-model behavior. Spectator paths retain full state
  synchronization. Temporary-entity visibility uses the same scoped path.
- Scope the original base collision-box callback to its one entity. Unknown
  subclass methods retain full synchronization.
- Publish pending message state at MessageEnd. MakeVectors and GetModelPtr
  transfer only the fields the pinned native functions consume.
- Cache successful model-name lookups with index, pointer, name and epoch
  validation. The later revision-aware cache described above also caches
  missing models when the engine supplies explicit invalidation evidence.
- Skip unchanged temporary-entity and particle prefixes. The native engine
  exports particle pool allocation identities, including tracers and beam
  trails. Free slots are not walked or written, and recycled slots receive
  fresh state before any guest writes can be applied.
- Avoid full effect synchronization for studio sequences without client
  events. Unknown headers and events numbered 5000 or above retain the full
  reentrant boundary. Renderer flag setters preserve pending entity writes.
- Offer 120 FPS, 60 FPS and uncapped limits plus VSync in the launcher. The
  engine now honors the selected single-player limit with VSync enabled.

These changes do not reduce effect counts, replace the original gameplay
functions with stubs, or change the translated instructions. Boundary fixtures
compare original callbacks through the full and narrowed adapters and exercise
pending writes, allocation reuse, model reset and nested visibility callbacks.

## Measurement method

`tools/benchmark_app.py` launches a hidden Metal window by default, or OpenGL
with `--renderer gl`, with dummy audio and
isolated data under `work/`. The game still renders every measured frame.
It hides the loading console, disables developer overlays, warms up the map
and runs 1,200 frames with a fixed 1/120-second simulation
step. A test-only SDL swap interposer records real elapsed presentation
intervals; `host_framerate` does not supply the measured FPS. Library paths
and hashes are checked so a comparison cannot accidentally load a different
adapter through `DYLD_LIBRARY_PATH`.

The uncapped comparison uses `fps_max 1000`, VSync off and no bridge profiling.
The reported 1% low is 1000 divided by the mean of the slowest 1% of frame
intervals. Runs use the same map and command sequence, but engine randomness
and OS scheduling can still vary. These are capacity tests for bounded scenes,
not a complete playthrough or a guarantee of visible fullscreen performance.

Example after packaging an isolated app:

```sh
.venv/bin/python tools/benchmark_app.py \
  --app 'work/package/Brutal Half-Life.app' --name hub --map start
.venv/bin/python tools/benchmark_app.py \
  --app 'work/package/Brutal Half-Life.app' --name combat --map c2a2d --attack
```

The combat case enables invulnerability and requests the assault rifle while
the map's enemies and mounted weapons remain active. `--width` and `--height`
are logical window dimensions; inspect `drawable_size` in the report for the
actual Retina render size. `--fps-cap 60 --vsync` checks the combined controls.
`--capture` saves the final scene after timing stops.
`--audio coreaudio` enables real device output for a visible acceptance run;
the default remains dummy audio for comparisons. `--real-time` selects normal
elapsed simulation time rather than the fixed step used in matched comparisons.
`--client`, `--server`, `--core` and `--engine` allow explicit library comparisons.
Reports and raw intervals stay in ignored `work/reports/`.

Optional `--bridge-profile` logs inclusive adapter timings at shutdown.
Nested totals overlap and profiling changes the workload; do not use those
runs as the primary before/after comparison.

## Earlier CPU optimization results (OpenGL)

Measured on an M1 Pro with 10 CPU cores and 32 GiB RAM, connected to AC power,
running macOS 27.2 (26B5101f). The 1280 × 720 logical window rendered at
2560 × 1440 Retina pixels. The baseline is the previously installed
`0a512a1` build. Each row uses 1,199 measured intervals from 1,200 frames.

| Workload | Previous FPS | Updated FPS | Previous 1% low | Updated 1% low |
|---|---:|---:|---:|---:|
| Campaign hub (`start`) | 55.17 | 89.01 | 43.18 | 61.69 |
| Mounted-weapon combat and assault rifle (`c2a2d`) | 11.54 | 43.78 | 6.91 | 23.32 |

The updated early campaign scene (`c1a0`) averaged 173.19 FPS uncapped; the
weapon showcase (`_zoo`) averaged 59.08 FPS.
This does not establish sustained 120 FPS: combat remains well below that
target. Reducing combat to 1280 × 720 physical pixels produced 44.29 FPS,
only about 1% faster despite rendering one quarter as many pixels. Together
with the adapter profiles, this points to a remaining CPU bottleneck.
The hub's 60 FPS limit with VSync enabled measured 59.80 FPS. With the 120 FPS
limit and VSync, `c1a0` averaged 110.08 FPS with a median interval of 8.333 ms;
slower frames still prevented a steady 120 FPS.

Raw reports are `work/reports/overnight-release-*.json`; the comparison index
is `work/reports/overnight-performance.json`. They include actual library
hashes, render dimensions, sample counts and failure checks. These figures
come from one matched run per workload; preliminary repeated combat runs
were in the 42–46 FPS range. Do not interpret the decimal precision as a
guaranteed result on another run or machine.

The same runtime passed the service/interface/boundary suites, all 990 startup
comparison boundaries with zero differences, CoreAudio output, all 134 map
activation checks, twelve sustained weapon batches requesting all 36 exported
classes, synthetic SDL input, and all eight packaged-app acceptance stages.
The latter include fresh-process saves, a landmark transition, mounted-weapon
combat and fourteen level loads in one process. These remain bounded checks;
full campaign, original-release visual/audio comparison and physical input
acceptance are separate work.

## Metal implementation

`ref_metal` uses Xash's GLES fixed-function shader adapter, SDL's EGL window
support and the [ANGLE Metal backend](https://github.com/google/angle).
The build fetches standalone ARM64 libraries from a pinned official Electron
release; it does not run Electron or depend on another installed application.
`tools/fetch_angle.py` verifies the archive checksum before extraction.
The app bundles the libraries and license notices with relative loader paths.

The engine selects Metal explicitly and validates both the renderer name and
the non-null Metal device returned by `EGL_ANGLE_device_metal`. EGL entry points
are resolved strictly through ANGLE. SDL's general symbol lookup could otherwise
find desktop GL functions through ANGLE's OpenGL framework dependency, mixing
two graphics contexts and crashing. A request for Metal rejects a desktop GL
fallback rather than silently changing the backend.

Client fog imports use the same shader state as the native world renderer.
The adapter now implements all three fog equations with eye-space depth,
texture environment modes and texture matrices. Seventeen GPU pixel-readback
contracts compare expected results with both OpenGL and Metal, including
lightmaps, indexed particle/beam batches, alpha cutouts, translucent/additive
effects and depth ordering:

```sh
.venv/bin/python tests/test_render_contracts.py
```

These are targeted rendering contracts, not proof of whole-game pixel identity.
The Metal path retains the original simulation, studio callbacks and effect
counts. It is a Metal-backed compatibility renderer, not a direct Metal rewrite.

## Initial Metal comparison results (before access tracking)

Matched runs on the same M1 Pro, at 2560 × 1440 physical pixels, with 1,199
presentation intervals per run and the original effects enabled. These are
windowed hidden capacity tests with Game Mode inactive, VSync off and a 1,000
FPS ceiling. The presentation probe verified an actual SDL swap interval of 0.

| Workload | OpenGL FPS | Metal FPS | OpenGL 1% low | Metal 1% low |
|---|---:|---:|---:|---:|
| Campaign hub (`start`) | 86.78 | 92.22 | 62.47 | 71.21 |
| Mounted-weapon combat (`c2a2d`, assault rifle) | 45.30 | 47.50 | 25.64 | 28.26 |
| Early campaign (`c1a0`) | 173.63 | 116.19 | 49.20 | 57.72 |

Metal modestly improves the hub and combat here, but reduces uncapped throughput
in the early campaign scene. It is not a universal performance improvement and
does not deliver sustained 120 FPS. Two early-scene Metal runs both averaged
about 116 FPS despite the API reporting swap interval 0; this observation alone
does not establish the cause of the lower throughput. Further work must measure
CPU adapter costs, rendering and presentation separately.

The final comparison reports are `work/reports/metal-final-{gl,metal}-{hub,combat,early}.json`.
They record the library hashes, actual Metal device check, real presentation
intervals and selected/effective swap interval. The same Metal renderer passed
all 134 initial map checks, 12 weapon batches requesting all 36 exported weapon
classes, 17 rendering contracts, input/save/reload checks and packaged acceptance.

A visible native fullscreen run of `c1a0` with the 120 FPS limit and VSync on
averaged **116.44 FPS**, with a 46.18 FPS 1% low, at 3456 × 2168 physical pixels.
The API reported swap interval 1 and macOS logged Game Mode activation for the
window-owning process. The median frame interval was 8.351 ms; occasional slower
frames still prevented a steady 120 FPS. This remains one bounded early scene,
with dummy audio, and does not represent heavy combat or a full playthrough.
Its report is `work/reports/metal-final-fullscreen-120.json`.

```sh
.venv/bin/python tools/benchmark_app.py --app 'work/package/Brutal Half-Life.app' \
  --renderer metal --map c1a0 --frames 1200 --fps-cap 120 --vsync --fullscreen \
  --name fullscreen-120
```

## macOS fullscreen and Game Mode

The bundle declares `public.app-category.games`, `GCSupportsGameMode` and
`LSSupportsGameMode`. The launcher starts another instance of its own executable
in engine mode; that instance loads the native engine directly. The game window
therefore belongs to an executable with the app's metadata. A bare command-line
child does not inherit the launcher's Game Mode eligibility.

The Fullscreen option uses SDL's macOS fullscreen Spaces path, and requests
from the in-game fullscreen menu are normalized to the same path. The window
resolution selector applies to windowed play; native fullscreen uses the desktop
resolution. Saves, game settings and the macOS Game Mode preference are preserved.

Visible acceptance checks cover entering fullscreen at launch, switching via
the in-game command and returning to a resized window. AppKit confirms entry
into native fullscreen. On the test Mac, `gamepolicyd` identified the game process
from `LSSupportsGameMode` and logged both `Game mode enabled` and
`Game mode status is now on`, then paused Game Mode on leaving fullscreen.
Evidence is in `work/reports/metal-gamemode.json` and its associated system log.

Game Mode remains controlled by macOS. Per [Apple's documentation](https://support.apple.com/en-mide/105118),
windowed play does not activate it, and a manual off choice persists until the
user turns it back on in Game Overlay (Command-Escape on macOS 26 or later).

```sh
.venv/bin/python tests/test_packaged_app.py --stages campaign --visible --fullscreen
.venv/bin/python tests/test_packaged_app.py --stages display --visible
```

## Other renderer investigation

The inspected local They Hunger installation also uses Xash's OpenGL renderer;
it does not provide a Metal backend to reuse. Apple's OpenGL version string
contains "Metal", but the application's API remains OpenGL.

The separate [Xash Vulkan experiment](https://github.com/w23/xash3d-fwgs/tree/vulkan)
documents missing decals/visibility culling and lower rasterization performance
than OpenGL. [MoltenVK](https://github.com/KhronosGroup/MoltenVK) can map Vulkan
to Metal, but it cannot supply those missing renderer features. Neither was
integrated as a substitute for the working renderer.

Profiles identify repeated CPU adapter synchronization and translated studio
animation as remaining expensive work. Changing the graphics API does not
remove that CPU work.
