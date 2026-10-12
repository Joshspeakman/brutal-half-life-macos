"""Prepare non-overlapping PE32 guest images; never modify the input DLLs."""
import json
import hashlib
import struct
import pefile
from inputs import ROOT, verify_module
from unpack_audio import verified_unpacked

BASES = {"hl.dll": 0x10000000, "client.dll": 0x30000000, "fmod.dll": 0x50000000, "vgui.dll": 0x40000000}


def main():
    out = ROOT / "work/images"
    out.mkdir(parents=True, exist_ok=True)
    report = {}
    ranges = []
    for name, base in BASES.items():
        if name=="vgui.dll" and not (ROOT/'work/inputs'/name).exists():
            continue
        path = verify_module(name)
        pe = pefile.PE(str(path))
        if any(s.Name.startswith(b"UPX") for s in pe.sections):
            # Relocating just the packed loader is insufficient: its unpacked
            # code/data relocation stream still needs a separate preparation step.
            if not (ROOT / "work/unpacked" / name).exists():
                (out / name).unlink(missing_ok=True)
                report[name] = dict(prepared=False, reason="UPX-packed; unpack before relocation/translation")
                print(f"{name}: deferred (UPX-packed)")
                continue
            path = verified_unpacked()
            pe = pefile.PE(str(path))
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
        # pefile.write() can rewrite parsed import structures incorrectly for
        # these inputs. Patch only the relocation words and ImageBase in the
        # original bytes; preserve every other file byte exactly.
        payload = bytearray(path.read_bytes())
        for rva, old in fixups:
            struct.pack_into("<I", payload, pe.get_offset_from_rva(rva), (old + delta) & 0xFFFFFFFF)
        struct.pack_into("<I", payload, pe.OPTIONAL_HEADER.get_field_absolute_offset("ImageBase"), base)
        staged = out / (name + ".new")
        staged.write_bytes(payload)
        check = pefile.PE(str(staged))
        assert check.OPTIONAL_HEADER.ImageBase == base
        for rva, old in fixups:
            assert check.get_dword_at_rva(rva) == (old + delta) & 0xFFFFFFFF, f"{name}: fixup {rva:x}"
        def imports(image):
            return [(d.dll, [(i.name, i.ordinal, i.address-image.OPTIONAL_HEADER.ImageBase) for i in d.imports])
                    for d in image.DIRECTORY_ENTRY_IMPORT]
        def exports(image):
            return [(s.name, s.ordinal, s.address, s.forwarder) for s in image.DIRECTORY_ENTRY_EXPORT.symbols]
        assert imports(pe) == imports(check), f"{name}: import table changed"
        assert exports(pe) == exports(check), f"{name}: export table changed"
        size = check.OPTIONAL_HEADER.SizeOfImage
        assert base + size <= 0xFFF00000, "Image collides with import-thunk range"
        for start, end in ranges:
            assert base + size <= start or base >= end, "Guest image ranges overlap"
        ranges.append((base, base + size))
        staged.replace(out / name)
        report[name] = dict(prepared=True, original_base=f"0x{original_base:08x}", guest_base=f"0x{base:08x}",
                            image_size=size, checked_highlow_fixups=len(fixups),
                            import_export_identity_verified=True,
                            sha256=hashlib.sha256((out / name).read_bytes()).hexdigest())
        print(f"{name}: {base:08x}, {len(fixups)} relocation words verified")
    (out / "layout.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
