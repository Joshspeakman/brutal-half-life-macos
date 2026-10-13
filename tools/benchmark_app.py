"""Measure real frame intervals in an isolated, fixed-timestep game workload.

The presentation probe preserves the renderer and all game effects.
Uncapped measurements are capacity tests, not a claim of sustained playthrough
performance. This tool refuses user data outside the ignored work directory.
"""
import argparse
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path
import selectors
import signal
import subprocess
import time
from inputs import ROOT


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--app',type=Path,required=True)
    ap.add_argument('--data',type=Path,default=ROOT/'work/app-data')
    ap.add_argument('--client',type=Path);ap.add_argument('--server',type=Path);ap.add_argument('--core',type=Path)
    ap.add_argument('--engine',type=Path)
    ap.add_argument('--map',default='start');ap.add_argument('--frames',type=int,default=1200)
    ap.add_argument('--width',type=int,default=1280);ap.add_argument('--height',type=int,default=720)
    ap.add_argument('--fps-cap',type=int,default=1000);ap.add_argument('--vsync',action='store_true')
    ap.add_argument('--attack',action='store_true');ap.add_argument('--bridge-profile',action='store_true')
    ap.add_argument('--capture',action='store_true',help='Capture the scene after timing stops')
    ap.add_argument('--name',required=True);ap.add_argument('--seconds',type=int,default=180)
    args=ap.parse_args()
    data=args.data.resolve();resources=args.app.resolve()/'Contents/Resources';runtime=resources/'Runtime'
    if not data.is_relative_to(ROOT/'work'):ap.error('Use isolated data under work/')
    if not args.map.replace('_','').isalnum():ap.error('Invalid map name')
    if not 10<=args.frames<=100000:ap.error('Use 10..100000 frames')
    if not 1<=args.fps_cap<=1000:ap.error('Use a frame limit from 1 through 1000')
    if Path(args.name).name!=args.name:ap.error('Name must be a filename component')
    out=ROOT/'work/reports'/args.name;out.parent.mkdir(parents=True,exist_ok=True)
    gate=out.with_suffix('.armed');timings=out.with_suffix('.frames.json')
    gate.unlink(missing_ok=True);timings.unlink(missing_ok=True)
    lock=(data/'.game.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    probe=ROOT/'work/native/frame-times.dylib'
    staged=probe.with_suffix('.stage.dylib')
    subprocess.run(['clang','-arch','arm64','-O2','-dynamiclib',str(ROOT/'tests/frame_times.c'),
                    '-L',str(runtime),'-lSDL2-2.0.0','-o',str(staged)],check=True)
    subprocess.run(['codesign','--force','--sign','-',str(staged)],check=True,capture_output=True)
    staged.replace(probe)
    script=data/'bhl/native-benchmark.cfg'
    if script.exists():raise RuntimeError('A previous benchmark script needs review')
    wait='wait\n'
    setup='sv_cheats 1\ngod\n'
    if args.attack:setup+='impulse 101\n'+wait*10+'weapon_9mmAR\n'+wait*30+'+attack\n'
    capture=data/'bhl/scrshots/native-benchmark.png'
    if args.capture:capture.unlink(missing_ok=True)
    screenshot=wait*3+'snapshot scrshots/native-benchmark.png\n'+wait*3 if args.capture else ''
    script.write_text('scr_conspeed 100000\ndeveloper 0\n'+wait*120+setup+wait*30+
                      'echo BHL_BENCH_BEGIN\n'+wait*args.frames+'echo BHL_BENCH_END\n-attack\n'+screenshot+'quit\n')
    env={k:v for k,v in os.environ.items() if not k.startswith(('BHL_','SDL_','DYLD_','XASH3D_'))}
    overrides=[p.resolve().parent for p in (args.core,args.client,args.server,args.engine) if p]
    env.update(BHL_IMAGE_DIR=str(resources/'Images'),XASH3D_BASEDIR=str(data),XASH3D_EXTRAS_PAK1=str(resources/'extras.pk3'),
               DYLD_LIBRARY_PATH=':'.join(map(str,dict.fromkeys(overrides+[runtime]))),DYLD_PRINT_LIBRARIES='1',
               DYLD_INSERT_LIBRARIES=str(probe),SDL_AUDIODRIVER='dummy',BHL_BENCH_GATE=str(gate),BHL_BENCH_TIMES=str(timings))
    if args.bridge_profile:env['BHL_BRIDGE_PROFILE']='1'
    command=[str(runtime/'xash3d'),'-game','bhl','-ref','gl','-dll',str((args.server or runtime/'hl_arm64.dylib').resolve()),
             '-clientlib',str((args.client or runtime/'client_arm64.dylib').resolve()),'-menulib',str(runtime/'libmenu.dylib'),
             '-hidden','-nomouse','-windowed','-width',str(args.width),'-height',str(args.height),'-noip','-noip6','-nomaster',
             '-nowriteconfig','-dev','3','+sv_lan','1','+cl_lw','0','+fps_max',str(args.fps_cap),'+gl_vsync',str(int(args.vsync)),
             '+host_framerate','0.008333333','+map',args.map,'+exec',script.name]
    began=time.monotonic();marks={};timeout=False;pending=b''
    report=dict(map=args.map,frames=args.frames,logical_size=[args.width,args.height],fixed_simulation_step=1/120,
                fps_cap=args.fps_cap,vsync=args.vsync,audio='dummy',attack=args.attack,profiling=args.bridge_profile,
                libraries={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in
                           [args.core or runtime/'libbhl.dylib',args.client or runtime/'client_arm64.dylib',args.server or runtime/'hl_arm64.dylib',args.engine or runtime/'libxash.dylib']})
    p=None
    try:
        p=subprocess.Popen(command,cwd=data,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
        with selectors.DefaultSelector() as sel,out.with_suffix('.log').open('wb') as log:
            sel.register(p.stdout,selectors.EVENT_READ)
            while sel.get_map():
                if time.monotonic()-began>args.seconds:
                    timeout=True;os.killpg(p.pid,signal.SIGKILL);break
                for key,_ in sel.select(0.25):
                    chunk=os.read(key.fd,65536)
                    if not chunk:sel.unregister(key.fd);continue
                    log.write(chunk);pending+=chunk
                    while b'\n' in pending:
                        line,pending=pending.split(b'\n',1)
                        for marker in ('BHL_BENCH_BEGIN','BHL_BENCH_END'):
                            if marker.encode() in line:
                                marks[marker]=time.monotonic()
                                if marker.endswith('BEGIN'):gate.touch()
                                else:gate.unlink(missing_ok=True)
            log.write(pending)
        report.update(returncode=p.wait(timeout=10),timeout=timeout)
    finally:
        if p and p.poll() is None:os.killpg(p.pid,signal.SIGKILL);p.wait()
        gate.unlink(missing_ok=True);script.unlink()
    values=json.loads(timings.read_text()) if timings.exists() else []
    log=re.sub(r'\x1b\[[0-9;]*m','',out.with_suffix('.log').read_text(errors='replace'))
    loaded={Path(p).name:Path(p).resolve() for p in re.findall(r'dyld\[\d+\]: <[^>]+> (.+)',log)}
    size=re.search(r'Window size: (\d+)x(\d+) \(real (\d+)x(\d+)\)',log)
    report['drawable_size']=[int(size[3]),int(size[4])] if size else None
    if args.capture:
        if capture.exists():
            target=out.with_suffix('.png');target.write_bytes(capture.read_bytes());report['capture']=str(target.relative_to(ROOT))
        else:report['capture']=None
    expected=[args.core or runtime/'libbhl.dylib',args.client or runtime/'client_arm64.dylib',args.server or runtime/'hl_arm64.dylib',args.engine or runtime/'libxash.dylib']
    report['library_paths_verified']=all(loaded.get(p.name)==p.resolve() for p in expected)
    report['runtime_failures']=[line for line in log.splitlines() if re.search(r'BHL.*(unimplemented|not implemented)|Crash:|Xash Error:|BHL native loader:|Host_Error:',line)]
    report['god_enabled']='godmode ON' in log
    report['presented_intervals']=len(values)
    report['passed']=report['returncode']==0 and not timeout and len(marks)==2 and abs(len(values)-args.frames)<=4 and report['library_paths_verified'] and not report['runtime_failures'] and report['god_enabled']
    if args.capture:report['passed']=report['passed'] and bool(report['capture'])
    if len(marks)==2:report['marker_fps']=args.frames/(marks['BHL_BENCH_END']-marks['BHL_BENCH_BEGIN'])
    if values:
        ordered=sorted(values);n=len(ordered)
        report.update(fps=1000*n/sum(values),frame_ms_p50=ordered[int(n*0.50)],frame_ms_p95=ordered[min(n-1,int(n*0.95))],
                      frame_ms_p99=ordered[min(n-1,int(n*0.99))],frame_ms_max=max(values),
                      one_percent_low_fps=1000/(sum(ordered[-max(1,n//100):])/max(1,n//100)))
    out.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2),flush=True)
    raise SystemExit(not report['passed'])


if __name__=='__main__':main()
