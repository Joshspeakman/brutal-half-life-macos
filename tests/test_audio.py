"""Exercise guest DirectSound COM -> native PCM mixing without audible output."""
import ctypes as C
import json
import os
import struct
import unittest
from native_harness import Native, ROOT


class Audio(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ['SDL_AUDIODRIVER']='dummy'
        cls.n=Native()
        cls.n.lib.bhl_dsound_export.argtypes=[C.c_char_p]
        cls.n.lib.bhl_dsound_export.restype=C.c_uint32
        cls.n.lib.bhl_audio_test_render.argtypes=[C.POINTER(C.c_float),C.c_uint]

    def call(self,obj,method,*args):
        table=struct.unpack('<I',self.n.read(obj,4))[0]
        fn=struct.unpack('<I',self.n.read(table+4*method,4))[0]
        return self.n.call(fn,[obj,*args],stdcall=True)

    def setUp(self):
        out=self.n.alloc(4)
        result=self.n.call(self.n.lib.bhl_dsound_export(b'DirectSoundCreate'),[0,out,0],stdcall=True)
        self.assertEqual(result,0)
        self.device=struct.unpack('<I',self.n.read(out,4))[0]
        self.n.lib.bhl_audio_test_pause()
        self.buffers=[]

    def tearDown(self):
        for buffer in self.buffers:self.assertEqual(self.call(buffer,2),0)
        self.assertEqual(self.call(self.device,2),0)

    def buffer(self,pcm,channels=2,bits=16,rate=44100,flags=0xe8):
        fmt=self.n.alloc(18);align=channels*bits//8
        self.n.write(fmt,struct.pack('<HHIIHHH',1,channels,rate,rate*align,align,bits,0))
        desc=self.n.alloc(20);self.n.write(desc,struct.pack('<5I',20,flags,len(pcm),0,fmt))
        out=self.n.alloc(4)
        self.assertEqual(self.call(self.device,3,desc,out,0),0)
        buffer=struct.unpack('<I',self.n.read(out,4))[0];self.buffers.append(buffer)
        lock=self.n.alloc(16)
        self.assertEqual(self.call(buffer,11,0,len(pcm),lock,lock+4,lock+8,lock+12,0),0)
        p,n,p2,n2=struct.unpack('<4I',self.n.read(lock,16))
        self.assertEqual((n,p2,n2),(len(pcm),0,0));self.n.write(p,pcm)
        self.assertEqual(self.call(buffer,19,p,n,p2,n2),0)
        return buffer

    def render(self,frames):
        output=(C.c_float*(frames*2))();self.n.lib.bhl_audio_test_render(output,frames);return list(output)

    def test_pcm_samples_loop_and_stop(self):
        buffer=self.buffer(struct.pack('<4h',16384,-16384,8192,-8192))
        self.assertEqual(self.call(buffer,12,0,0,1),0)
        self.assertEqual(self.render(3),[.5,-.5,.25,-.25,.5,-.5])
        position=self.n.alloc(8);self.assertEqual(self.call(buffer,4,position,position+4),0)
        self.assertEqual(struct.unpack('<I',self.n.read(position,4))[0],4)
        self.assertEqual(self.call(buffer,18),0)
        self.assertEqual(self.render(3),[0]*6)

    def test_volume_pan_and_frequency(self):
        buffer=self.buffer(struct.pack('<8h',16384,16384,8192,8192,0,0,-8192,-8192))
        self.assertEqual(self.call(buffer,15,(-2000)&0xffffffff),0)
        self.assertEqual(self.call(buffer,16,10000),0)
        self.assertEqual(self.call(buffer,17,88200),0)
        self.assertEqual(self.call(buffer,12,0,0,1),0)
        output=self.render(2)
        self.assertAlmostEqual(output[0],0.0000005,places=7)
        self.assertAlmostEqual(output[1],0.05,places=6)
        self.assertEqual(output[2:],[0,0])

    def test_unsigned_mono_and_wrapping_lock(self):
        buffer=self.buffer(bytes([128,192,64,255]),channels=1,bits=8)
        self.assertEqual(self.call(buffer,12,0,0,1),0)
        self.assertEqual(self.render(3),[0,0,.5,.5,-.5,-.5])
        lock=self.n.alloc(16)
        self.assertEqual(self.call(buffer,11,3,3,lock,lock+4,lock+8,lock+12,0),0)
        p,n,p2,n2=struct.unpack('<4I',self.n.read(lock,16))
        self.assertEqual((n,n2),(1,2));self.assertEqual(p-p2,3)
        self.assertEqual(self.call(buffer,19,p,n,p2,n2),0)

    def test_invalid_format_and_unavailable_controls(self):
        buffer=self.buffer(bytes(8),flags=8)
        self.assertEqual(self.call(buffer,15,0),0x8878001e)
        self.assertEqual(self.call(buffer,13,8),0x80070057)


if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Audio))
    (ROOT/'work/reports/audio-contracts.json').write_text(json.dumps(dict(tests=result.testsRun,passed=result.wasSuccessful(),failures=len(result.failures),errors=len(result.errors),audible_output_tested=False),indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
