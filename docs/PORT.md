# Native conversion status and acceptance criteria

The target is the original Brutal Half-Life v3 release. The 2017 repository
is reference material, not a substitute for v3. The authorized scope is the
complete local conversion; this development milestone is not completion.

## Inputs and execution

`config/modules.json` pins the three user DLLs and SDK VGUI by SHA256. UPX
unpacks a local FMOD copy with a separately pinned hash. Originals are never
modified.

| Module | Guest base | Imports |
|---|---|---:|
| Server `hl.dll` | `0x10000000` | 71 |
| Client `client.dll` | `0x30000000` | 648 |
| SDK `vgui.dll` | `0x40000000` | 138 |
| Audio `fmod.dll` | `0x50000000` | 152 |

All 549 client VGUI import names exist in the pinned SDK library. This proves
symbol coverage, not GUI behavior. Relocation verifies all HIGHLOW fixups and
import/export identity. A previous PE serialization approach changed import
names; preparation now patches only the original base field and fixups.

The native loader verifies prepared-image hashes and loads PE data sections.
It never executes PE machine code. Generated functions and adapters compile
to ARM64. Unsupported instructions and missing callbacks fail explicitly.
Guest threads execute translated code on native pthreads with separate stacks,
TEBs, TLS/FLS values and DLL thread notifications. Events, waits and recursive
locks support the original FMOD workers. TerminateThread uses cooperative
cancellation at API boundaries, deferred while guest critical sections are
held; it is not Windows asynchronous thread termination. Unsupported thread
behaviors remain explicit failures.

## Tests and their limits

The four-export proof has 1,000 cases covering accepted/rejected versions,
null output tables, unaligned destinations and randomized engine tables.
It compares registers, defined flags, XMM and writable image/stack/scratch
bytes. It does not execute startup.

| Startup | API calls | Before/after boundaries | Differences |
|---|---:|---:|---:|
| Server | 179 | 358 | 0 |
| Client | 253 | 506 | 0 |
| FMOD | 3 | 6 | 0 |
| VGUI | 60 | 120 | 0 |

Startup comparisons cover integer registers, writable active-module sections,
used heap and the top 64 KiB of stack. They share the same native OS services.
They do not prove Windows equivalence. Flags, floating-point state, other
modules and virtual allocations need broader coverage for later execution.

Independent service tests cover all 256 CP1252 round-trips, composition,
best-fit/default replacement, buffer sizing, private heap ownership and
destruction, recursive locks, Unicode file names, binary I/O, sharing and
directory boundaries, directory enumeration, binary CRT streams and time-zone
conversion at daylight-saving boundaries. They also check virtual reserve/commit/decommit,
protection metadata and the legacy CRT's 64-bit float-to-integer conversion.
Protection metadata is not a full hardware MMU: the host mapping does not
enforce every guest protection on direct translated memory accesses. This is
not a full Win32 conformance suite.

The dedicated-engine test checks GameInit, PM_Init and delta registration,
then intro-map activation and at least ten frames. It stops a bounded session
intentionally. Because Xash can catch an abort and exit zero, tests require
stage markers and reject bridge/crash messages as well as inspecting status.

The full engine with a real OpenGL context passes client Initialize,
weapon-event registration, movement initialization, VGUI construction and
HUD_Init, then connects the player. Engine captures show the intro title and
a coherent `_zoo` scene with a view model. The client hull callback leaves its
input bounds untouched; preserving those bounds fixed a fall-through-floor
bug. These captures are not a comparison against the Windows renderer.

The `_zoo` integration test saves, reloads, runs at least ten server frames
again, and exits normally. Save fields are packed with guest offsets; the
original serializer handles the file encoding. A separate fixture checks
strings, entity references, time rebasing, landmark offsets, token lifetimes
and exported/unexported function references. Unexported callbacks use a
binary-digest-qualified RVA identifier in native saves. Compatibility with
Windows saves, quicksave/autosave and campaign transitions remain unverified.

A scripted `_zoo` session gives/selects the pistol, shotgun and rocket
launcher, runs primary/secondary attack and reload commands, requests map
music, then saves, reloads and quits. Original event handlers execute without
an adapter fault in this test. Dynamic lights and tracer particles have mutable
native mirrors; fixtures also cover particle callbacks and beam records.
The command script is a smoke test, not evidence that every input, animation,
hit result, weapon variant or enemy behaves correctly.

Five native PCM tests cover 8/16-bit samples, mono/stereo, looping, volume,
pan, playback rate, wrapping locks, stopped output and publication only after
the matching Unlock. Native audio reads a shadow buffer rather than racing
guest Lock writes. Four thread tests cover
separate TEB/TLS state, suspended startup, event reset modes and cooperative
termination. The original FMOD passes initialization at 44.1 kHz, worker
execution, device-frame advancement and shutdown. It also decodes a synthetic
WAV and an owned game MP3 to nonzero PCM. These use SDL's dummy driver;
audible output, pause/resume and broader playback control remain unverified.

The VGUI fixture executes the original App/Label/SurfaceBase constructors,
font measurement and glyph extraction, then records native drawing calls.
It checks nonempty glyph alpha, drawing bounds and two surface lifecycles.
The font adapter maps the GDI memory-DC subset to CoreText and 32-bit DIBs.
This does not establish Windows-identical font metrics or visual fidelity.
Interactive menus and visual font fidelity remain unverified.

## Engine boundary

The server uses 159 engine entries, 50 game entries and five new-game entries.
Guest globals/entvars/edicts are 172/676/804 bytes; native layouts differ.
Entity pointers, string IDs, cvars, traces, private data and delta encoders
are explicitly marshaled. Viewmodel/weaponmodel fields are string IDs despite
being declared as integers. String conversion caches validate native pool
reuse, and global trace writes return to the engine. Many callbacks still
synchronize broad entity state and require further profiling.

Client Initialize copies 135 DWORDs. Native trampolines represent guest
callbacks. Nested API tables remain guest tables; missing methods fail by
name. Client and server movement callbacks use distinct thunk namespaces.
Models preserve pointer identity and copy studio data. Model descriptors are
cached and invalidated at map boundaries. BSP graph pointers that have no
adapter remain guarded opaque tokens. Temporary entities preserve list heads
across nested allocations, and studio matrices synchronize in both directions.
Fixtures cover entity-array replacement, mutable writes and callback lifetimes.

The engine has a small local patch adding `-hidden` for tests. Session staging
replaces Mach-O files atomically and takes a lock shared with engine tests, so
a running test cannot have its executable overwritten. Each client run starts
from the mod's shipped config (`cl_lw 0`); client weapon prediction with
`cl_lw 1` remains unverified.

SDL imports use native SDL, controller objects use handles, and OpenGL calls
require a real native context. Windows global mouse-acceleration settings
are reported unsupported; the original client handles the failure and uses
SDL input. macOS system mouse preferences are not changed.

The server's `playaudio` command is routed to the original client's `PlayMP3`
message handler. Bare names resolve under `sound/music/` with looping enabled;
explicit paths are preserved. The original FMOD code retains stream ownership.

## Work required for the complete conversion

1. **Audio:** audit overlapping buffer locks, reference lifetimes and remaining
   DirectSound controls. Verify map music routing and actual decoded music,
   effects, loops, pause/resume and shutdown audibly.
2. **GUI:** compare font metrics and appearance, exercise interactive menus,
   test nested clipping/cursor/input behavior and audit guest UI allocations
   across level/restart cycles. Guest C++ objects and vtables remain in guest
   memory; only OS-facing App and engine-owned Surface methods cross to native.
3. **Rendering/client:** finish triangle/effects/event/studio APIs, models,
   entities, view calculation, HUD, particles, weapon animation, fades and
   input prediction. Compare real rendering and combat against original behavior.
4. **Server/player:** finish movement/collision, connections, data packing,
   visibility and remaining callbacks. Audit guest writes to globals and
   invalidate model-copy caches across level transitions.
5. **Saves:** extend the working serializer and showcase reload checks to
   quicksave/load, checkpoints, adjacent levels, fresh process loads and
   repeated save cycles. Compare restored behavior with the original.
6. **Runtime:** finish memory protection/reservation, module lifetime,
   exceptions, data imports, threading, locale edges and file modes reached
   by gameplay. Audit extended CPUID, x87 precision, unsupported instructions
   and dynamic jump targets.
7. **Performance:** profile real frames, then optimize synchronization and
   audit string/file-copy lifetimes. No performance claim is established.
8. **QA/packaging:** exercise every v3 weapon/enemy, campaign scripts and
   transitions. Then create the native Mac launcher and isolated user
   data/saves. Nightly publication follows local acceptance separately.

Two map warnings name `ammo_chaingunclip` and `item_glow`; neither is exported
by the original server. Check original behavior before adding aliases.

`tools/build_project.py` reproduces the development build. Logs and reports
remain in ignored `work/reports/`. No assets, DLLs, generated game code or
saves belong in Git.
