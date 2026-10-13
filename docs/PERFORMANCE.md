# Performance work, October 2026

The local build remains ARM64 CPU code with Xash's OpenGL renderer. There is
no native Metal renderer in this project. A 120 FPS launcher option sets a
ceiling; it does not promise that all scenes can reach it.

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

`tools/benchmark_app.py` launches a hidden OpenGL window with dummy audio and
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

## Measured results

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

## Metal investigation

The inspected local They Hunger installation also uses Xash's OpenGL renderer;
it does not provide a Metal backend to reuse. Apple's OpenGL version string
contains "Metal", but the application's API remains OpenGL.

The separate [Xash Vulkan experiment](https://github.com/w23/xash3d-fwgs/tree/vulkan)
documents missing decals/visibility culling and lower rasterization performance
than OpenGL. [MoltenVK](https://github.com/KhronosGroup/MoltenVK) can map Vulkan
to Metal, but it cannot supply those missing renderer features. Neither was
integrated as a substitute for the working renderer.

Profiles identify repeated CPU adapter synchronization and translated studio
animation as the current bottleneck. A future Metal renderer also needs to
cover v3's direct GL fog/state imports, original studio callbacks, lightmaps,
decals, temporary entities, particles, UI, resize/fullscreen and captures.
Changing the graphics API alone does not remove the measured CPU work.
