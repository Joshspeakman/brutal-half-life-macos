"""Exercise the actual launcher, bundled libraries and separate game data.

All test saves have reserved names. Rendering is hidden and audio uses the
dummy device. This is not a claim of interactive gameplay equivalence.
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import signal
import subprocess
import time
from pathlib import Path
from native_harness import ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--app', type=Path, default=ROOT/'work/package/Brutal Half-Life.app')
    ap.add_argument('--stages', nargs='+', choices=['menu', 'campaign', 'save', 'reload', 'transition', 'combat', 'movement', 'cycles'],
                    default=['menu', 'campaign', 'save', 'reload', 'transition', 'combat', 'movement', 'cycles'])
    ap.add_argument('--seconds', type=int, default=180)
    args = ap.parse_args()
    app = args.app.resolve()
    with (app/'Contents/Info.plist').open('rb') as stream:
        data = Path(plistlib.load(stream)['BHLDataDirectory'])
    if not data.is_relative_to(ROOT/'work'):
        ap.error('Acceptance tests require isolated data inside ignored work/')
    executable = app/'Contents/MacOS/BrutalHalfLife'
    out = ROOT/'work/reports/packaged'; out.mkdir(exist_ok=True)
    image_probe = ROOT/'work/native/image-coverage'
    if 'menu' in args.stages:
        subprocess.run(['swiftc', str(ROOT/'tests/image_coverage.swift'), '-o', str(image_probe)], check=True)
    wait = 'wait\n'
    scripts = {
        'menu': 'scr_conspeed 100000\nescape\n'+wait*20+'snapshot scrshots/native-package-menu.png\n'+wait*3+'echo BHL_PACKAGE_MENU\nquit\n',
        'campaign': 'map start\n'+wait*35+'snapshot scrshots/native-package-start.png\n'+wait*3+'echo BHL_PACKAGE_CAMPAIGN\nquit\n',
        'save': 'map _zoo\n'+wait*30+'sv_cheats 1\ngod\nimpulse 101\n'+wait*10+'save native-package\n'+wait*5+'echo BHL_PACKAGE_SAVE\nquit\n',
        'reload': 'load native-package\n'+wait*35+'echo BHL_PACKAGE_RELOAD\nquit\n',
        'transition': 'map c0a0\n'+wait*30+'changelevel c0a0a c0a0toa\n'+wait*35+'save native-transition\n'+wait*5+'load native-transition\n'+wait*30+'echo BHL_PACKAGE_TRANSITION\nquit\n',
        'combat': 'map c2a2d\n'+wait*30+'sv_cheats 1\ngod\n'+wait*200+'save native-tank\n'+wait*5+'load native-tank\n'+wait*40+'echo BHL_PACKAGE_COMBAT\nquit\n',
        'movement': 'map _zoo\n'+wait*35+'viewpos\n+forward\n'+wait*30+'-forward\n'+wait*3+'viewpos\n+jump\n'+wait*5+'-jump\nviewpos\n'+wait*40+'+duck\n'+wait*10+'viewpos\n-duck\n'+wait*10+'viewpos\n+use\n'+wait*3+'-use\necho BHL_PACKAGE_MOVEMENT\nquit\n',
    }
    cycle_maps=['_zoo','start','c1a0','c1a1','c1a4','c2a2d','c2a3','c2a4','c3a1','c4a1','c4a3','c5a1','_zoo','start']
    scripts['cycles']=''.join('map '+name+'\n'+wait*35+'echo BHL_CYCLE '+name+'\n' for name in cycle_maps)+'save native-cycles\n'+wait*5+'load native-cycles\n'+wait*35+'echo BHL_PACKAGE_CYCLES\nquit\n'
    config = data/'bhl/config.cfg'
    before = hashlib.sha256(config.read_bytes()).hexdigest()
    report = dict(app_build=json.loads((app/'Contents/Resources/Build.json').read_text()), stages={}, passed=False)
    for stage in args.stages:
        name = 'native-package-'+stage+'.cfg'
        (data/'bhl'/name).write_text(scripts[stage])
        if stage == 'save': (data/'bhl/save/native-package.sav').unlink(missing_ok=True)
        if stage == 'combat': (data/'bhl/save/native-tank.sav').unlink(missing_ok=True)
        if stage == 'cycles': (data/'bhl/save/native-cycles.sav').unlink(missing_ok=True)
        capture = data/'bhl/scrshots/native-package-start.png'
        if stage == 'campaign': capture.unlink(missing_ok=True)
        menu_capture = data/'bhl/scrshots/native-package-menu.png'
        if stage == 'menu': menu_capture.unlink(missing_ok=True)
        log = out/(stage+'.launcher.log'); began = time.monotonic(); timeout = False
        with log.open('w') as stream:
            p = subprocess.Popen([str(executable), '--engine-test', name], stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = p.wait(timeout=args.seconds)
            except subprocess.TimeoutExpired:
                timeout = True; os.killpg(p.pid, signal.SIGKILL); code = p.wait()
        text = re.sub(r'\x1b\[[0-9;]*m', '', (data/'Last Run.log').read_text(errors='replace'))
        (out/(stage+'.log')).write_text(text)
        failures = [line for line in text.splitlines() if re.search(r'BHL.*(unimplemented|failed|not implemented)|Crash:|Xash Error:|BHL native loader:|Host_Error:', line)]
        checks = dict(normal_exit=code == 0 and not timeout, marker='BHL_PACKAGE_'+stage.upper() in text,
                      no_runtime_failure=not failures, config_preserved=hashlib.sha256(config.read_bytes()).hexdigest() == before)
        loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+)$', text, re.M)
        runtime = app/'Contents/Resources/Runtime'
        names = set(report['app_build']['runtime'])
        checks['bundled_libraries'] = bool(loaded) and all(Path(p).parent == runtime for p in loaded if Path(p).name in names)
        checks['no_homebrew_libraries'] = not any('/opt/homebrew/' in p for p in loaded)
        # mainui probes optional WON splash art even when the mod supplies a
        # Steam BackgroundLayout. All actual menu controls must be present.
        missing_art = re.findall(r'couldn.t load "(gfx/shell/[^\"]+)"', text)
        checks['menu_assets_available'] = not (set(missing_art)-{'gfx/shell/splash'})
        if stage == 'campaign': checks['capture_written'] = capture.exists()
        if stage == 'menu':
            checks['capture_written'] = menu_capture.exists()
            probe = subprocess.run([str(image_probe), str(menu_capture)], text=True, capture_output=True)
            checks['menu_not_blank'] = probe.returncode == 0
            (out/'menu-coverage.json').write_text(probe.stdout or json.dumps({'passed': False, 'error': probe.stderr}))
        if stage != 'menu':
            checks.update(connected='client connected at' in text, frames='BHL v3 completed 10 server frames' in text)
        if stage == 'save':
            saved = data/'bhl/save/native-package.sav'; checks['save_written'] = saved.exists() and saved.stat().st_size > 0
        if stage == 'transition': checks['three_map_activations'] = text.count('BHL v3 ServerActivate completed') >= 3
        if stage == 'combat':
            saved = data/'bhl/save/native-tank.sav'
            checks.update(god_enabled='godmode ON' in text,
                          expired_sprite_retired='BHL v3 retired an expired mounted-weapon sprite reference' in text,
                          save_written=saved.exists() and saved.stat().st_size > 0,
                          map_reloaded=text.count('BHL v3 ServerActivate completed') >= 2)
        if stage == 'movement':
            positions=[tuple(map(float,match)) for match in re.findall(r'org \( ([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) \)',text)]
            checks['five_positions']=len(positions)==5
            if len(positions)==5:
                checks.update(walking=sum((positions[0][i]-positions[1][i])**2 for i in (0,1))>64,
                              jumping=positions[2][2]>positions[1][2]+8,
                              crouching=positions[4][2]>positions[3][2]+8)
        if stage == 'cycles':
            saved=data/'bhl/save/native-cycles.sav'
            checks.update(all_maps_ran=len(re.findall(r'BHL_CYCLE ',text))==len(cycle_maps),
                          all_maps_activated=text.count('BHL v3 ServerActivate completed')>=len(cycle_maps)+1,
                          save_written=saved.exists() and saved.stat().st_size>0)
        report['stages'][stage] = dict(checks=checks, failures=failures, seconds=round(time.monotonic()-began, 2), passed=all(checks.values()))
        if stage == 'movement':report['stages'][stage]['view_positions']=positions
        report['passed'] = all(r['passed'] for r in report['stages'].values()) and stage == args.stages[-1]
        (out/'acceptance.json').write_text(json.dumps(report, indent=2)+'\n')
        print(stage, json.dumps(report['stages'][stage]), flush=True)
        if not report['stages'][stage]['passed']: raise SystemExit(1)


if __name__ == '__main__':
    main()
