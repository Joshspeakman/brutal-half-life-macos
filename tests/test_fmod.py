"""Run original FMOD's DirectSound initialization/workers/shutdown on native PCM.

The dummy driver keeps this test silent. It does not verify audible music.
Executable PE sections are checked for runtime modifications that the static
translation would otherwise miss.
"""
import ctypes as C
import json
import os
import subprocess
import sys
import time
import math
import struct
import wave
from native_harness import Native,ROOT


def main():
    os.environ['SDL_AUDIODRIVER']='dummy'
    n=Native(trace=bool(os.environ.get('BHL_TRACE')))
    n.lib.bhl_set_deterministic(0)
    n.attach('fmod.dll')
    pe=n.images['fmod.dll']['pe'];base=n.images['fmod.dll']['meta']['base']
    before=[(base+s.VirtualAddress,n.read(base+s.VirtualAddress,s.Misc_VirtualSize)) for s in pe.sections if s.Characteristics&0x20000000]
    exports=n.images['fmod.dll']['exports']
    def call(name,*args):return n.call(exports[name],args,stdcall=True)
    assert call('_FSOUND_SetOutput@4',2)&255==1
    assert call('_FSOUND_SetDriver@4',0)&255==1
    assert call('_FSOUND_Init@12',44100,1,0)&255==1
    error=call('_FSOUND_GetError@0')
    assert error==0,error
    assert call('_FSOUND_GetOutput@0')==2
    assert call('_FSOUND_GetOutputRate@0')==44100
    n.lib.bhl_audio_frames.restype=C.c_uint64
    first=n.lib.bhl_audio_frames();time.sleep(.15);last=n.lib.bhl_audio_frames()
    assert last>first,(first,last)
    # Exercise original file I/O, decoding, mixer workers and output together.
    fixture=ROOT/'work/fixtures/fmod-tone.wav';fixture.parent.mkdir(exist_ok=True)
    with wave.open(str(fixture),'wb') as wav:
        wav.setparams((1,2,22050,0,'NONE','not compressed'))
        wav.writeframes(b''.join(struct.pack('<h',int(12000*math.sin(2*math.pi*440*i/22050))) for i in range(22050*2)))
    n.lib.bhl_audio_nonzero_samples.restype=C.c_uint64
    streams=[]
    sources=[fixture]
    music=ROOT/'work/session/bhl/media/gamestartup.mp3'
    if music.exists():sources.append(music)
    for source in sources:
        path=str(source).encode()+b'\0';p=n.alloc(len(path));n.write(p,path)
        stream=call('_FSOUND_Stream_Open@16',p,1,0,0)
        assert stream,(source.name,call('_FSOUND_GetError@0'))
        length=call('_FSOUND_Stream_GetLengthMs@4',stream);assert length>0,(source.name,length)
        # Drain PCM left in the previous stream's output ring before measuring
        # this decoder, so a prior source cannot satisfy the nonzero assertion.
        time.sleep(.25)
        count=n.lib.bhl_audio_nonzero_samples()
        channel=call('_FSOUND_Stream_Play@8',0xffffffff,stream)
        assert channel!=0xffffffff,(source.name,call('_FSOUND_GetError@0'))
        deadline=time.monotonic()+4
        while time.monotonic()<deadline and n.lib.bhl_audio_nonzero_samples()==count:time.sleep(.05)
        decoded=n.lib.bhl_audio_nonzero_samples()-count
        assert decoded>0,(source.name,'no decoded PCM reached output')
        assert call('_FSOUND_SetVolume@8',channel,96)&255==1
        assert call('_FSOUND_GetVolume@4',channel)==96
        assert call('_FSOUND_SetPan@8',channel,192)&255==1
        assert call('_FSOUND_GetPan@4',channel)==192
        assert call('_FSOUND_SetPaused@8',channel,1)&255==1
        assert call('_FSOUND_GetPaused@4',channel)&255==1
        paused=call('_FSOUND_Stream_GetTime@4',stream)
        time.sleep(.15)
        assert call('_FSOUND_Stream_GetTime@4',stream)==paused,(source.name,'paused stream advanced')
        assert call('_FSOUND_Stream_SetTime@8',stream,500)&255==1
        seeked=call('_FSOUND_Stream_GetTime@4',stream)
        assert abs(seeked-500)<50,(source.name,seeked)
        assert call('_FSOUND_SetPaused@8',channel,0)&255==1
        assert call('_FSOUND_GetPaused@4',channel)&255==0
        time.sleep(.15)
        resumed=call('_FSOUND_Stream_GetTime@4',stream)
        assert resumed>seeked,(source.name,'resumed stream did not advance',seeked,resumed)
        assert call('_FSOUND_Stream_Stop@4',stream)&255==1
        assert call('_FSOUND_Stream_Close@4',stream)&255==1
        streams.append(dict(name=source.name,length_ms=length,nonzero_output_samples=decoded,
                            pause_resume=True,seek_ms=seeked,volume_pan=True))
    call('_FSOUND_Close@0')
    changed=[hex(p) for p,blob in before if n.read(p,len(blob))!=blob]
    assert not changed,f'Executable guest sections changed: {changed}'
    report=dict(passed=True,original_fmod_initialized=True,output='DirectSound -> native SDL PCM',rate=44100,native_frames=last-first,streams=streams,guest_code_unchanged=True,shutdown_completed=True,audible_output_tested=False)
    (ROOT/'work/reports/fmod-native.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    if '--worker' in sys.argv:main()
    else:
        report=ROOT/'work/reports/fmod-native.json'
        report.write_text(json.dumps(dict(passed=False,status='running'))+'\n')
        try:result=subprocess.run([sys.executable,__file__,'--worker'],timeout=20)
        except subprocess.TimeoutExpired:
            report.write_text(json.dumps(dict(passed=False,status='timed out'))+'\n');raise SystemExit(1)
        raise SystemExit(result.returncode)
