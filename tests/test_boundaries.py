"""Exercise native/guest ownership and pointer replacement without a desktop."""
import json
import os
import subprocess
from native_harness import ROOT


def main():
    out=ROOT/'work/boundary-tests';out.mkdir(exist_ok=True)
    upstream=ROOT/'work/upstream/xash3d-fwgs'
    includes=[ROOT/'runtime',ROOT/'work/bridge']+[upstream/p for p in ['common','engine','public','pm_shared','3rdparty/library_suffix/include']]
    records=[]
    for side in ['server','client']:
        exe=out/side
        command=['clang','-arch','arm64','-std=gnu11','-O2','-g','-Wno-c2x-extensions']
        for include in includes:command+=['-I',str(include)]
        command+=[str(ROOT/f'tests/{side}_boundary_fixture.c'),'-L',str(ROOT/'work/native'),'-lbhl','-Wl,-rpath,@executable_path/../native','-o',str(exe)]
        subprocess.run(command,check=True)
        result=subprocess.run([str(exe)],env=dict(os.environ,BHL_IMAGE_DIR=str(ROOT/'work/images')),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=15)
        print(result.stdout,end='')
        records.append(dict(side=side,passed=result.returncode==0,output=result.stdout))
    passed=all(r['passed'] for r in records)
    (ROOT/'work/reports/boundaries.json').write_text(json.dumps(dict(passed=passed,fixtures=records),indent=2)+'\n')
    raise SystemExit(not passed)


if __name__=='__main__':main()
