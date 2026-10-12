# Brutal Half-Life v3 native recompilation

A local research project to translate the original v3 Windows x86 game
modules into native Apple Silicon code and connect them to a native engine.
This project is **not playable yet**. It currently builds and tests four
original server interface functions as ARM64 code, without Wine or Rosetta.

The implementation uses an x86-to-C static recompiler and a 32-bit guest
address space. Clang compiles the generated C to ARM64. Unicorn executes
the original x86 instructions only as a development test oracle; it is not
part of the native runtime. The 2017 BHL source is reference material, not
a substitute for the downloaded v3 behavior.

## First milestone

On an Apple Silicon Mac with Python 3 and Xcode Command Line Tools:

```sh
./bootstrap.sh /path/to/bhl_v3.zip
```

This verifies the three original DLL hashes, inventories their interfaces,
prepares relocated server/client copies, recompiles the server interface
exports and compares them with the original code. All binaries, generated
translations and reports stay in ignored `work/`. Original downloads,
installed games and saves are never modified by these tools.

The native library is `work/probe/libbhl_interfaces.dylib`. The test performs
1,000 deterministic comparisons across `GetEntityAPI`, `GetEntityAPI2`,
`GetNewDLLFunctions` and `GiveFnptrsToDll`. It compares integer registers,
defined flags, XMM registers and all writable image/stack/scratch bytes.
Inputs cover accepted/rejected versions, null output tables, unaligned
destinations and randomized engine-table contents. Invalid version-pointer
arguments are outside this test's supported calling contract.

Passing these tests does not validate DLL initialization, engine callbacks,
entity layout, weapons, maps, rendering, audio or save compatibility.

## Analysis and translation audit

After bootstrap:

```sh
.venv/bin/python vendor/lifter/analyze.py work/inputs/hl.dll work/reports/server-analysis.json
.venv/bin/python tools/lift_module.py hl.dll work/reports/server-analysis.json --check-c
```

The initial server scan discovers 4,082 candidate functions. This is heuristic
disassembly, not a count of verified routines. All 41 generated C files pass
Clang's ARM64 syntax check. Generated whole-module code
still contains explicit traps for unsupported instructions and unresolved
control flow. The translation audit is not linked into the four-function
probe, and it must not be labeled a working server.

See [the port plan](docs/PORT.md) for module boundaries, outstanding work and
acceptance criteria. [NOTICE.txt](NOTICE.txt) records recompiler provenance.
No mod/game binaries, assets, generated game code or saves belong in Git.
