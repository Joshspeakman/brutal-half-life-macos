"""Run the full engine with a null renderer to locate client integration faults.

This test cannot validate graphics, input, GUI drawing or audible output.
"""
import argparse
import json
import os
import re
import subprocess
from native_harness import ROOT


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seconds',type=int,default=15)
    ap.add_argument('--trace',action='store_true')
    args=ap.parse_args()
    log=ROOT/'work/reports/client-engine.log'
    environment=dict(os.environ,BHL_IMAGE_DIR=str(ROOT/'work/images'),SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',DYLD_LIBRARY_PATH=str(ROOT/'work/session'))
    if args.trace:environment.update(BHL_TRACE='1',BHL_BRIDGE_TRACE='1')
    command=['./xash3d','-game','bhl','-ref','null','-dev','3','-noip','-noip6','-nomaster','-console','+sv_lan','1','+map','c0a0']
    timeout=False
    with log.open('w') as stream:
        try:
            code=subprocess.run(command,cwd=ROOT/'work/session',env=environment,stdout=stream,stderr=subprocess.STDOUT,timeout=args.seconds).returncode
        except subprocess.TimeoutExpired:
            code=None;timeout=True
    text=re.sub(r'\x1b\[[0-9;]*m','',log.read_text(errors='replace'))
    stages={key:message in text for key,message in {
        'vgui_initialize':'BHL v3 VGUI App and native surface initialized',
        'client_initialize':'BHL v3 client Initialize returned 1',
        'client_movement_initialize':'BHL v3 client movement initialization completed',
        'hud_initialize':'BHL v3 HUD_Init completed',
        'server_activate':'BHL v3 ServerActivate completed',
        'server_frames':'BHL v3 completed 10 server frames',
    }.items()}
    failures=[line for line in text.splitlines() if re.search('BHL.*(unimplemented|failed|not implemented)|Crash:|Xash Error:|BHL native loader:|Host_Error:',line)]
    report=dict(stages=stages,failures=failures,returncode=code,timeout=timeout,passed=all(stages.values()) and not failures,rendering_tested=False,gameplay_validated=False)
    log.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    raise SystemExit(not report['passed'])


if __name__=='__main__':main()
