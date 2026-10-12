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
The runtime currently supports one guest thread; audio worker lifetimes and
thread/TLS isolation remain to be implemented.

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
directory boundaries. This is not a full Win32 conformance suite.

The dedicated-engine test checks GameInit, PM_Init and delta registration,
then intro-map activation and at least ten frames. It stops a bounded session
intentionally. Because Xash can catch an abort and exit zero, tests require
stage markers and reject bridge/crash messages as well as inspecting status.

The full engine with a null renderer passes client Initialize, weapon-event
registration and movement initialization. HUD_Init reaches FMOD and stops at
`WINMM.dll!mixerGetNumDevs`. Audio calls are not discarded or replaced with
success. Neither a rendered frame nor audible output has been verified.

## Engine boundary

The server uses 159 engine entries, 50 game entries and five new-game entries.
Guest globals/entvars/edicts are 172/676/804 bytes; native layouts differ.
Entity pointers, string IDs, cvars, traces, private data and delta encoders
are explicitly marshaled. Current entity synchronization copies the whole
entity table for many callbacks and requires profiling.

Client Initialize copies 135 DWORDs. Native trampolines represent guest
callbacks. Nested API tables remain guest tables; missing methods fail by
name. Client and server movement callbacks use distinct thunk namespaces.

SDL imports use native SDL, controller objects use handles, and OpenGL calls
require a real native context. Windows global mouse-acceleration settings
are reported unsupported; the original client handles the failure and uses
SDL input. macOS system mouse preferences are not changed.

## Work required for the complete conversion

1. **Audio:** implement the actual FMOD output path, devices, buffers,
   synchronization, worker lifetimes and errors. The client explicitly
   requests DirectSound; map that backend or adapt it with behavior tests.
   Verify music, effects, loops, pause/resume and shutdown audibly.
2. **GUI:** bridge guest VGUI App/Surface objects to native drawing/input,
   including fonts, cursor, clipping and lifecycle. Guest C++ objects and
   vtables must remain in guest memory.
3. **Rendering/client:** finish triangle/effects/event/studio APIs, models,
   entities, view calculation, HUD, particles, weapon animation, fades and
   input prediction. Test with a real renderer after headless integration.
4. **Server/player:** finish movement/collision, connections, data packing,
   visibility and remaining callbacks. Audit guest writes to globals and
   invalidate model-copy caches across level transitions.
5. **Saves:** marshal save contexts, field descriptions, entities and function
   references. Test fresh saves, quicksave/load, checkpoints and transitions
   against original behavior.
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
