# Native port plan and verified starting point

## Input identity

The target is the original Brutal Half-Life v3 archive, not a recreation
based on the incomplete 2017 repository. `config/modules.json` pins each
DLL by SHA256. A different build must get a separate profile and tests.

| Module | Export names | Static imports | Current finding |
|---|---:|---:|---|
| hl.dll | 643 | 71 | Server gameplay and entities; imports KERNEL32 |
| client.dll | 675 | 648 | Client, VGUI, SDL2, OpenGL and Windows calls |
| fmod.dll | 222 | 9 visible in packed loader | UPX-packed; actual dependencies need unpacking |

Client static imports include 549 VGUI symbols. It also contains dynamic
FMOD API-name strings, so the static import table is not the full dependency
surface. Presence of a string does not prove an API is used during gameplay.

All three modules prefer guest base 0x10000000. The preparation layout keeps
the server there and rebases the client to 0x30000000; 0x50000000 is reserved
for the audio module. The packed FMOD module is explicitly deferred.
The rebaser checks every HIGHLOW fixup in the prepared server/client files
against the original value plus relocation delta. This is not a loader test.

## Milestone 1: completed interface proof

Four original server exports compile to an ARM64 Mach-O library and pass
1,000 comparisons with their original x86 implementations. The probe uses
the original initialized PE data, but does not execute DllMain or CRT startup.
There are no Windows API stubs involved in these four functions.

The whole-server audit emits 4,082 candidate function translations across
41 C files, all accepted by Clang's ARM64 syntax checker. It reports 22 kinds
of unsupported instructions and two unresolved jump-table sites. Some may
be unreachable CPU-dispatch branches or incorrectly discovered data; that
must be investigated, not assumed. This code has not been linked or run.

The observed interface is DLL_FUNCTIONS version 140: 50 guest pointers
(200 bytes). NEW_DLL_FUNCTIONS version 1 copies five guest pointers
(20 bytes). GiveFnptrsToDll copies 159 engine pointers (636 bytes), stores
the globals pointer and uses stdcall stack cleanup; the other three exports
use cdecl. The 64-bit engine's pointers cannot simply be cast into those
32-bit slots.

## Milestone 2: loader and server initialization

1. Build a reproducible PE data loader, import-thunk table, guest stack/heap
   and thread state. Keep the caller's executable data files read-only.
2. Audit CRT/DllMain execution and implement the observed KERNEL32 behavior.
   Unsupported calls must fail with the module/address/API name; never
   return fabricated success to get farther into startup.
3. Compile the reachable server call graph. Review discovered targets,
   unresolved jump tables, SSE/AVX branches and exception paths individually.
   CPU-feature choices must agree with instruction support. x87 precision
   requires separate differential coverage before gameplay claims.
4. Pass deterministic initialization tests and register the engine callbacks
   without invoking unimplemented bridge entries.

## Milestone 3: native engine boundary

Use native Xash3D as the first engine candidate. Confirm its ABI against
the mod's interface rather than treating the old SDK as authoritative.
Define adapters for engine callbacks, strings, edicts, entvars, globals,
traces, user messages, saves and movement state. Guest pointers stay 32-bit
offsets; host pointers and native function pointers need explicit handles
or marshaling. C++ objects/vtables stay inside the translated guest module.

The engine bridge is not implemented. Engine ABI feasibility remains an
open integration risk: success in the isolated probe does not resolve it.
First acceptance target is an isolated headless map session with entity
creation and server ticks, followed by save/load and level transitions.

## Milestone 4: client, graphics and audio

Recompile the relocated client and bridge its callbacks, SDL2, OpenGL and
VGUI calls. Handle FMOD after inspecting the unpacked module and actual
call sites; choose faithful translation or a documented native audio API
adapter based on that evidence. Neither audio nor graphics is working here.

Acceptance requires the real v3 HUD, weapon switching, input, viewmodels,
particles and positional audio in an isolated native client/server session.

## Milestone 5: gameplay and packaging

Exercise every v3 weapon and enemy addition, scripted campaign events,
map transitions and repeated save/reload. Compare representative scenarios
with the original execution and keep performance claims measured.
Only then add a native launcher and local installation. Nightly publication
and a playable app are not part of this initial bootstrap milestone.
