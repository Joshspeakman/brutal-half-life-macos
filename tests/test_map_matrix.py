"""Run native activation, simulation and capture checks for every shipped map.

This covers initial map execution, not full campaign/script completion.
Reports are keyed by the tested binaries, so rebuilds cannot reuse old passes.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import time
from native_harness import ROOT
from pathlib import Path


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session',type=Path,default=ROOT/'work/session')
    ap.add_argument('--maps',nargs='+')
    ap.add_argument('--seconds',type=int,default=90)
    ap.add_argument('--resume',action='store_true')
    ap.add_argument('--renderer',choices=['gl','metal'],default='metal')
    args=ap.parse_args()
    session=args.session.resolve()
    if not session.is_relative_to((ROOT/'work').resolve()):ap.error('Tests require a session inside work/')
    mod=session/'bhl'
    available={p.stem:p for p in (mod/'maps').glob('*.bsp')}
    selected=args.maps or sorted(available)
    if set(selected)-set(available) or any(not re.fullmatch('[A-Za-z0-9_-]+',m) for m in selected):ap.error('Unknown map')
    binaries=['work/session/bhl/dlls/libbhl.dylib','work/session/bhl/dlls/hl_arm64.dylib','work/session/bhl/cl_dlls/client_arm64.dylib',
              'work/session/libxash.dylib',f'work/session/libref_{args.renderer}.dylib']
    if args.renderer=='metal':binaries += ['work/session/libEGL.dylib','work/session/libGLESv2.dylib']
    hashes={p:hashlib.sha256((session/p.removeprefix('work/session/')).read_bytes()).hexdigest() for p in binaries}
    key=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()[:16]
    out=ROOT/'work/reports/maps'/key;out.mkdir(parents=True,exist_ok=True)
    report=dict(binary_hashes=hashes,requested=selected,maps={},passed=False,full_gameplay_validated=False)
    for name in selected:
        record_file=out/(name+'.json')
        if args.resume and record_file.exists():
            record=json.loads(record_file.read_text())
            if record['passed']:report['maps'][name]=record;continue
        print('Map:',name,flush=True);began=time.monotonic()
        command=[sys.executable,str(ROOT/'tests/test_client_engine.py'),'--renderer',args.renderer,'--video-driver','cocoa',
                 '--session',str(session),'--report-dir',str(out),'--map',name,'--capture','--quit-after-frames','35','--seconds',str(args.seconds)]
        with (out/(name+'.console.log')).open('w') as stream:
            result=subprocess.run(command,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT)
        source=out/'client-engine.json';record=json.loads(source.read_text())
        record['passed']=record['passed'] and result.returncode==0
        record['wall_seconds']=round(time.monotonic()-began,2)
        raw=available[name].read_bytes();offset,size=struct.unpack_from('<ii',raw,4)
        record['entity_classes']=sorted(set(re.findall(r'"classname"\s*"([^"]+)"',raw[offset:offset+size].decode(errors='replace'))))
        record['map_sha256']=hashlib.sha256(raw).hexdigest()
        record_file.write_text(json.dumps(record,indent=2)+'\n')
        shutil.copy2(out/'client-engine.log',out/(name+'.log'))
        if record['capture']:shutil.copy2(ROOT/record['capture'],out/(name+'.png'))
        report['maps'][name]=record
        report['passed']=len(report['maps'])==len(selected) and all(r['passed'] for r in report['maps'].values())
        (out/'matrix.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f'{name}: {"passed" if record["passed"] else "failed"} ({record["wall_seconds"]} seconds)',flush=True)
        if not record['passed']:raise SystemExit(1)
    report['passed']=all(r['passed'] for r in report['maps'].values())
    (out/'matrix.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Map smoke tests passed:',len(selected),flush=True)


if __name__=='__main__':main()
