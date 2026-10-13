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
    call('_FSOUND_Close@0')
    changed=[hex(p) for p,blob in before if n.read(p,len(blob))!=blob]
    assert not changed,f'Executable guest sections changed: {changed}'
    report=dict(passed=True,original_fmod_initialized=True,output='DirectSound -> native SDL PCM',rate=44100,native_frames=last-first,guest_code_unchanged=True,shutdown_completed=True,audible_output_tested=False)
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
