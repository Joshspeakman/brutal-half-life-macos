"""Inventory imports, exports, relocations and interface code without executing DLLs."""
import json
import capstone
import pefile
from inputs import ROOT, PROFILE, verify_module

INTERFACES = {"GetEntityAPI", "GetEntityAPI2", "GetNewDLLFunctions", "GiveFnptrsToDll",
              "Initialize", "HUD_Init", "HUD_VidInit", "HUD_Redraw", "HUD_GetHullBounds"}


def inspect(name):
    pe = pefile.PE(str(verify_module(name)))
    if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
        raise ValueError("Expected x86 PE32")
    base = pe.OPTIONAL_HEADER.ImageBase
    imports = {}
    for dll in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        imports[dll.dll.decode()] = [dict(name=i.name.decode() if i.name else f"#{i.ordinal}",
                                         iat=f"0x{i.address:08x}") for i in dll.imports]
    exports = []
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    excerpts = {}
    for symbol in getattr(pe, "DIRECTORY_ENTRY_EXPORT").symbols:
        label = symbol.name.decode() if symbol.name else f"#{symbol.ordinal}"
        address = base + symbol.address
        exports.append(dict(name=label, ordinal=symbol.ordinal, address=f"0x{address:08x}",
                            forwarder=symbol.forwarder.decode() if symbol.forwarder else None))
        if label in INTERFACES:
            excerpts[label] = [f"{i.address:08x}: {i.mnemonic} {i.op_str}" for i in
                               md.disasm(pe.get_data(symbol.address, 128), address)]
    return dict(sha256=PROFILE["modules"][name]["sha256"], base=f"0x{base:08x}",
                packed=any(s.Name.startswith(b"UPX") for s in pe.sections),
                image_size=pe.OPTIONAL_HEADER.SizeOfImage,
                entry=f"0x{base + pe.OPTIONAL_HEADER.AddressOfEntryPoint:08x}",
                sections=[dict(name=s.Name.rstrip(b"\0").decode(), rva=s.VirtualAddress,
                               size=s.Misc_VirtualSize, flags=f"0x{s.Characteristics:08x}")
                          for s in pe.sections], imports=imports, exports=exports,
                relocations=sum(len(b.entries) for b in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])),
                interface_disassembly=excerpts)


def main():
    report = {name: inspect(name) for name in PROFILE["modules"]}
    out = ROOT / "work/reports"
    out.mkdir(parents=True, exist_ok=True)
    (out / "modules.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, module in report.items():
        count = sum(map(len, module["imports"].values()))
        print(f"{name}: {len(module['exports'])} exports, {count} imports, base {module['base']}")


if __name__ == "__main__":
    main()
