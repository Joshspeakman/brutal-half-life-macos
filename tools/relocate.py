"""Prepare non-overlapping PE32 guest images; never modify the input DLLs."""
import json
import hashlib
import pefile
from inputs import ROOT, verify_module

BASES = {"hl.dll": 0x10000000, "client.dll": 0x30000000, "fmod.dll": 0x50000000}


def main():
    out = ROOT / "work/images"
    out.mkdir(parents=True, exist_ok=True)
    report = {}
    ranges = []
    for name, base in BASES.items():
        path = verify_module(name)
        pe = pefile.PE(str(path))
        if any(s.Name.startswith(b"UPX") for s in pe.sections):
            # Relocating just the packed loader is insufficient: its unpacked
            # code/data relocation stream still needs a separate preparation step.
            (out / name).unlink(missing_ok=True)
            report[name] = dict(prepared=False, reason="UPX-packed; unpack before relocation/translation")
            print(f"{name}: deferred (UPX-packed)")
            continue
        original_base = pe.OPTIONAL_HEADER.ImageBase
        delta = base - original_base
        fixups = []
        for block in pe.DIRECTORY_ENTRY_BASERELOC:
            for entry in block.entries:
                if entry.type == 0:
                    continue
                if entry.type != 3:
                    raise ValueError(f"{name}: unsupported relocation type {entry.type}")
                fixups.append((entry.rva, pe.get_dword_at_rva(entry.rva)))
        if delta and not fixups:
            raise ValueError(f"{name}: no relocations")
        pe.relocate_image(base)
        staged = out / (name + ".new")
        pe.write(str(staged))
        check = pefile.PE(str(staged))
        assert check.OPTIONAL_HEADER.ImageBase == base
        for rva, old in fixups:
            assert check.get_dword_at_rva(rva) == (old + delta) & 0xFFFFFFFF, f"{name}: fixup {rva:x}"
        size = check.OPTIONAL_HEADER.SizeOfImage
        assert base + size <= 0xFFF00000, "Image collides with import-thunk range"
        for start, end in ranges:
            assert base + size <= start or base >= end, "Guest image ranges overlap"
        ranges.append((base, base + size))
        staged.replace(out / name)
        report[name] = dict(prepared=True, original_base=f"0x{original_base:08x}", guest_base=f"0x{base:08x}",
                            image_size=size, checked_highlow_fixups=len(fixups),
                            sha256=hashlib.sha256((out / name).read_bytes()).hexdigest())
        print(f"{name}: {base:08x}, {len(fixups)} relocation words verified")
    (out / "layout.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
