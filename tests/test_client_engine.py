"""Run the full engine with a null renderer to locate client integration faults.

This test cannot validate graphics, input, GUI drawing or audible output.
"""
import argparse
import json
import os
import re
import subprocess
import shutil
import fcntl
from native_harness import ROOT
from pathlib import Path


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session',type=Path,default=ROOT/'work/session')
    ap.add_argument('--report-dir',type=Path,default=ROOT/'work/reports')
    ap.add_argument('--seconds',type=int,default=15)
    ap.add_argument('--trace',action='store_true')
    ap.add_argument('--renderer',choices=['null','soft','gl','metal'],default='null')
    ap.add_argument('--video-driver',choices=['dummy','cocoa'],default='dummy')
    ap.add_argument('--map',default='c0a0')
    ap.add_argument('--capture',action='store_true',help='Request an engine-only PNG after twenty frames')
    ap.add_argument('--capture-frames',type=int,default=20)
    ap.add_argument('--save-load',action='store_true',help='Save, reload, then quit this isolated test session')
    ap.add_argument('--weapons',nargs='+',help='Exercise primary/secondary fire and reload for these weapon class names')
    ap.add_argument('--weapon-soak',action='store_true',help='Allow full draw/reload animations and retain per-weapon event evidence')
    ap.add_argument('--music',action='store_true',help='Request this map music through the original PlayMP3 handler')
    ap.add_argument('--god',action='store_true',help='Keep the player alive during unattended combat/save checks')
    ap.add_argument('--quit-after-frames',type=int,help='Finish a bounded map activation/capture run normally')
    args=ap.parse_args()
    session=args.session.resolve();reports=args.report_dir.resolve()
    if not session.is_relative_to(ROOT/'work') or not reports.is_relative_to(ROOT/'work'):ap.error('Tests require isolated paths inside work/')
    reports.mkdir(parents=True,exist_ok=True)
    session_lock=(session/'.session.lock').open('a')
    fcntl.flock(session_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    save_file=session/'bhl/save/native-acceptance.sav'
    if args.save_load:save_file.unlink(missing_ok=True)
    log=reports/'client-engine.log'
    # Earlier startup failures can make the engine save its own defaults before
    # it ever executes the mod's config. Always test the shipped v3 settings.
    shutil.copy2(ROOT/'work/fixtures/bhl-config.cfg',session/'bhl/config.cfg')
    environment=dict(os.environ,BHL_IMAGE_DIR=str(ROOT/'work/images'),SDL_VIDEODRIVER=args.video_driver,SDL_AUDIODRIVER='dummy',DYLD_LIBRARY_PATH=str(session))
    if args.trace:environment.update(BHL_TRACE='1',BHL_BRIDGE_TRACE='1')
    if args.weapons:environment['BHL_COMBAT_TRACE']='1'
    # Let a native fault fail this bounded process immediately. Xash's crash
    # dialog otherwise waits for desktop input until the harness times out.
    command=['./xash3d','-noch','-game','bhl','-ref',args.renderer,'-hidden','-windowed','-width','800','-height','600','-nomouse','-dev','3','-noip','-noip6','-nomaster','-nowriteconfig','-console','+sv_lan','1','+map',args.map]
    if args.renderer=='metal':
        environment.update(SDL_EGL_LIBRARY=str(session/'libEGL.dylib'),SDL_OPENGL_LIBRARY=str(session/'libGLESv2.dylib'),ANGLE_DEFAULT_PLATFORM='metal')
        command.insert(1,'-metal')
    capture=session/'bhl/scrshots/native-acceptance.png'
    if args.capture or args.save_load or args.weapons or args.music or args.quit_after_frames:
        capture.unlink(missing_ok=True)
        # Startup exec runs before signon; forwarding "god" before the
        # client connects is rejected as an unknown command by the engine.
        script=('wait\n'*25+'sv_cheats 1\ngod\n' if args.god else '')+'wait\n'*(args.quit_after_frames or args.capture_frames)
        if args.capture:script+='scr_conspeed 100000\n'+'wait\n'*3+'snapshot scrshots/native-acceptance.png\n'+'wait\n'*3
        if args.music:
            if not re.fullmatch(r'[A-Za-z0-9_-]+',args.map):ap.error('Map name cannot form a music path')
            script+=f'playaudio {args.map}.mp3\n'+'wait\n'*5
        if args.weapons:
            script+='sv_cheats 1\ngod\nimpulse 101\n'+'wait\n'*5
            for weapon in args.weapons:
                if not re.fullmatch(r'weapon_[A-Za-z0-9_]+',weapon):ap.error('Invalid weapon class name')
                draw,primary,secondary,reload=(90,60,60,150) if args.weapon_soak else (5,8,5,5)
                script+=f'echo BHL_WEAPON_BEGIN {weapon}\ngive {weapon}\n{weapon}\n'+'wait\n'*draw+'+attack\n'+'wait\n'*primary+'-attack\n'+'wait\n'*3+'+attack2\n'+'wait\n'*secondary+'-attack2\n+reload\n'+'wait\n'*reload+f'-reload\necho BHL_WEAPON_END {weapon}\n'
            script+='echo BHL_ACCEPTANCE_COMBAT_FINISHED\n'
        if args.save_load:script+='save native-acceptance\n'+'wait\n'*5+'load native-acceptance\n'+'wait\n'*20+'echo BHL_ACCEPTANCE_SAVE_LOAD_FINISHED\nquit\n'
        elif args.weapons or args.music or args.quit_after_frames:script+='echo BHL_ACCEPTANCE_MAP_FINISHED\nquit\n'
        (session/'bhl/native-acceptance.cfg').write_text(script)
        command+=['+exec','native-acceptance.cfg']
    timeout=False
    with log.open('w') as stream:
        try:
            code=subprocess.run(command,cwd=session,env=environment,stdout=stream,stderr=subprocess.STDOUT,timeout=args.seconds).returncode
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
        'client_connected':'client connected at',
    }.items()}
    failures=[line for line in text.splitlines() if re.search('BHL.*(unimplemented|failed|not implemented)|Crash:|Xash Error:|BHL native loader:|Host_Error:',line)]
    if args.renderer=='metal':
        stages['metal_backend_verified']='BHL Metal backend verified: ANGLE' in text and 'ANGLE Metal Renderer:' in text
        stages['metal_device_verified']='BHL Metal device verified: EGL_ANGLE_device_metal' in text
        failures += [line for line in text.splitlines() if re.search(r'OpenGL Error:|GL2_.*compile failed',line)]
    if args.save_load:
        stages['fresh_save_written']=save_file.exists() and save_file.stat().st_size>0
        stages['map_reactivated']=text.count('BHL v3 ServerActivate completed')>=2
        stages['save_load_finished']='BHL_ACCEPTANCE_SAVE_LOAD_FINISHED' in text and code==0 and not timeout
    if args.weapons:
        stages['combat_script_finished']='BHL_ACCEPTANCE_COMBAT_FINISHED' in text and code==0 and not timeout
        stages['weapon_event_executed']='BHL original weapon event:' in text
    if args.music:stages['original_music_requested']='BHL original map audio requested:' in text
    if args.god:stages['god_enabled']='godmode on' in text.lower()
    if args.quit_after_frames and not args.save_load:stages['bounded_map_finished']='BHL_ACCEPTANCE_MAP_FINISHED' in text and code==0 and not timeout
    report=dict(stages=stages,failures=failures,returncode=code,timeout=timeout,renderer=args.renderer,map=args.map,requested_weapons=args.weapons or [],weapon_events=sorted(set(re.findall(r'BHL original weapon event: (\S+)',text))),music_requested=args.music,save_load_requested=args.save_load,passed=all(stages.values()) and not failures and (timeout or code==0) and (not args.capture or capture.exists()),capture=str(capture.relative_to(ROOT)) if args.capture and capture.exists() else None,rendering_tested=False,gameplay_validated=False)
    report['weapon_soak']=args.weapon_soak
    report['per_weapon_events']={weapon:sorted(set(re.findall(r'BHL original weapon event: (\S+)',body))) for weapon,body in re.findall(r'BHL_WEAPON_BEGIN (weapon_\w+)\s+(.*?)BHL_WEAPON_END \1',text,re.S)}
    log.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    raise SystemExit(not report['passed'])


if __name__=='__main__':main()
