"""Compare original x86 exports with their compiled arm64 translations."""
import ctypes as C
import json
import random
import struct
import sys
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as X

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from inputs import verify_module

REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
UREGS = {r: getattr(X, "UC_X86_REG_" + r.upper()) for r in REGS}
STACK, STACK_SIZE = 0x00200000, 0x10000
SCRATCH, SCRATCH_SIZE = 0x20000000, 0x10000
SENTINEL = 0x00100000


class Flags(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("op", "a", "b", "r", "cin", "eflags", "df")]


class Context(C.Structure):
    _fields_ = [(r, C.c_uint32) for r in REGS] + [
        ("f", Flags), ("st", C.c_double * 8), ("top", C.c_uint32),
        ("fcw", C.c_uint32), ("fsw", C.c_uint32), ("fs_base", C.c_uint32),
        ("tc", C.c_uint32), ("mxcsr", C.c_uint32),
        ("xmm", (C.c_uint8 * 16) * 8), ("mm", C.c_uint64 * 8)]


class Harness:
    def __init__(self):
        self.pe = pefile.PE(str(verify_module("hl.dll")))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.image = self.pe.get_memory_mapped_image().ljust(self.size, b"\0")
        self.exports = json.loads((ROOT / "work/probe/exports.json").read_text())
        self.lib = C.CDLL(str(ROOT / "work/probe/libbhl_interfaces.dylib"))
        self.lib.bhl_ctx_size.restype = C.c_size_t
        self.lib.bhl_ctx_xmm_offset.restype = C.c_size_t
        assert self.lib.bhl_ctx_size() == C.sizeof(Context), "C/Python context layout differs"
        assert self.lib.bhl_ctx_xmm_offset() == Context.xmm.offset
        self.lib.rt_test_mem.restype = C.c_void_p
        self.memory = self.lib.rt_test_mem()
        assert self.memory, "Guest address-space allocation failed"
        self.lib.rt_test_run.argtypes = [C.POINTER(Context), C.c_uint32,
                                        C.POINTER(C.c_uint32), C.POINTER(C.c_char_p)]
        self.lib.rt_test_flags.argtypes = [C.POINTER(Context)]
        self.lib.rt_test_flags.restype = C.c_uint32
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(self.base, self.size)
        self.uc.mem_map(STACK, STACK_SIZE)
        self.uc.mem_map(SCRATCH, SCRATCH_SIZE)
        self.uc.mem_map(SENTINEL, 0x1000)
        self.uc.mem_write(SENTINEL, b"\xcc" * 0x1000)
        self.uc.mem_write(self.base, self.image)
        C.memmove(self.memory + self.base, self.image, len(self.image))
        self.writable = [(self.base + s.VirtualAddress, s.Misc_VirtualSize)
                         for s in self.pe.sections if s.Characteristics & 0x80000000]
        self.regions = self.writable + [(STACK, STACK_SIZE), (SCRATCH, SCRATCH_SIZE)]

    def write(self, address, data):
        self.uc.mem_write(address, data)
        C.memmove(self.memory + address, data, len(data))

    def reset(self):
        for address, size in self.writable:
            self.write(address, self.image[address-self.base:address-self.base+size])
        self.write(STACK, bytes(STACK_SIZE))
        self.write(SCRATCH, b"\xa5" * SCRATCH_SIZE)

    def run(self, name, args, rng):
        context = Context()
        for r in REGS:
            value = rng.getrandbits(32)
            setattr(context, r, value)
            self.uc.reg_write(UREGS[r], value)
        context.esp = STACK + STACK_SIZE - 0x100
        self.write(context.esp, struct.pack("<" + "I" * (1 + len(args)), SENTINEL, *args))
        self.uc.reg_write(X.UC_X86_REG_ESP, context.esp)
        context.f.eflags = 0x202
        context.fcw, context.mxcsr = 0x27F, 0x1F80
        self.uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
        self.uc.reg_write(X.UC_X86_REG_FPCW, context.fcw)
        self.uc.reg_write(X.UC_X86_REG_FPSW, 0)
        self.uc.reg_write(X.UC_X86_REG_FPTAG, 0xFFFF)
        self.uc.reg_write(X.UC_X86_REG_MXCSR, context.mxcsr)
        for i in range(8):
            value = rng.getrandbits(128)
            context.xmm[i][:] = value.to_bytes(16, "little")
            self.uc.reg_write(getattr(X, f"UC_X86_REG_XMM{i}"), value)
        address = self.exports[name]
        self.uc.emu_start(address, SENTINEL, count=100000)
        assert self.uc.reg_read(X.UC_X86_REG_EIP) == SENTINEL, f"{name}: x86 did not return"
        pc, why = C.c_uint32(), C.c_char_p()
        result = self.lib.rt_test_run(C.byref(context), address, C.byref(pc), C.byref(why))
        assert result == 0, f"{name}: native fault at {pc.value:08x}: {why.value}"
        for r in REGS:
            assert getattr(context, r) == self.uc.reg_read(UREGS[r]), f"{name}: register {r} differs"
        # AF is undefined after TEST; compare the defined arithmetic/direction bits.
        flag_mask = 0xCC5
        assert self.lib.rt_test_flags(C.byref(context)) & flag_mask == self.uc.reg_read(X.UC_X86_REG_EFLAGS) & flag_mask
        for i in range(8):
            assert bytes(context.xmm[i]) == self.uc.reg_read(getattr(X, f"UC_X86_REG_XMM{i}")).to_bytes(16, "little")
        for start, size in self.regions:
            assert bytes(self.uc.mem_read(start, size)) == C.string_at(self.memory + start, size), f"{name}: memory differs at {start:08x}"
        return context.eax


def main():
    h = Harness()
    rng = random.Random(0xB417A1)
    counts = {}
    for name in h.exports:
        for i in range(250):
            h.reset()
            # Exercise aligned and deliberately unaligned destinations, accepted
            # and rejected versions, null tables, and randomized engine pointers.
            output = SCRATCH + 0x1000 + i % 16
            version_ptr = SCRATCH + 0x500
            if name == "GiveFnptrsToDll":
                values = [rng.getrandbits(32) for _ in range(159)]
                h.write(output, struct.pack("<159I", *values))
                args = [output, SCRATCH + 0x8000 + i * 4]
                h.run(name, args, rng)
            else:
                expected_version = 1 if name == "GetNewDLLFunctions" else 140
                version = expected_version if i % 4 < 2 else rng.choice([0, 1, 139, 141, 0xFFFFFFFF])
                if i % 4 == 1:
                    output = 0
                h.write(version_ptr, struct.pack("<I", version))
                args = [output, version if name == "GetEntityAPI" else version_ptr]
                result = h.run(name, args, rng)
                assert result == int(bool(output) and version == expected_version)
                if result:
                    size = 20 if name == "GetNewDLLFunctions" else 200
                    table = bytes(h.uc.mem_read(output, size))
                    assert any(table), "Accepted interface returned an empty table"
            counts[name] = counts.get(name, 0) + 1
    report = dict(native_arch="arm64", reference="original x86 in Unicorn 2.1.4",
                  seed="0xB417A1", cases=counts, total=sum(counts.values()), mismatches=0,
                  compared=["integer registers", "defined flags", "XMM registers", "all writable image bytes", "stack", "scratch"],
                  scope="Interface exports only; no DLL initialization, callbacks, engine, maps or gameplay")
    out = ROOT / "work/reports/interface-tests.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {sum(counts.values())} x86/arm64 comparisons across {len(counts)} original v3 exports")


if __name__ == "__main__":
    main()
