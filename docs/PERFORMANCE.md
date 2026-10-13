# Performance work, October 2026

The local app now combines ARM64 CPU code with Metal rendering through ANGLE.
A 120 FPS launcher option sets a ceiling; it does not promise that all scenes
can reach it. The earlier CPU optimization measurements below used OpenGL.

## Changes

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
  validation. Missing models are never cached.
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

## Metal comparison results

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
