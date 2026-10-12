"""Recompile four real server interface exports to an arm64 test library."""
import json
import platform
import subprocess
import sys
import pefile
from inputs import ROOT, verify_module

sys.path.insert(0, str(ROOT / "vendor/lifter"))
from lift import Lifter

NAMES = ("GetEntityAPI", "GetEntityAPI2", "GetNewDLLFunctions", "GiveFnptrsToDll")


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise SystemExit("The native probe requires an Apple Silicon Mac and arm64 Python")
    binary = verify_module("hl.dll")
    pe = pefile.PE(str(binary))
    exports = {s.name.decode(): pe.OPTIONAL_HEADER.ImageBase + s.address
               for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
    addresses = sorted(exports[name] for name in NAMES)
    out = ROOT / "work/probe"
    out.mkdir(parents=True, exist_ok=True)
    analysis = out / "seeds.json"
    analysis.write_text(json.dumps({"funcs": addresses}))
    lifter = Lifter(str(binary), str(analysis))
    lifter.jt_cache = {}
    lifter.hooks = {}
    lifter.overrides = {}
    lifter.illegal_seh = False  # Unknown instructions must not masquerade as CPU-feature probes.
    bodies = []
    for address in addresses:
        source, calls = lifter.lift_function(address)
        if calls:
            raise RuntimeError(f"Unexpected dependency for interface 0x{address:x}: {calls}")
        bodies.append(source)
    if lifter.unsup or lifter.internal or lifter.unres_jt:
        raise RuntimeError(f"Incomplete lift: {lifter.unsup}, {lifter.internal}, {lifter.unres_jt}")
    header = '#include "rt.h"\n'
    (out / "interfaces.c").write_text(header + "\n".join(bodies))
    table = header + "\n".join(f"void f_{a:08x}(Ctx *);" for a in addresses)
    table += "\nconst struct rt_fn { uint32_t addr; GuestFn fn; } rt_fntab[] = {\n"
    table += "".join(f"{{0x{a:08x}u, f_{a:08x}}},\n" for a in addresses)
    table += f"}};\nconst unsigned rt_fntab_n = {len(addresses)};\n"
    table += 'const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[] = {{0, ""}};\n'
    table += "const unsigned rt_imptab_n = 0;\n"
    table += 'const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[] = {{"hl.dll", 0x10000000u, 0, 0}};\n'
    table += "const unsigned rt_images_n = 1;\n"
    (out / "tables.c").write_text(table)
    destination = out / "libbhl_interfaces.dylib"
    staged = out / "libbhl_interfaces.new.dylib"
    subprocess.run(["clang", "-arch", "arm64", "-dynamiclib", "-O2", "-g",
                    "-ffp-contract=off", "-fno-strict-aliasing", "-Wall",
                    "-Wno-unused-variable", "-Wno-unused-label", "-I", str(ROOT / "runtime"),
                    str(ROOT / "runtime/rt.c"), str(ROOT / "tests/abi_layout.c"),
                    str(out / "interfaces.c"), str(out / "tables.c"), "-o", str(staged)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(staged)], check=True)
    staged.replace(destination)
    (out / "exports.json").write_text(json.dumps({n: exports[n] for n in NAMES}, indent=2) + "\n")
    print(f"Built {len(addresses)} real v3 exports as arm64; no unsupported instructions")


if __name__ == "__main__":
    main()
