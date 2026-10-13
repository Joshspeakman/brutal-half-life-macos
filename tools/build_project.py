"""Reproduce the current native development build from the user's own inputs.

This builds all four modules and both bridges into an isolated session.
Use install.sh to package a local application after the build. Checks cover
bounded integration; they do not establish complete campaign equivalence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import pefile
from inputs import ROOT

MODULES=['hl.dll','client.dll','fmod.dll','vgui.dll']


def digest(paths):
    h=hashlib.sha256()
    for path in paths:
        h.update(str(path.relative_to(ROOT)).encode());h.update(path.read_bytes())
    return h.hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive',type=Path,required=True)
    ap.add_argument('--valve',type=Path,help='Owned Half-Life valve data, required for engine tests')
    ap.add_argument('--check',action='store_true',help='Run service, startup differential and server map checks')
    ap.add_argument('--check-client',action='store_true',help='Also run the bounded Metal client save/load acceptance test')
    args=ap.parse_args()
    if (args.check or args.check_client) and not args.valve:ap.error('--valve is required for engine tests')
    archive=args.archive.expanduser().resolve()
    reports=ROOT/'work/reports';reports.mkdir(parents=True,exist_ok=True)
    def run(name,script,*arguments):
        log=reports/('build-'+name+'.log')
        print(f'{name}: {log.relative_to(ROOT)}',flush=True)
        with log.open('w') as stream:
            result=subprocess.run([sys.executable,script,*map(str,arguments)],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT)
        if result.returncode:raise RuntimeError(f'{name} failed; inspect {log.relative_to(ROOT)}')
    run('prepare','tools/prepare.py',archive)
    run('dependencies','tools/fetch_dependencies.py','--build-engine','--build-client-engine')
    run('unpack','tools/unpack_audio.py')
    run('relocate','tools/relocate.py')
    tool_inputs=[ROOT/'tools/lift_module.py']+sorted((ROOT/'vendor/lifter').glob('*.py'))
    cache=ROOT/'work/lift-cache.json'
    old=json.loads(cache.read_text()) if cache.exists() else {}
    current={};first=0
    for module in MODULES:
        binary=ROOT/'work/images'/module
        fingerprint=digest([binary,*tool_inputs])+f':{first}'
        metadata=ROOT/'work/lifted'/module/'image.json'
        analysis=Path('work/reports')/(module+'-analysis.json')
        if old.get(module)!=fingerprint or not metadata.exists():
            run('analyze-'+module,'vendor/lifter/analyze.py',binary,analysis)
            run('lift-'+module,'tools/lift_module.py',module,analysis,'--prepared','--imp-base',first)
        meta=json.loads(metadata.read_text())
        if meta['sha256']!=hashlib.sha256(binary.read_bytes()).hexdigest() or meta['imp_base']!=first:
            raise RuntimeError(f'{module}: stale translation metadata')
        current[module]=fingerprint
        first+=sum(len(d.imports) for d in pefile.PE(str(binary)).DIRECTORY_ENTRY_IMPORT)
    cache.write_text(json.dumps(current,indent=2)+'\n')
    run('runtime','tools/build_runtime.py',*MODULES)
    run('server-bridge','tools/build_bridge.py')
    run('client-bridge','tools/build_client_bridge.py')
    if args.valve:
        run('session','tools/prepare_session.py','--archive',archive,'--valve',args.valve.expanduser().resolve(),'--client')
    if args.check:
        run('interfaces-build','tools/build_probe.py')
        run('interfaces','tests/test_interfaces.py')
        run('services','tests/test_win32.py')
        run('audio','tests/test_audio.py')
        run('threads','tests/test_threads.py')
        run('fmod','tests/test_fmod.py')
        run('vgui','tests/test_vgui.py')
        run('boundaries','tests/test_boundaries.py')
        for module in MODULES:run('differential-'+module,'tests/test_startup_differential.py','--module',module)
        run('server-map','tests/test_engine.py','--map','c0a0')
    if args.check_client:run('client-engine','tests/test_client_engine.py','--renderer','metal','--video-driver','cocoa','--map','_zoo','--save-load','--capture-frames','25','--seconds','90')
    print('Native development build complete. Full campaign validation remains separate; use tools/package_app.py to package this local build.')


if __name__=='__main__':main()
