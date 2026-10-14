"""Deliver SDL key events to a hidden engine and verify original movement.

No desktop input is generated. This validates the event/binding path, not a
physical keyboard, mouse capture, or fullscreen interaction.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
from native_harness import ROOT


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session',type=Path,required=True)
    ap.add_argument('--renderer',choices=['gl','metal'],default='metal')
    args=ap.parse_args();session=args.session.resolve()
    if not session.is_relative_to((ROOT/'work').resolve()):ap.error('Tests require isolated work/ data')
    lock=(session/'.session.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    driver=ROOT/'work/native/input-driver.dylib'
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2'],text=True))
    subprocess.run(['clang','-arch','arm64','-dynamiclib',*flags,str(ROOT/'tests/input_driver.c'),'-o',str(driver)],check=True)
    wait='wait\n'
    script='bind F1 "viewpos"\nbind F12 "echo BHL_INPUT_DONE; quit"\n'+wait*35+'viewpos\necho BHL_INPUT_READY\n'
    (session/'bhl/native-input.cfg').write_text(script)
    shutil.copy2(ROOT/'work/fixtures/bhl-config.cfg',session/'bhl/config.cfg')
    log=ROOT/'work/reports/input.log'
    saved=session/'bhl/save/quick.sav';saved.unlink(missing_ok=True)
    env=dict(os.environ,BHL_IMAGE_DIR=str(ROOT/'work/images'),SDL_VIDEODRIVER='cocoa',SDL_AUDIODRIVER='dummy',DYLD_LIBRARY_PATH=str(session),DYLD_INSERT_LIBRARIES=str(driver),BHL_TEST_INPUT_LOG=str(log))
    command=['./xash3d','-game','bhl','-ref',args.renderer,'-hidden','-windowed','-width','800','-height','600','-nomouse','-noip','-noip6','-nomaster','-nowriteconfig','-dev','3','+map','_zoo','+exec','native-input.cfg']
    if args.renderer=='metal':
        env.update(SDL_EGL_LIBRARY=str(session/'libEGL.dylib'),SDL_OPENGL_LIBRARY=str(session/'libGLESv2.dylib'),ANGLE_DEFAULT_PLATFORM='metal')
        command.insert(1,'-metal')
    with log.open('w') as output:
        p=subprocess.run(command,cwd=session,env=env,stdout=output,stderr=subprocess.STDOUT,timeout=120)
    text=re.sub(r'\x1b\[[0-9;]*m','',log.read_text(errors='replace'))
    positions=[tuple(map(float,m)) for m in re.findall(r'org \( ([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) \)',text)]
    checks=dict(normal_exit=p.returncode==0,finished='BHL_INPUT_DONE' in text,all_events_injected=len(re.findall(r'BHL SDL injected scan=\d+ down=[01]: 1',text))==24,five_positions=len(positions)==5,no_failure=not re.search(r'Crash:|BHL.*(?:unimplemented|exhausted)|Host_Error:',text),
                quicksave_written=saved.exists() and saved.stat().st_size>0,quickload_reconnected=text.count('client connected at')>=2)
    if len(positions)==5:
        checks.update(walking=sum((positions[1][i]-positions[0][i])**2 for i in (0,1))>64,jumping=positions[2][2]>positions[1][2]+8,crouching=positions[4][2]>positions[3][2]+8)
    if args.renderer=='metal':checks['metal_device_verified']='BHL Metal device verified: EGL_ANGLE_device_metal' in text
    report=dict(checks=checks,view_positions=positions,passed=all(checks.values()),physical_input_tested=False)
    (ROOT/'work/reports/input.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));raise SystemExit(not report['passed'])


if __name__=='__main__':main()
