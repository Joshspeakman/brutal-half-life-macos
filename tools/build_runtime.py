"""Compile complete audited modules and the native runtime (not an installed game)."""
import argparse
from pathlib import Path
import hashlib
import json
import os
import shlex
import subprocess
import pefile
from concurrent.futures import ThreadPoolExecutor
from inputs import ROOT, PROFILE, verify_module
from generate_nls import generate as generate_nls


def build_game_only(args, out, sources):
    """The translated modules and their tables as libgame.dylib, which the app's runtime (built with --split) binds at load.
    No LTO: the player's Mac links it with ld64.lld."""
    cc = [os.environ.get("CC", "clang")] + (shlex.split(os.environ["BHL_CFLAGS"]) if os.environ.get("BHL_CFLAGS") else ["-arch", "arm64"])
    flags = cc + ["-std=gnu11", f"-O{args.opt}", "-DRT_MEMORY_WATCH", "-DRT_SPLIT", "-ffp-contract=off", "-fno-strict-aliasing", "-Wno-unused-value",
                  "-w", "-I", str(ROOT / "runtime"), "-I", str(out)]
    objdir = out / "game-obj"
    objdir.mkdir(parents=True, exist_ok=True)
    def compile_one(source):
        obj = objdir / (source.parent.name + "_" + source.stem + ".o")
        if not obj.exists():
            subprocess.run(flags + ["-c", str(source), "-o", str(obj) + ".tmp"], check=True)
            os.replace(str(obj) + ".tmp", obj)
        return obj
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        objects = list(pool.map(compile_one, sources))
    lib = out / "libgame.dylib"
    ld = os.environ.get("BHL_LD")
    # the app's runtime, linked: its thread-locals (rt_watch_pages) can't be bound by name at load (ld64.lld); found
    # at run time through the engine's DYLD_LIBRARY_PATH (the app's Runtime folder)
    runtime = [os.environ["BHL_RUNTIME_LIB"]] if os.environ.get("BHL_RUNTIME_LIB") else []
    if ld:
        cmd = [ld, "-dylib", "-arch", "arm64", "-platform_version", "macos", "14.0", "14.0", "-undefined", "dynamic_lookup",
               "-install_name", "@rpath/libgame.dylib", "-o", str(lib) + ".tmp", *map(str, objects), *runtime]
    else:
        cmd = ["clang", "-arch", "arm64", "-dynamiclib", "-undefined", "dynamic_lookup", "-install_name", "@rpath/libgame.dylib",
               "-o", str(lib) + ".tmp", *map(str, objects), *runtime]
    subprocess.run(cmd, check=True)
    os.replace(str(lib) + ".tmp", lib)
    print(f"Linked the game library: {len(objects)} objects")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("modules", nargs="+", choices=list(PROFILE['modules']))
    ap.add_argument("--opt", choices=["0", "1", "2"], default="2")
    ap.add_argument("--lto", action=argparse.BooleanOptionalAction, default=True,
                    help="Enable ThinLTO across translated modules (default; --no-lto disables it)")
    ap.add_argument("--game-only", action="store_true",
                    help="Only the game's library (libgame.dylib) for a split runtime: the drag-and-drop app's setup on the player's "
                         "Mac, with its own clang (CC, BHL_CFLAGS) and linker (BHL_LD); no SDK, no runtime")
    ap.add_argument("--out", type=Path, default=ROOT / "work/native")
    ap.add_argument("--split", action="store_true",
                    help="The runtime (libbhl.dylib) and the translated modules (libgame.dylib) as two libraries, for the "
                         "drag-and-drop app: its runtime is built here, the game's library on the player's Mac")
    args = ap.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    generate_nls(out / 'nls_tables.h')
    functions, imports, images, sources, metadata = [], [], [], [], []
    for module in args.modules:
        verify_module(module)
        directory = ROOT / "work/lifted" / module
        meta = json.loads((directory / "image.json").read_text())
        if hashlib.sha256((ROOT / meta["binary"]).read_bytes()).hexdigest() != meta["sha256"]:
            raise RuntimeError(f"{module}: translation input changed")
        if meta["imp_base"] != len(imports):
            raise RuntimeError(f"{module}: import indices overlap or leave a gap")
        images.append((module, meta["base"], len(imports), len(meta["imports"])))
        functions.extend(meta["functions"])
        imports.extend(meta["imports"])
        sources.extend(sorted(directory.glob("lift_*.c")))
        metadata.append(meta)
    if len(set(functions)) != len(functions):
        raise RuntimeError("Overlapping module function addresses")
    functions.sort()
    tables = '#include "rt.h"\n'
    tables += ''.join(f'void f_{f:08x}(Ctx *);\n' for f in functions)
    tables += 'const struct rt_fn { uint32_t addr; GuestFn fn; } rt_fntab[] = {\n'
    tables += ''.join(f'{{0x{f:08x}u, f_{f:08x}}},\n' for f in functions)
    tables += f'}};\nconst unsigned rt_fntab_n = {len(functions)};\n'
    tables += 'const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[] = {\n'
    tables += ''.join(f'{{0x{a:08x}u, {json.dumps(n)}}},\n' for a, n in imports)
    tables += f'}};\nconst unsigned rt_imptab_n = {len(imports)};\n'
    tables += 'const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[] = {\n'
    tables += ''.join(f'{{{json.dumps(n)}, 0x{b:08x}u, {i}, {c}}},\n' for n, b, i, c in images)
    tables += f'}};\nconst unsigned rt_images_n = {len(images)};\n'
    (out / "tables.c").write_text(tables)
    description = '#include "loader.h"\n'
    rows = []
    for i, meta in enumerate(metadata):
        pe = pefile.PE(str(ROOT / meta['binary']))
        exports = sorted((s.name.decode(), meta['base']+s.address) for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name and not s.forwarder)
        description += f'static const BhlExport exports_{i}[] = {{\n'
        description += ''.join(f'{{{json.dumps(n)},0x{a:08x}u}},\n' for n,a in exports) + '};\n'
        rows.append(f'{{{json.dumps(meta["module"])},{json.dumps(meta["sha256"])},0x{meta["base"]:08x}u,{pe.OPTIONAL_HEADER.SizeOfImage},0x{meta["entry"]:08x}u,exports_{i},{len(exports)}}},\n')
    description += 'const BhlModule bhl_modules[] = {\n' + ''.join(rows) + '};\n'
    description += f'const unsigned bhl_module_count={len(metadata)};\n'
    (out / "modules.c").write_text(description)
    if args.game_only:
        build_game_only(args, out, sources + [out / "tables.c", out / "modules.c"])
        return
    sources += [ROOT / "runtime/rt.c", ROOT / "runtime/memory_watch.c", ROOT / "runtime/win32.c", ROOT / "runtime/loader.c", ROOT / "runtime/platform.c", ROOT / "runtime/directsound.c", ROOT / "runtime/threading.c", ROOT / "runtime/gdi.c", ROOT / "tests/abi_layout.c", out / "tables.c", out / "modules.c"]
    sdl_cflags=shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2'],text=True))
    sdl_libs=shlex.split(subprocess.check_output(['pkg-config','--libs','sdl2'],text=True))
    target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")     # in the flags, so objects built for another macOS are rebuilt
    flags = ["clang", "-arch", "arm64", *([f"-mmacosx-version-min={target}"] if target else []), "-std=gnu11", f"-O{args.opt}", "-g",
             "-DRT_MEMORY_WATCH", "-ffp-contract=off", "-fno-strict-aliasing", "-Wno-unused-value",
             "-I", str(ROOT / "runtime"), "-I", str(out)] + (["-DRT_SPLIT"] if args.split else [])
    if args.lto:flags += ['-flto=thin']
    headers = b''.join(p.read_bytes() for p in sorted((ROOT / "runtime").glob("*.h")))
    compiler = subprocess.check_output(["clang", "--version"])
    def compile_one(source):
        compile_flags=flags+(sdl_cflags if source.name in ('platform.c','directsound.c') else [])
        extra = (out / 'nls_tables.h').read_bytes() + (ROOT/'runtime/guest_format.inc').read_bytes() if source.name == 'win32.c' else b''
        digest = hashlib.sha256(source.read_bytes() + headers + extra + compiler + repr(compile_flags).encode()).hexdigest()
        obj = out / (source.parent.name + "_" + source.stem + ".o")
        stamp = obj.with_suffix(".sha256")
        if not obj.exists() or not stamp.exists() or stamp.read_text() != digest:
            staged = obj.with_suffix(".new.o")
            subprocess.run(compile_flags + ["-c", str(source), "-o", str(staged)], check=True)
            staged.replace(obj)
            stamp.write_text(digest)
            print(f"Compiled {source.parent.name}/{source.name}", flush=True)
        return obj
    with ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as pool:
        objects = list(pool.map(compile_one, sources))
    staged = out / "libbhl.new.dylib"
    split = []
    if args.split:   # the game: lifted code and its tables, resolving the runtime's symbols when loaded (weakly linked below)
        is_game = lambda s: s.parent.parent == ROOT / "work/lifted" or s.parent == out   # lift_*.c, tables.c, modules.c
        game = [o for o, s in zip(objects, sources) if is_game(s)]
        objects = [o for o, s in zip(objects, sources) if not is_game(s)]
        lib = out / "libgame.new.dylib"
        subprocess.run(["clang", "-arch", "arm64", "-dynamiclib", *(['-flto=thin'] if args.lto else []), *map(str, game),
                        "-undefined", "dynamic_lookup", "-install_name", "@rpath/libgame.dylib", "-o", str(lib)], check=True)
        lib.replace(out / "libgame.dylib")
        split = ["-Wl,-weak_library," + str(out / "libgame.dylib"), "-Wl,-rpath,@loader_path"]
    subprocess.run(["clang", "-arch", "arm64", "-dynamiclib", *(['-flto=thin'] if args.lto else []), *map(str, objects), *split, *sdl_libs, "-liconv", "-framework", "CoreFoundation", "-framework", "CoreText", "-framework", "CoreGraphics",
                    "-install_name", "@rpath/libbhl.dylib", "-o", str(staged)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(staged)], check=True)
    staged.replace(out / "libbhl.dylib")
    (out / "build.json").write_text(json.dumps(dict(modules=args.modules, functions=len(functions),
        imports=len(imports), architecture="arm64", optimization=args.opt, thin_lto=args.lto, split=args.split, gameplay_tested=False), indent=2) + "\n")
    print(f"Linked {len(functions)} functions and {len(imports)} imports; gameplay validation pending")


if __name__ == "__main__":
    main()
