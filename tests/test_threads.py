"""Test native worker lifetimes, guest TLS/TEBs, events and safe cancellation."""
import ctypes as C
import json
import struct
import subprocess
import time
import unittest
from native_harness import Native,ROOT


class Threads(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out=ROOT/'work/thread-tests';out.mkdir(exist_ok=True)
        library=out/'fixture.dylib'
        subprocess.run(['clang','-arch','arm64','-dynamiclib','-O1','-I',str(ROOT/'runtime'),str(ROOT/'tests/thread_fixture.c'),'-L',str(ROOT/'work/native'),'-lbhl','-Wl,-rpath,'+str(ROOT/'work/native'),'-o',str(library)],check=True)
        cls.n=Native();cls.fixture=C.CDLL(str(library))
        cls.functions={name:cls.n.lib.bhl_thunk(('test!'+name).encode(),C.cast(getattr(cls.fixture,name),C.c_void_p)) for name in ['test_tls_entry','test_wait_entry','test_critical_entry','test_cdecl_entry']}

    def api(self,name,*args):return self.n.call(self.n.lib.bhl_thunk(('KERNEL32.dll!'+name).encode(),None),args,stdcall=True)
    def words(self,p,n=1):return struct.unpack('<'+'I'*n,self.n.read(p,4*n))
    def blob(self,*words):
        p=self.n.alloc(4*len(words));self.n.write(p,struct.pack('<'+'I'*len(words),*words));return p
    def create(self,name,argument,suspended=0):
        tid=self.n.alloc(4);handle=self.api('CreateThread',0,0,self.functions[name],argument,suspended,tid);self.assertNotEqual(handle,0);return handle,self.words(tid)[0]
    def ready(self,p):
        deadline=time.monotonic()+2
        while not self.words(p)[0] and time.monotonic()<deadline:time.sleep(.001)
        self.assertEqual(self.words(p)[0],1)

    def test_separate_tebs_tls_and_suspended_start(self):
        slot=self.api('TlsAlloc');self.api('TlsSetValue',slot,0x1234)
        blocks=[self.blob(slot,100+i,0,0) for i in range(4)]
        workers=[self.create('test_tls_entry' if i%2 else 'test_cdecl_entry',p,4) for i,p in enumerate(blocks)]
        self.assertEqual([self.words(p+8)[0] for p in blocks],[0]*4)
        for h,tid in workers:self.assertEqual(self.api('ResumeThread',h),1)
        for (h,tid),p in zip(workers,blocks):
            self.assertEqual(self.api('WaitForSingleObject',h,2000),0)
            self.assertEqual(self.words(p+8,2),(self.words(p+4)[0],tid))
            result=self.n.alloc(4);self.assertEqual(self.api('GetExitCodeThread',h,result),1)
            self.assertEqual(self.words(result)[0],self.words(p+4)[0]);self.assertEqual(self.api('CloseHandle',h),1)
        self.assertEqual(len({tid for h,tid in workers}),4)
        self.assertEqual(self.api('TlsGetValue',slot),0x1234);self.api('TlsFree',slot)

    def test_event_reset_modes(self):
        event=self.api('CreateEventA',0,0,0,0)
        self.assertEqual(self.api('WaitForSingleObject',event,0),258)
        self.assertEqual(self.api('SetEvent',event),1)
        self.assertEqual(self.api('WaitForSingleObject',event,0),0)
        self.assertEqual(self.api('WaitForSingleObject',event,0),258)
        self.api('CloseHandle',event)
        event=self.api('CreateEventA',0,1,1,0)
        self.assertEqual(self.api('WaitForSingleObject',event,0),0)
        self.assertEqual(self.api('WaitForSingleObject',event,0),0)
        self.api('ResetEvent',event);self.assertEqual(self.api('WaitForSingleObject',event,0),258);self.api('CloseHandle',event)

    def test_termination_interrupts_event_wait(self):
        event=self.api('CreateEventA',0,0,0,0);p=self.blob(event,0)
        h,tid=self.create('test_wait_entry',p);self.ready(p+4)
        self.assertEqual(self.api('TerminateThread',h,77),1)
        self.assertEqual(self.api('WaitForSingleObject',h,0),0)
        out=self.n.alloc(4);self.api('GetExitCodeThread',h,out);self.assertEqual(self.words(out)[0],77)
        self.api('CloseHandle',h);self.api('CloseHandle',event)

    def test_cancellation_waits_for_critical_section_release(self):
        lock=self.n.alloc(24);self.api('InitializeCriticalSection',lock);p=self.blob(lock,0,0)
        h,tid=self.create('test_critical_entry',p);self.ready(p+4)
        self.assertEqual(self.api('TerminateThread',h,88),1)
        self.assertEqual(self.words(p+8)[0],1)
        self.assertEqual(self.words(lock+8,2),(0,0))
        self.api('EnterCriticalSection',lock);self.api('LeaveCriticalSection',lock)
        self.api('DeleteCriticalSection',lock);self.api('CloseHandle',h)


if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Threads))
    (ROOT/'work/reports/thread-contracts.json').write_text(json.dumps(dict(tests=result.testsRun,passed=result.wasSuccessful(),failures=len(result.failures),errors=len(result.errors)),indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
