"""Run original VGUI construction, text rasterization, painting and surface restart.

The native engine drawing API is recorded in the C fixture; this is not a
visual or interactive game test. No display or clipboard is required.
"""
import json
import os
import shlex
import subprocess
from native_harness import ROOT


def main():
    executable=ROOT/'work/vgui-tests/fixture'
    executable.parent.mkdir(exist_ok=True)
    command=['clang','-arch','arm64','-std=gnu11','-O1','-g','-I',str(ROOT/'runtime'),'-I',str(ROOT/'work/upstream/xash3d-fwgs/engine')]
    command+=shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2'],text=True))
    command += [str(ROOT/'tests/vgui_fixture.c'),str(ROOT/'runtime/vgui_bridge.c'),'-L',str(ROOT/'work/native'),'-lbhl']
    command+=shlex.split(subprocess.check_output(['pkg-config','--libs','sdl2'],text=True))
    command+=['-Wl,-rpath,@executable_path/../native','-o',str(executable)]
    subprocess.run(command,check=True)
    env=dict(os.environ,BHL_IMAGE_DIR=str(ROOT/'work/images'),SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy')
    report=ROOT/'work/reports/vgui-native.json'
    report.write_text(json.dumps(dict(passed=False,status='running'))+'\n')
    result=subprocess.run([str(executable)],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=20)
    print(result.stdout)
    (ROOT/'work/reports/vgui-native.log').write_text(result.stdout)
    success=result.returncode==0 and 'VGUI lifecycle passed' in result.stdout
    report.write_text(json.dumps(dict(passed=success,original_gui=True,native_font_rasterization=True,visual_comparison_tested=False,returncode=result.returncode),indent=2)+'\n')
    raise SystemExit(not success)


if __name__=='__main__':main()
