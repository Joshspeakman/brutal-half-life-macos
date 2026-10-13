"""Run isolated weapon smoke-test batches and retain each batch's evidence.

This exercises commands and original events, not visual/gameplay equivalence.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
import pefile
from native_harness import ROOT


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--weapons',nargs='+',help='Default: all original exported weapon classes')
    ap.add_argument('--batch-size',type=int,default=3)
    ap.add_argument('--seconds-per-weapon',type=int,default=45)
    ap.add_argument('--soak',action='store_true',help='Use longer draw/fire/reload intervals')
    args=ap.parse_args()
    if not 1<=args.batch_size<=8:ap.error('Batch size must be 1 through 8')
    pe=pefile.PE(str(ROOT/'work/images/hl.dll'))
    available=sorted(e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name and e.name.startswith(b'weapon_'))
    selected=args.weapons or available
    if set(selected)-set(available):ap.error('Unknown original weapon export')
    libraries=['work/session/bhl/dlls/libbhl.dylib','work/session/bhl/dlls/hl_arm64.dylib','work/session/bhl/cl_dlls/client_arm64.dylib',
               'work/session/libxash.dylib','work/session/libref_gl.dylib']
    report=dict(binary_hashes={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in libraries},requested=selected,batches=[],passed=False,gameplay_validated=False)
    key=hashlib.sha256(json.dumps(report['binary_hashes'],sort_keys=True).encode()).hexdigest()[:16]
    out=ROOT/'work/reports/weapons'/key;out.mkdir(parents=True,exist_ok=True)
    for start in range(0,len(selected),args.batch_size):
        batch=selected[start:start+args.batch_size];key='-'.join(w.removeprefix('weapon_') for w in batch)
        print(f'Weapon batch: {", ".join(batch)}',flush=True);began=time.monotonic()
        command=[sys.executable,str(ROOT/'tests/test_client_engine.py'),'--renderer','gl','--video-driver','cocoa','--map','_zoo','--save-load','--capture-frames','25','--seconds',str(45+args.seconds_per_weapon*len(batch)),'--weapons',*batch]
        if args.soak:command.append('--weapon-soak')
        with (out/(key+'.console.log')).open('w') as stream:
            result=subprocess.run(command,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT)
        source=ROOT/'work/reports/client-engine.json';record=json.loads(source.read_text());record['wall_seconds']=round(time.monotonic()-began,2)
        record['passed']=record['passed'] and result.returncode==0
        (out/(key+'.json')).write_text(json.dumps(record,indent=2)+'\n');shutil.copy2(ROOT/'work/reports/client-engine.log',out/(key+'.log'))
        report['batches'].append(record);report['passed']=all(r['passed'] for r in report['batches']) and start+len(batch)==len(selected)
        (out/'matrix.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f'Batch {"passed" if record["passed"] else "failed"}: {key} ({record["wall_seconds"]} seconds)',flush=True)
        if not record['passed']:raise SystemExit(1)


if __name__=='__main__':main()
