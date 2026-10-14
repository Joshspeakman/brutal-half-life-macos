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


def percentiles(values):
    if not values:return None
    ordered=sorted(values);n=len(ordered)
    return {k:round(ordered[min(n-1,int(n*q))],3) for k,q in (('p50',.5),('p95',.95),('p99',.99))}|{'max':round(ordered[-1],3),'mean':round(sum(values)/n,3)}


def summarize_present(present):
    """Separate CPU work, swap blocking, drawable waits and on-glass cadence."""
    rows=present['frames']
    work=[(b[0]-a[1])*1000 for a,b in zip(rows,rows[1:])]
    shown=[r[4] for r in rows if r[4]>0]
    glass=[(b-a)*1000 for a,b in zip(shown,shown[1:])]
    latency=[(r[4]-r[1])*1000 for r in rows if r[4]>0]
    vblank=1000/120
    return dict(frames=len(rows),dropped=sum(1 for r in rows if r[4]<=0),display_sync_calls=present['display_sync_calls'],
                cpu_between_swaps_ms=percentiles(work),swap_ms=percentiles([(r[1]-r[0])*1000 for r in rows]),
                next_drawable_wait_ms=percentiles([r[2]*1000 for r in rows]),presented_interval_ms=percentiles(glass),
                swap_to_glass_ms=percentiles(latency),
                gpu_busy_ms=percentiles([r[7]*1000 for r in rows if len(r)>7 and r[7]>0]),
                gpu_end_to_glass_ms=percentiles([(r[4]-r[6])*1000 for r in rows if len(r)>7 and r[4]>0 and r[6]>0]),
                swap_to_gpu_end_ms=percentiles([(r[6]-r[1])*1000 for r in rows if len(r)>7 and r[6]>0]),
                command_buffers=percentiles([r[8] for r in rows if len(r)>8]),
                presented_vblanks={str(k):sum(1 for g in glass if round(g/vblank)==k) for k in range(0,6)})


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--app',type=Path,required=True)
    ap.add_argument('--data',type=Path,default=ROOT/'work/app-data')
    ap.add_argument('--client',type=Path);ap.add_argument('--server',type=Path);ap.add_argument('--core',type=Path)
    ap.add_argument('--engine',type=Path)
    ap.add_argument('--renderer',choices=['gl','metal'],default='metal')
    ap.add_argument('--map',default='start');ap.add_argument('--frames',type=int,default=1200)
    ap.add_argument('--width',type=int,default=1280);ap.add_argument('--height',type=int,default=720)
    ap.add_argument('--fps-cap',type=int,default=1000);ap.add_argument('--vsync',action='store_true')
    ap.add_argument('--fullscreen',action='store_true',help='Show a native fullscreen window for an interactive-display measurement')
    ap.add_argument('--audio',choices=['dummy','coreaudio'],default='dummy',help='Use CoreAudio for a measurement with real device output')
    ap.add_argument('--real-time',action='store_true',help='Use normal elapsed simulation time instead of the matched fixed step')
    ap.add_argument('--attack',action='store_true');ap.add_argument('--bridge-profile',action='store_true')
    ap.add_argument('--verify-entities',action='store_true',help='Audit untouched mirrors for untracked writes; not a timing run')
    ap.add_argument('--eager-entities',action='store_true',help='Use complete mirror scans as a synchronization comparison control')
    ap.add_argument('--capture',action='store_true',help='Capture the scene after timing stops')
    ap.add_argument('--present-trace',action='store_true',help='Record swap, nextDrawable waits and Metal presented times')
    ap.add_argument('--max-drawables',type=int,choices=[2,3],help='Presentation experiment: override CAMetalLayer maximumDrawableCount')
    ap.add_argument('--display-sync',type=int,choices=[0,1],help='Presentation experiment: override CAMetalLayer displaySyncEnabled')
    ap.add_argument('--sample',action='store_true',help='With --present-trace, sample engine-thread stacks in process (changes timing slightly)')
    ap.add_argument('--env',action='append',default=[],metavar='BHL_NAME=VALUE',help='Pass a BHL_ diagnostic option to the game process')
    ap.add_argument('--cvar',action='append',default=[],metavar='NAME=VALUE',help='Set an engine cvar at launch')
    ap.add_argument('--no-spaces',action='store_true',help='Experiment: SDL fullscreen without a macOS fullscreen Space')
    ap.add_argument('--mouse',action='store_true',help='Leave mouse input active (captures the pointer while the game is focused)')
    ap.add_argument('--command',action='append',default=[],metavar='CONSOLE',help='Console command run after setup (repeatable; "wait N" waits N frames)')
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
    if (args.max_drawables or args.display_sync is not None) and not args.present_trace:ap.error('Layer overrides require --present-trace')
    trace=out.with_suffix('.present.json');trace.unlink(missing_ok=True)
    probe=ROOT/('work/native/present-probe.dylib' if args.present_trace else 'work/native/frame-times.dylib')
    staged=probe.with_suffix('.stage.dylib')
    source=['tests/present_probe.m','-fobjc-arc','-framework','Foundation','-framework','QuartzCore','-framework','Metal'] if args.present_trace else ['tests/frame_times.c']
    subprocess.run(['clang','-arch','arm64','-O2','-dynamiclib',str(ROOT/source[0]),*source[1:],
                    '-L',str(runtime),'-lSDL2-2.0.0','-o',str(staged)],check=True)
    subprocess.run(['codesign','--force','--sign','-',str(staged)],check=True,capture_output=True)
    staged.replace(probe)
    script=data/'bhl/native-benchmark.cfg'
    if script.exists():raise RuntimeError('A previous benchmark script needs review')
    wait='wait\n'
    setup='sv_cheats 1\ngod\n'
    if args.attack:setup+='impulse 101\n'+wait*10+'weapon_9mmAR\n'+wait*30+'+attack\n'
    for command in args.command:
        if '\n' in command or ';' in command:ap.error('--command takes one console command')
        setup+=command+'\n'
    capture=data/'bhl/scrshots/native-benchmark.png'
    if args.capture:capture.unlink(missing_ok=True)
    screenshot=wait*3+'snapshot scrshots/native-benchmark.png\n'+wait*3 if args.capture else ''
    # Xash accepts a frame count for wait. Keep the original short-run script
    # for matched comparisons, but avoid overflowing its 32 KiB command buffer
    # when a soak requests many thousands of frames.
    measured_wait=wait*args.frames if args.frames<=4096 else f'wait {args.frames}\n'
    script.write_text('scr_conspeed 100000\ndeveloper 0\n'+wait*120+setup+wait*30+
                      'vid_info\necho BHL_BENCH_BEGIN\n'+measured_wait+'echo BHL_BENCH_END\n-attack\n'+screenshot+'quit\n')
    env={k:v for k,v in os.environ.items() if not k.startswith(('BHL_','SDL_','DYLD_','XASH3D_'))}
    overrides=[p.resolve().parent for p in (args.core,args.client,args.server,args.engine) if p]
    env.update(BHL_IMAGE_DIR=str(resources/'Images'),XASH3D_BASEDIR=str(data),XASH3D_EXTRAS_PAK1=str(resources/'extras.pk3'),
               DYLD_LIBRARY_PATH=':'.join(map(str,dict.fromkeys(overrides+[runtime]))),DYLD_PRINT_LIBRARIES='1',
               DYLD_INSERT_LIBRARIES=str(probe),SDL_AUDIODRIVER=args.audio,BHL_BENCH_GATE=str(gate),BHL_BENCH_TIMES=str(timings))
    env['SDL_VIDEO_MAC_FULLSCREEN_SPACES']='0' if args.no_spaces else '1'
    if args.present_trace:env['BHL_PRESENT_TRACE']=str(trace)
    if args.sample:env['BHL_PROBE_SAMPLES']=str(out.with_suffix('.samples.json'))
    if args.max_drawables:env['BHL_PROBE_MAX_DRAWABLES']=str(args.max_drawables)
    if args.display_sync is not None:env['BHL_PROBE_DISPLAY_SYNC']=str(args.display_sync)
    if args.bridge_profile:env['BHL_BRIDGE_PROFILE']='1'
    for option in args.env:
        key,_,value=option.partition('=')
        if not key.startswith('BHL_') or not key[4:].replace('_','').isalnum():ap.error('--env accepts BHL_ options only')
        env[key]=value
    if args.eager_entities:env['BHL_EAGER_ENTITIES']='1'
    if args.verify_entities:env['BHL_VERIFY_ENTITY_ACCESS']='1'
    if args.renderer=='metal':
        env.update(SDL_EGL_LIBRARY=str(runtime/'libEGL.dylib'),SDL_OPENGL_LIBRARY=str(runtime/'libGLESv2.dylib'),ANGLE_DEFAULT_PLATFORM='metal')
    command=[str(args.app.resolve()/'Contents/MacOS/BrutalHalfLife'),'--run-engine','-game','bhl','-ref',args.renderer,'-dll',str((args.server or runtime/'hl_arm64.dylib').resolve()),
             '-clientlib',str((args.client or runtime/'client_arm64.dylib').resolve()),'-menulib',str(runtime/'libmenu.dylib'),
             *(['-borderless'] if args.fullscreen else ['-hidden','-windowed']),*([] if args.mouse else ['-nomouse']),'-width',str(args.width),'-height',str(args.height),'-noip','-noip6','-nomaster',
             '-nowriteconfig','-dev','3','+sv_lan','1','+cl_lw','0','+fps_max',str(args.fps_cap),'+gl_vsync',str(int(args.vsync)),
             '+host_framerate','0' if args.real_time else '0.008333333',
             *[part for option in args.cvar for part in ('+'+option.partition('=')[0],option.partition('=')[2])],'+map',args.map,'+exec',script.name]
    if args.renderer=='metal':command.insert(2,'-metal')
    began=time.monotonic();marks={};timeout=False;pending=b''
    expected=[args.core or runtime/'libbhl.dylib',args.client or runtime/'client_arm64.dylib',
              args.server or runtime/'hl_arm64.dylib',args.engine or runtime/'libxash.dylib',
              runtime/f'libref_{args.renderer}.dylib']
    if args.renderer=='metal':expected += [runtime/'libEGL.dylib',runtime/'libGLESv2.dylib']
    report=dict(map=args.map,renderer=args.renderer,frames=args.frames,logical_size=[args.width,args.height],fixed_simulation_step=None if args.real_time else 1/120,
                fps_cap=args.fps_cap,vsync=args.vsync,commands=args.command,mouse=args.mouse,fullscreen_spaces=not args.no_spaces,cvars=args.cvar,diagnostic_env=args.env,present_trace=args.present_trace,max_drawables=args.max_drawables,display_sync=args.display_sync,fullscreen=args.fullscreen,audio=args.audio,attack=args.attack,profiling=args.bridge_profile,eager_entities=args.eager_entities,verify_entities=args.verify_entities,
                libraries={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in expected})
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
    if args.present_trace and trace.exists():
        present=json.loads(trace.read_text());rows=present['frames']
        values=[(b[1]-a[1])*1000 for a,b in zip(rows,rows[1:])]
        timings.write_text(json.dumps(values)+'\n')
        report['present']=summarize_present(present)
    log=re.sub(r'\x1b\[[0-9;]*m','',out.with_suffix('.log').read_text(errors='replace'))
    loaded={Path(p).name:Path(p).resolve() for p in re.findall(r'dyld\[\d+\]: <[^>]+> (.+)',log)}
    sizes=re.findall(r'Window size: (\d+)x(\d+) \(real (\d+)x(\d+)\)',log)
    report['drawable_size']=[int(sizes[-1][2]),int(sizes[-1][3])] if sizes else None
    if args.capture:
        if capture.exists():
            target=out.with_suffix('.png');target.write_bytes(capture.read_bytes());report['capture']=str(target.relative_to(ROOT))
        else:report['capture']=None
    report['library_paths_verified']=all(loaded.get(p.name)==p.resolve() for p in expected)
    report['runtime_failures']=[line for line in log.splitlines() if re.search(r'BHL.*(unimplemented|not implemented)|Crash:|Xash Error:|BHL native loader:|Host_Error:',line)]
    report['god_enabled']='godmode ON' in log
    report['audio_verified']=f'Using SDL audio driver: {args.audio} @' in log and f'BHL native audio: SDL {args.audio},' in log
    interval=re.search(r'BHL_BENCH_SWAP_INTERVAL (-?\d+)',log)
    report['swap_interval']=int(interval[1]) if interval else None
    report['presented_intervals']=len(values)
    report['passed']=report['returncode']==0 and not timeout and len(marks)==2 and abs(len(values)-args.frames)<=4 and report['library_paths_verified'] and not report['runtime_failures'] and report['god_enabled']
    report['passed']=report['passed'] and report['swap_interval']==int(args.vsync) and report['audio_verified']
    if args.renderer=='metal':
        report['metal_verified']='BHL Metal backend verified: ANGLE' in log and 'BHL Metal device verified: EGL_ANGLE_device_metal' in log
        report['passed']=report['passed'] and report['metal_verified']
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
