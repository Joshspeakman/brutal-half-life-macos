"""Load verified PE data into the ARM64 runtime; no original x86 is executed here."""
import ctypes as C
import hashlib
import json
import struct
import sys
from pathlib import Path
import pefile
from test_interfaces import Context

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from inputs import verify_module


class Native:
    def __init__(self, trace=False, library=None):
        self.lib = C.CDLL(str(library or ROOT / "work/native/libbhl.dylib"))
        self.lib.rt_test_mem.restype = C.c_void_p
        self.lib.rt_test_run.argtypes = [C.POINTER(Context), C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_char_p)]
        self.lib.bhl_process_init.argtypes = [C.POINTER(Context)]
        self.lib.bhl_heap_alloc.argtypes = [C.c_uint32, C.c_int]
        self.lib.bhl_heap_alloc.restype = C.c_uint32
        self.lib.bhl_thunk.argtypes = [C.c_char_p, C.c_void_p]
        self.lib.bhl_thunk.restype = C.c_uint32
        self.lib.bhl_import_name.restype = C.c_char_p
        self.lib.bhl_set_trace(int(trace))
        self.lib.bhl_set_deterministic(1)
        self.memory = self.lib.rt_test_mem()
        if not self.memory:
            raise RuntimeError("Unable to reserve guest memory")
        self.images = {}
        build = json.loads((ROOT / "work/native/build.json").read_text())
        for name in build["modules"]:
            verify_module(name)
            meta = json.loads((ROOT / "work/lifted" / name / "image.json").read_text())
            binary = ROOT / meta["binary"]
            if hashlib.sha256(binary.read_bytes()).hexdigest() != meta["sha256"]:
                raise RuntimeError(f"Image changed: {name}")
            pe = pefile.PE(str(binary))
            blob = pe.get_memory_mapped_image().ljust(pe.OPTIONAL_HEADER.SizeOfImage, b"\0")
            self.write(meta["base"], blob)
            exports = {e.name.decode(): meta["base"] + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
            self.images[name] = dict(pe=pe, meta=meta, exports=exports)
        self.context = Context()
        self.lib.bhl_process_init(C.byref(self.context))

    def write(self, address, blob):
        C.memmove(self.memory + address, blob, len(blob))

    def read(self, address, size):
        return C.string_at(self.memory + address, size)

    def alloc(self, size):
        ptr = self.lib.bhl_heap_alloc(size, 1)
        if not ptr:
            raise MemoryError(size)
        return ptr

    def call(self, address, args=(), stdcall=False):
        c = self.context
        before = 0x703fff00
        c.esp = before
        self.write(before, struct.pack("<" + "I" * (len(args)+1), 0x00100000, *args))
        pc, reason = C.c_uint32(), C.c_char_p()
        status = self.lib.rt_test_run(C.byref(c), address, C.byref(pc), C.byref(reason))
        if status:
            raise RuntimeError(f"{pc.value:08x}: {reason.value.decode()}")
        expected = before + 4 + (4 * len(args) if stdcall else 0)
        if c.esp != expected:
            raise RuntimeError(f"Stack mismatch: {c.esp:08x} != {expected:08x}")
        return c.eax

    def attach(self, module):
        meta = self.images[module]["meta"]
        return self.call(meta["entry"], [meta["base"], 1, 0], stdcall=True)
