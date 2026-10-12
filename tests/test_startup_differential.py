"""Compare native CRT startup with original x86 using the same native OS services.

This validates translation, not equivalence of our OS services to Windows.
Every API boundary compares integer registers and writable image/heap/stack
bytes. Independent API contract tests cover the native service implementations.
"""
import ctypes as C
import argparse
import hashlib
import json
import shutil
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_HOOK_INSN, UcError, UC_ERR_FETCH_UNMAPPED
from unicorn import x86_const as X
from native_harness import Native, ROOT
from test_interfaces import Context, REGS, UREGS

STOP, GDT = 0x00100000, 0x00300000
OBSERVER = C.CFUNCTYPE(None, C.POINTER(Context), C.c_uint32, C.c_int)


def descriptor(base, limit, access, flags):
    return struct.pack('<Q', (limit & 65535) | ((base & 0xffffff) << 16) | (access << 40)
                       | (((limit >> 16) & 15) << 48) | (flags << 52) | ((base >> 24) << 56))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--module',choices=['hl.dll','client.dll','fmod.dll','vgui.dll'],default='hl.dll')
    module=ap.parse_args().module
    out = ROOT / "work/differential"
    out.mkdir(exist_ok=True)
    for name in ("native", "oracle"):
        shutil.copyfile(ROOT / "work/native/libbhl.dylib", out / (name + ".dylib"))
    a = Native(library=out / "native.dylib")
    records = []
    regions = [(0x703f0000, 0x10000)]
    pe = a.images[module]["pe"]
    base=a.images[module]['meta']['base']
    regions += [(base + s.VirtualAddress, s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x80000000]
    def snapshot(n, cp):
        c = cp.contents
        memory = [(p, n.read(p, size)) for p, size in regions]
        memory.append((0x60000000, n.read(0x60000000, n.lib.bhl_heap_used())))
        return tuple(getattr(c, r) for r in REGS), memory
    def record(cp, index, phase):
        records.append((index, phase, snapshot(a, cp)))
    observer_a = OBSERVER(record)
    a.lib.bhl_set_import_observer.argtypes = [OBSERVER]
    a.lib.bhl_set_import_observer(observer_a)
    assert a.attach(module) == 1
    final_a = snapshot(a, C.pointer(a.context))
    b = Native(library=out / "oracle.dylib")
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map_ptr(0x10000, 0xffe00000 - 0x10000, UC_PROT_ALL, b.memory + 0x10000)
    gdt = descriptor(0,0,0,0) + descriptor(0,0xfffff,0x9b,12) + descriptor(0,0xfffff,0x93,12) + descriptor(b.context.fs_base,0xfff,0x93,4)
    uc.mem_write(GDT,gdt)
    uc.reg_write(X.UC_X86_REG_GDTR,(0,GDT,len(gdt)-1,0))
    for r in (X.UC_X86_REG_SS,X.UC_X86_REG_DS,X.UC_X86_REG_ES):uc.reg_write(r,0x10)
    uc.reg_write(X.UC_X86_REG_FS,0x18)
    for r in REGS:uc.reg_write(UREGS[r],getattr(b.context,r))
    for reg,value in [(X.UC_X86_REG_EFLAGS,0x202),(X.UC_X86_REG_FPCW,0x27f),(X.UC_X86_REG_FPSW,0),(X.UC_X86_REG_FPTAG,0xffff),(X.UC_X86_REG_MXCSR,0x1f80)]:uc.reg_write(reg,value)
    def cpuid(uc, data):
        leaf=uc.reg_read(X.UC_X86_REG_EAX)
        values=(1,0x756e6547,0x6c65746e,0x49656e69) if leaf==0 else (0x6f6,0,0,0x078081bf)
        for reg,value in zip((X.UC_X86_REG_EAX,X.UC_X86_REG_EBX,X.UC_X86_REG_ECX,X.UC_X86_REG_EDX),values):uc.reg_write(reg,value)
        return True
    uc.hook_add(UC_HOOK_INSN,cpuid,None,1,0,X.UC_X86_INS_CPUID)
    b.write(b.context.esp,struct.pack('<4I',STOP,base,1,0))
    cursor=0
    def compare(actual, expected, label):
        if actual[0]!=expected[0]:
            differences=[f'{r}: {x:08x} != {y:08x}' for r,x,y in zip(REGS,actual[0],expected[0]) if x!=y]
            raise AssertionError(f'{label}: '+', '.join(differences))
        for (p,x),(q,y) in zip(actual[1],expected[1]):
            if p!=q or x!=y:
                first=next((i for i,(v,w) in enumerate(zip(x,y)) if v!=w),min(len(x),len(y)))
                raise AssertionError(f'{label}: memory differs at {p+first:08x}; lengths {len(x)}/{len(y)}')
    b.lib.rt_import.argtypes=[C.POINTER(Context),C.c_uint32]
    pc=b.images[module]['meta']['entry']
    while True:
        try:
            uc.emu_start(pc,STOP,count=10000000)
            assert uc.reg_read(X.UC_X86_REG_EIP)==STOP,'instruction budget exhausted'
            break
        except UcError as error:
            pc=uc.reg_read(X.UC_X86_REG_EIP)
            if error.errno!=UC_ERR_FETCH_UNMAPPED or pc<0xfff00000:
                raise RuntimeError(f'x86 oracle fault at {pc:08x}, import boundary {cursor}: {error}') from error
        index=(pc-0xfff00000)//16
        for r in REGS:setattr(b.context,r,uc.reg_read(UREGS[r]))
        ret=struct.unpack('<I',b.read(b.context.esp,4))[0]
        for phase in (0,1):
            ri,rp,expected=records[cursor]
            assert (ri,rp)==(index,phase),f'API order differs at {cursor}: {index}/{phase} != {ri}/{rp}'
            if phase:b.lib.rt_import(C.byref(b.context),index)
            compare(snapshot(b,C.pointer(b.context)),expected,f'import {cursor//2} {b.lib.bhl_import_name(index).decode()} phase {phase}')
            cursor+=1
        for r in REGS:uc.reg_write(UREGS[r],getattr(b.context,r))
        pc=ret
    for r in REGS:setattr(b.context,r,uc.reg_read(UREGS[r]))
    assert cursor==len(records)
    compare(snapshot(b,C.pointer(b.context)),final_a,'final')
    report=dict(module=module,imports=cursor//2,boundaries_compared=cursor,dll_process_attach=b.context.eax,
                differences=0,os_services='shared native model; not Windows differential',gameplay_tested=False)
    (ROOT/'work/reports'/f'{module}-startup-differential.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
