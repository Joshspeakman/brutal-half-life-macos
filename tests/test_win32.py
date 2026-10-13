"""Independent service-contract tests through the guest calling convention."""
import json
import os
import struct
import tempfile
import unittest
from native_harness import Native, ROOT


class Services(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.n = Native()

    def api(self, name, *args):
        address = self.n.lib.bhl_thunk(('KERNEL32.dll!' + name).encode(), None)
        return self.n.call(address, args, stdcall=True)

    def blob(self, data):
        p = self.n.alloc(len(data))
        self.n.write(p, data)
        return p

    def words(self, address, count):
        return struct.unpack('<' + 'I' * count, self.n.read(address, 4 * count))

    def test_codepage_roundtrip_and_c1_controls(self):
        source = self.blob(bytes(range(256)))
        wide, back = self.n.alloc(512), self.n.alloc(256)
        self.assertEqual(self.api('MultiByteToWideChar', 1252, 1, source, 256, wide, 256), 256)
        self.assertEqual(struct.unpack('<H', self.n.read(wide + 0x80 * 2, 2))[0], 0x20ac)
        self.assertEqual(struct.unpack('<H', self.n.read(wide + 0x81 * 2, 2))[0], 0x81)
        used = self.n.alloc(4)
        self.assertEqual(self.api('WideCharToMultiByte', 1252, 0x400, wide, 256, back, 256, 0, used), 256)
        self.assertEqual(self.n.read(back, 256), bytes(range(256)))
        self.assertEqual(self.words(used, 1), (0,))

    def test_composition_bestfit_and_default(self):
        source = self.blob('A\u0308\u221e\u2603\0'.encode('utf-16le'))
        out, used = self.n.alloc(16), self.n.alloc(4)
        self.assertEqual(self.api('WideCharToMultiByte', 1252, 0x220, source, 0xffffffff, 0, 0, 0, used), 4)
        self.assertEqual(self.api('WideCharToMultiByte', 1252, 0x220, source, 0xffffffff, out, 16, 0, used), 4)
        self.assertEqual(self.n.read(out, 4), b'\xc48?\0')
        self.assertEqual(self.words(used, 1), (1,))
        fallback = self.blob(b'!')
        self.assertEqual(self.api('WideCharToMultiByte', 1252, 0x620, source, 0xffffffff, out, 16, fallback, used), 4)
        self.assertEqual(self.n.read(out, 4), b'\xc4!!\0')
        self.assertEqual(self.api('WideCharToMultiByte', 1252, 0x220, source, 0xffffffff, out, 2, 0, used), 0)
        self.assertEqual(self.api('GetLastError'), 122)

    def test_heap_ownership_reallocation_and_destruction(self):
        process = self.api('GetProcessHeap')
        private = self.api('HeapCreate', 0, 0, 0)
        self.assertNotEqual(process, private)
        block = self.api('HeapAlloc', private, 8, 7)
        self.assertEqual(self.n.read(block, 7), b'\0' * 7)
        self.n.write(block, b'payload')
        self.assertEqual(self.api('HeapFree', process, 0, block), 0)
        self.assertEqual(self.api('HeapSize', private, 0, block), 7)
        grown = self.api('HeapReAlloc', private, 8, block, 4097)
        self.assertEqual(self.n.read(grown, 4097), b'payload' + b'\0' * 4090)
        self.assertEqual(self.api('HeapSize', process, 0, grown), 0xffffffff)
        self.assertEqual(self.api('HeapDestroy', private), 1)
        self.assertEqual(self.api('HeapSize', private, 0, grown), 0xffffffff)
        self.assertEqual(self.api('HeapDestroy', process), 0)

    def test_recursive_locks(self):
        lock = self.n.alloc(24)
        self.api('InitializeCriticalSection', lock)
        self.assertEqual(self.words(lock + 4, 3), (0xffffffff, 0, 0))
        self.api('EnterCriticalSection', lock)
        self.api('EnterCriticalSection', lock)
        self.assertEqual(self.words(lock + 4, 3), (1, 2, self.api('GetCurrentThreadId')))
        self.api('LeaveCriticalSection', lock)
        self.api('LeaveCriticalSection', lock)
        self.assertEqual(self.words(lock + 4, 3), (0xffffffff, 0, 0))
        self.api('DeleteCriticalSection', lock)
        with self.assertRaisesRegex(RuntimeError, 'uninitialized critical section'):
            self.api('EnterCriticalSection', lock)

    def test_file_unicode_io_sharing_and_path_boundary(self):
        previous = os.getcwd()
        with tempfile.TemporaryDirectory(prefix='bhl-api-', dir=ROOT / 'work') as directory:
            try:
                os.chdir(directory)
                name = self.blob('t\u00e9st.dat\0'.encode('utf-16le'))
                handle = self.api('CreateFileW', name, 0xc0000000, 0, 0, 1, 0x80, 0)
                self.assertNotEqual(handle, 0xffffffff)
                self.assertEqual(self.api('CreateFileW', name, 0x80000000, 1, 0, 3, 0x80, 0), 0xffffffff)
                self.assertEqual(self.api('GetLastError'), 32)
                payload = self.blob(b'abc\0xyz')
                count, result = self.n.alloc(8), self.n.alloc(16)
                self.assertEqual(self.api('WriteFile', handle, payload, 7, count, 0), 1)
                self.assertEqual(self.words(count, 1), (7,))
                self.assertEqual(self.api('SetFilePointerEx', handle, 0, 0, 0, 0), 1)
                self.assertEqual(self.api('ReadFile', handle, result, 16, count, 0), 1)
                self.assertEqual(self.words(count, 1), (7,))
                self.assertEqual(self.n.read(result, 7), b'abc\0xyz')
                self.assertEqual(self.api('CloseHandle', handle), 1)
                self.assertEqual(self.api('CloseHandle', handle), 0)
                escape = self.blob(b'../escape.dat\0')
                self.assertEqual(self.api('CreateFileA', escape, 0x40000000, 0, 0, 2, 0x80, 0), 0xffffffff)
                self.assertEqual(self.api('GetLastError'), 5)
            finally:
                os.chdir(previous)

    def test_unknown_behavior_is_explicit(self):
        with self.assertRaisesRegex(RuntimeError, 'unimplemented Windows behavior'):
            self.api('BhlDeliberatelyMissingAPI')

    def test_virtual_reserve_commit_protection_and_decommit(self):
        p=self.api('VirtualAlloc',0,8192,0x2000,4);self.assertNotEqual(p,0)
        old=self.n.alloc(4)
        self.assertEqual(self.api('VirtualProtect',p,4096,2,old),0)
        self.assertEqual(self.api('GetLastError'),487)
        self.assertEqual(self.api('VirtualAlloc',p+4096,4096,0x1000,4),p+4096)
        self.n.write(p+4096,b'changed')
        self.assertEqual(self.api('VirtualProtect',p+4096,4096,0x20,old),1)
        self.assertEqual(self.words(old,1),(4,))
        self.assertEqual(self.api('VirtualProtect',p+4096,4096,4,old),1)
        self.assertEqual(self.words(old,1),(0x20,))
        self.assertEqual(self.api('VirtualFree',p+4096,4096,0x4000),1)
        self.assertEqual(self.api('VirtualProtect',p+4096,4096,2,old),0)
        self.assertEqual(self.api('VirtualAlloc',p+4096,4096,0x1000,4),p+4096)
        self.assertEqual(self.n.read(p+4096,7),b'\0'*7)
        self.assertEqual(self.api('VirtualFree',p,4096,0x8000),0)
        self.assertEqual(self.api('VirtualFree',p,0,0x8000),1)

    def test_legacy_ftol_truncates_and_returns_64_bits(self):
        fn=self.n.lib.bhl_thunk(b'MSVCRT.dll!_ftol',None)
        for value,expected in [(1.9,1),(-1.9,-1),(2**40+.75,2**40),(-2**40-.75,-2**40),(float('nan'),-(2**63))]:
            self.n.context.top=0;self.n.context.st[0]=value
            low=self.n.call(fn);result=(self.n.context.edx<<32)|low
            self.assertEqual(result,expected&0xffffffffffffffff)
            self.assertEqual(self.n.context.top,1)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Services)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    (ROOT / 'work/reports/win32-contracts.json').write_text(json.dumps({
        'tests': result.testsRun, 'failures': len(result.failures),
        'errors': len(result.errors), 'passed': result.wasSuccessful(),
    }, indent=2) + '\n')
    raise SystemExit(not result.wasSuccessful())
