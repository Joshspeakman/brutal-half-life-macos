"""Independent service-contract tests through the guest calling convention."""
import json
import os
from pathlib import Path
import struct
import tempfile
import datetime
import time
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

    def test_directory_search_unicode_wildcards_metadata_and_lifetime(self):
        previous=os.getcwd()
        with tempfile.TemporaryDirectory(prefix='bhl-find-',dir=ROOT/'work') as directory:
            try:
                os.chdir(directory)
                Path('music').mkdir()
                Path('music/intro.MP3').write_bytes(b'music')
                Path('music/t\u00e9st.mp3').write_bytes(b'abc')
                Path('music/README').write_text('notes')
                Path('music/ambient').mkdir()
                output=self.n.alloc(597)
                self.n.write(output+592,b'GUARD')
                def search(pattern,op=0,flags=0):
                    return self.api('FindFirstFileExW',self.blob((pattern+'\0').encode('utf-16le')),0,output,op,0,flags)
                def name():
                    return self.n.read(output+44,520).decode('utf-16le').split('\0',1)[0]
                handle=search('music\\*.mp3')
                self.assertNotEqual(handle,0xffffffff)
                found={}
                while True:
                    found[name()]=self.words(output+28,2)
                    self.assertEqual(self.words(output,1)[0]&0x10,0)
                    self.assertGreater(struct.unpack('<Q',self.n.read(output+20,8))[0],116444736000000000)
                    if not self.api('FindNextFileW',handle,output):break
                self.assertEqual(found,{'intro.MP3':(0,5),'t\u00e9st.mp3':(0,3)})
                self.assertEqual(self.api('GetLastError'),18)
                self.assertEqual(self.n.read(output+592,5),b'GUARD')
                self.assertEqual(self.api('FindClose',handle),1)
                self.assertEqual(self.api('FindClose',handle),0)
                self.assertEqual(self.api('GetLastError'),6)
                newer=search('music/*.*')
                names={name()}
                while self.api('FindNextFileW',newer,output):names.add(name())
                self.assertTrue({'README','ambient','intro.MP3','t\u00e9st.mp3'}<=names)
                self.assertEqual(self.api('FindNextFileW',handle,output),0)
                self.assertEqual(self.api('GetLastError'),6)
                self.api('FindClose',newer)
                handle=search('music/*.mp3',flags=1)
                self.assertEqual(name(),'t\u00e9st.mp3')
                self.assertEqual(self.api('FindNextFileW',handle,output),0)
                self.api('FindClose',handle)
                handle=search('music/ambient',op=1)
                self.assertEqual(self.words(output,1)[0]&0x10,0x10)
                self.api('FindClose',handle)
                ansi=self.blob(b'music/t\xe9st.mp3\0')
                handle=self.api('FindFirstFileA',ansi,output)
                self.assertNotEqual(handle,0xffffffff)
                self.assertEqual(self.n.read(output+44,9),b't\xe9st.mp3\0')
                self.api('FindClose',handle)
                self.assertEqual(search('music/*.missing'),0xffffffff)
                self.assertEqual(self.api('GetLastError'),2)
                self.assertEqual(search('../*'),0xffffffff)
                self.assertEqual(self.api('GetLastError'),5)
                self.assertEqual(search('music/'),0xffffffff)
            finally:os.chdir(previous)

    def test_file_time_utc_and_dst_conversion(self):
        source,output=self.n.alloc(16),self.n.alloc(16)
        self.n.write(source,struct.pack('<Q',0))
        self.assertEqual(self.api('FileTimeToSystemTime',source,output),1)
        self.assertEqual(struct.unpack('<8H',self.n.read(output,16)),(1601,1,1,1,0,0,0,0))
        self.n.write(source,struct.pack('<Q',0x8000000000000000))
        self.assertEqual(self.api('FileTimeToSystemTime',source,output),0)
        self.assertEqual(self.api('GetLastError'),87)
        zone=self.n.alloc(172)
        self.n.write(zone,bytes(172))
        self.n.write(zone,struct.pack('<i',480))
        self.n.write(zone+68,struct.pack('<8H',0,11,0,1,2,0,0,0))
        self.n.write(zone+152,struct.pack('<8H',0,3,0,2,2,0,0,0))
        self.n.write(zone+168,struct.pack('<i',-60))
        for month,day,hour,minute,expected in [(3,8,9,59,(1,59)),(3,8,10,0,(3,0)),(11,1,8,59,(1,59)),(11,1,9,0,(1,0))]:
            self.n.write(source,struct.pack('<8H',2026,month,0,day,hour,minute,0,123))
            self.assertEqual(self.api('SystemTimeToTzSpecificLocalTime',zone,source,output),1)
            fields=struct.unpack('<8H',self.n.read(output,16))
            self.assertEqual(fields[4:6],expected)
            self.assertEqual(fields[7],123)
            self.assertEqual(self.api('SystemTimeToFileTime',source,output),1)
            seconds=int(datetime.datetime(2026,month,day,hour,minute,tzinfo=datetime.timezone.utc).timestamp())
            self.assertEqual(struct.unpack('<Q',self.n.read(output,8))[0],(seconds+11644473600)*10000000+1230000)
        self.n.write(source,struct.pack('<8H',2026,2,0,30,0,0,0,0))
        self.assertEqual(self.api('SystemTimeToFileTime',source,output),0)
        original=os.environ.get('TZ')
        try:
            os.environ['TZ']='America/Los_Angeles';time.tzset()
            self.n.write(source,struct.pack('<8H',2026,3,0,8,10,0,0,123))
            self.assertEqual(self.api('SystemTimeToTzSpecificLocalTime',0,source,output),1)
            self.assertEqual(struct.unpack('<8H',self.n.read(output,16))[4:6],(3,0))
            self.assertIn(self.api('GetTimeZoneInformation',zone),(1,2))
            self.assertEqual(self.words(zone,1),(480,))
            self.assertEqual(self.words(zone+168,1),(0xffffffc4,))
        finally:
            if original is None:os.environ.pop('TZ',None)
            else:os.environ['TZ']=original
            time.tzset()

    def test_crt_binary_stream_seeks_and_partial_items(self):
        def crt(name,*args):
            return self.n.call(self.n.lib.bhl_thunk(('MSVCRT.dll!'+name).encode(),None),args)
        previous=os.getcwd()
        with tempfile.TemporaryDirectory(prefix='bhl-stdio-',dir=ROOT/'work') as directory:
            try:
                os.chdir(directory)
                Path('sample.bin').write_bytes(b'\x00\r\n\x1a\xff')
                h=crt('fopen',self.blob(b'sample.bin\0'),self.blob(b'rb\0'))
                self.assertNotEqual(h,0)
                output=self.n.alloc(16)
                self.assertEqual(crt('fread',output,2,3,h),2)
                self.assertEqual(self.n.read(output,5),b'\x00\r\n\x1a\xff')
                self.assertEqual(crt('ftell',h),5)
                self.assertNotEqual(crt('feof',h),0)
                self.assertEqual(crt('fseek',h,0xfffffffe,2),0)
                self.assertEqual(crt('feof',h),0)
                self.assertEqual(crt('ftell',h),3)
                self.assertEqual(crt('fread',output,1,2,h),2)
                self.assertEqual(self.n.read(output,2),b'\x1a\xff')
                self.assertEqual(crt('fclose',h),0)
                self.assertEqual(crt('fclose',h),0xffffffff)
                self.assertEqual(self.words(crt('_errno'),1),(9,))
                self.assertEqual(crt('fopen',self.blob(b'../escape\0'),self.blob(b'wb\0')),0)
                self.assertEqual(self.words(crt('_errno'),1),(13,))
            finally:os.chdir(previous)

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
