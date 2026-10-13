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
Windows saves and quicksave/autosave remain unverified. The packaged app also
passes a fresh-process showcase reload and a `c0a0` to `c0a0a` transition using
the `c0a0toa` landmark, then saves and reloads the destination map.

Scripted `_zoo` batches request all 36 exported weapon class names, run
primary/secondary attack and reload commands, request map music, then save,
reload and quit. Original event handlers execute without
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
WAV and an owned game MP3 to nonzero PCM. These pass on SDL's dummy driver
and the native CoreAudio device at 44.1 kHz stereo. Pause/resume with stable
paused time, seeking, volume/pan readback and shutdown also pass through
original FMOD. The hardware test advances real device frames at reduced
volume; audible quality has not been judged by a listener.

The VGUI fixture executes the original App/Label/SurfaceBase constructors,
font measurement and glyph extraction, then records native drawing calls.
It checks nonempty glyph alpha, drawing bounds and two surface lifecycles.
It also rejects drawing-offset calls before client screen initialization.
The adapter previously published its initial zero offset during VGUI startup,
before Xash had initialized the HUD screen size. Scaling that offset divided
by zero and left NaNs in the shared 2D transform, making the initial menu
black. Offsets now cross to the renderer only during panel painting. A
packaged menu capture regression produces zero lit pixels with the old
adapter and a visible menu with the corrected adapter; campaign startup and
fresh-process save/load still pass.
The font adapter maps the GDI memory-DC subset to CoreText and 32-bit DIBs.
This does not establish Windows-identical font metrics or visual fidelity.
Interactive menus and visual font fidelity remain unverified.

## Engine boundary

The app now uses Metal through ANGLE and Xash's GLES shader adapter, with the
original client's studio animation code adapted to Xash3D. The launcher
selects `ref_metal`; startup verifies both ANGLE's Metal renderer and its
`EGL_ANGLE_device_metal` device. It does not silently fall back to desktop GL.
The original OpenGL renderer remains a development comparison path. Profiling the
campaign hub identified substantial CPU overhead in entity synchronization
and identity searches. The adapters now compile with optimization level 2
and cache entity/model identity lookups, preserving the existing state
comparisons and writes. Entity pool replacement clears cached host identities;
model epochs still invalidate borrowed data. Boundary tests exercise more
identities than cache slots, pending writes, entity relocation and model reset.
The translated runtime also uses `-O2` and ThinLTO. Further scoped boundaries,
particle allocation tracking and measured results are described in
[performance notes](PERFORMANCE.md). Full campaign performance validation
remains necessary.

The server uses 159 engine entries, 50 game entries and five new-game entries.
Guest globals/entvars/edicts are 172/676/804 bytes; native layouts differ.
Entity pointers, string IDs, cvars, traces, private data and delta encoders
are explicitly marshaled. Viewmodel/weaponmodel fields are string IDs despite
being declared as integers. String conversion caches validate native pool
reuse, and global trace writes return to the engine. Native edict-pool
replacement and growth retain stable guest addresses. Snapshots skip unchanged
records; audited scalar queries avoid full synchronization. Sphere searches
publish only the bounds the pinned engine reads. Profiling identified repeated
whole-world synchronization in the packet loop. The pinned v3 AddToFullPack
only writes its output state and group-mask globals; it now reuses the snapshot
created by SetupVisibility. Other server callbacks and non-query engine calls
invalidate it. A fixture compares full and reused-snapshot results for visible,
culled, hidden, grouped and player entities. The first sustained weapon batch
took 85 seconds versus 140 before this change on the development machine;
concurrent test workloads prevent treating that as a gameplay FPS benchmark.

Client Initialize copies 135 DWORDs. Native trampolines represent guest
callbacks. Nested API tables remain guest tables; missing methods fail by
name. Client and server movement callbacks use distinct thunk namespaces.
Models preserve pointer identity and copy studio data. Model descriptors are
cached and invalidated at map boundaries. BSP graph pointers that have no
adapter remain guarded opaque tokens. Temporary entities preserve list heads
across nested allocations, and studio matrices synchronize in both directions.
Fixtures cover entity-array replacement, mutable writes and callback lifetimes.
Inactive temporary-entity bodies are excluded using the actual free/active
lists, while their links remain synchronized. Allocations during an update
reenter the active set before entity data are published. Studio drawing
synchronizes its current entity, matrices and lights; sequences that may invoke
client events retain the complete boundary for reentrant gameplay. Native
particle allocation identities prevent retired free-list entries from being
traversed and stale guest writes from reaching recycled native slots.

Local engine patches add `-hidden` for tests, expose particle allocation
identities, honor the single-player frame cap with VSync, and implement the
Metal/ANGLE path. Session staging
replaces Mach-O files atomically and takes a lock shared with engine tests, so
a running test cannot have its executable overwritten. Each client run starts
from the mod's shipped config (`cl_lw 0`); client weapon prediction with
`cl_lw 1` remains unverified.

SDL imports use native SDL, controller objects use handles, and original GL
imports require a live graphics context. The client obtains the renderer's
procedure resolver through `HUD_GetRenderInterface`, so its fog calls share
the world renderer's shader state under Metal. The GLES adapter supports
linear, exponential and squared-exponential fog with eye-space distance;
MODULATE, REPLACE, DECAL and ADD texture environments; RGB/RGBA alpha semantics;
and modelview/projection plus two texture matrices. General texture combiners
remain unadvertised, allowing the engine's existing multipass paths to run.
Windows global mouse-acceleration settings
are reported unsupported; the original client handles the failure and uses
SDL input. macOS system mouse preferences are not changed.

The server's `playaudio` command is routed to the original client's `PlayMP3`
message handler. Bare names resolve under `sound/music/` with looping enabled;
explicit paths are preserved. The original FMOD code retains stream ownership.

## v3 compatibility repair

`runtime/v3_lifetime.inc` repairs an expired muzzle-flash sprite reference in
the pinned v3 server. `CFuncTank::Fire` stores the sprite at object offset
`0x22c`; its timed removal leaves that raw reference behind. In `c2a2d`, the
next mounted-weapon Think accessed the recycled allocation as a sprite and
crashed. The native free callback now clears matching references on the four
identified tank classes before releasing the sprite. A regression fixture
checks live/unrelated references and immediate allocation reuse. The packaged
combat test enables invulnerability after signon, runs 200 combat frames,
observes the repair, writes a fresh save and reloads successfully.

This is an explicit compatibility repair, not a claim that the original code
already handled this lifetime. Executable image bytes and translated
instructions remain unchanged.

## Sustained-session memory

Long firing sessions exhausted the original 128 MiB translation heap, and a
nine-map cycle exhausted it while copying a model. The former allocator could
reserve a freed 950 KiB block for a 16-byte request; old level models also
accumulated indefinitely. The heap now splits recycled blocks, coalesces
adjacent free spans, and uses free-size bins plus a live-address index.
Regressions retain 4,096 small allocations between large transient buffers,
verify their contents and verify reclamation in alternating free order.

Model reset releases borrowed BSP/studio copies while preserving descriptor
identity; opaque token slots are reused. Surviving native studio cache data
are remapped before returning their guest handle. Borrowed string queries
reuse equal results instead of creating another permanent copy each frame.
A boundary fixture checks 160 model resets and 10,000 repeated string queries.
Fourteen actual level loads plus save/reload now pass in one process.

## Local app and broad smoke tests

The Swift/AppKit launcher includes campaign/menu/showcase/latest-save modes,
display, frame limit, VSync and difficulty settings, original binding help, save/log shortcuts,
installation locking and fatal-error recovery. Packaging copies owned data
into a separate installation and retains settings/saves on rebuild. Mach-O
dependencies are bundled with loader-relative paths and ad-hoc signatures;
the prepared original images remain data for the ARM64 runtime.

Eight packaged-launcher stages pass: menu startup, campaign hub with capture,
showcase save, fresh-process reload, the first tram landmark transition,
mounted-weapon combat/save/reload, movement and fourteen consecutive level
loads followed by another save/reload.
Loaded-library checks verify bundled SDL2/SDL3 and engine dependencies without
Homebrew runtime paths. These checks must be repeated after runtime changes.

`tests/test_map_matrix.py` records activation, 35 initial frames, capture and
normal shutdown for all 134 shipped BHL maps. Reports are keyed by staged
binary hashes; a rebuilt runtime cannot reuse earlier passes. The initial
sweep passed 57 maps before finding the tank-sprite fault. All 134 maps pass
with the tank, memory and packet fixes. `tests/test_weapon_matrix.py` retains separate
weapon-batch logs. These are smoke checks, not complete campaign/script,
weapon-hit, animation or enemy-behavior validation.

The SDL input driver is a test-only dylib loaded into a hidden engine. It
posts to that process's SDL queue, never macOS's global event stream. Original
bindings produce measured walking/jumping/crouching, menu return, a fresh F5
quicksave and F6 reload. This does not establish physical mouse capture or
fullscreen behavior.

## Remaining acceptance work

The local app is a development build. These are material limits on the current
evidence, not skipped features that can be claimed as verified:

- Complete the campaign and custom-map scripts, including bosses, endings and
  secrets. Initial execution of every map is not a complete playthrough.
- Compare weapon variants, damage/gore, enemy behavior, animations, lighting,
  fonts and audio with the original release. Weapon exports being requested
  does not prove every variant was equipped or every hit behaved identically.
- Exercise physical mouse capture, fullscreen switching, interactive settings,
  controller behavior and audible output. Synthetic SDL events and device
  frame counts cover narrower boundaries.
- Verify Windows save imports and further checkpoint/landmark combinations.
  Native fresh-process saves work; Windows compatibility is not established.
- Continue testing runtime paths reached by gameplay. Exceptions, guest memory
  protection, less common OS/file modes and x87 precision have the explicit
  limitations described above. Unimplemented behavior fails by name.
- Profile representative real gameplay. Synchronization has improved, but no
  minimum frame-rate guarantee has been established.

Nightly publication remains a separate step after local acceptance.

Two map warnings name `ammo_chaingunclip` and `item_glow`; neither is exported
by the original server. Check original behavior before adding aliases.

`tools/build_project.py` reproduces the development build. Logs and reports
remain in ignored `work/reports/`. No assets, DLLs, generated game code or
saves belong in Git.
