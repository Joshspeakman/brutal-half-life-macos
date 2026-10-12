"""Run the native headless engine and distinguish initialization from a map pass."""
import argparse
import json
import os
import re
import subprocess
from native_harness import ROOT


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--initialize-only',action='store_true')
    ap.add_argument('--map',default='c0a0')
    ap.add_argument('--seconds',type=int,default=10)
    ap.add_argument('--trace',action='store_true')
    args=ap.parse_args()
    session=ROOT/'work/session'
    name='engine-initialization' if args.initialize_only else 'map-session'
    log=ROOT/'work/reports'/f'{name}.log'
    environment=dict(os.environ,BHL_IMAGE_DIR=str(ROOT/'work/images'))
    if args.trace:environment['BHL_BRIDGE_TRACE']='1'
    command=['./xash','-game','bhl','-dev','3','-noip','-noip6','-nomaster','+sv_lan','1']
    command+=['+quit'] if args.initialize_only else ['+map',args.map]
    timeout=False
    with log.open('w') as stream:
        try:
            result=subprocess.run(command,cwd=session,env=environment,stdout=stream,stderr=subprocess.STDOUT,timeout=args.seconds)
            returncode=result.returncode
        except subprocess.TimeoutExpired:
            timeout=True;returncode=None
    text=re.sub(r'\x1b\[[0-9;]*m','',log.read_text(errors='replace'))
    stages={key:message in text for key,message in {
        'game_init':'BHL v3 GameInit completed',
        'movement_init':'BHL v3 PM_Init completed',
        'delta_registration':'BHL v3 registered all three native delta encoders',
        'map_requested':'Spawn Server: '+args.map,
        'server_activated':'BHL v3 ServerActivate completed',
        'ten_frames':'BHL v3 completed 10 server frames',
    }.items()}
    failures=[line for line in text.splitlines() if re.search('BHL.*(unimplemented|failed|not implemented)|Crash:|Xash Error:|BHL native loader:',line)]
    passed=not failures and all(stages[k] for k in ['game_init','movement_init','delta_registration'])
    if args.initialize_only:passed=passed and not timeout and returncode==0
    else:passed=passed and stages['server_activated'] and stages['ten_frames']
    report=dict(stages=stages,passed=passed,failures=failures,timeout=timeout,returncode=returncode,
                mode='initialization' if args.initialize_only else 'map',map=args.map,gameplay_validated=False)
    (log.with_suffix('.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if not passed:raise SystemExit(1)


if __name__=='__main__':main()
