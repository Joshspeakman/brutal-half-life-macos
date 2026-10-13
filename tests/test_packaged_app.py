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
    ap.add_argument('--stages', nargs='+', choices=['menu', 'campaign', 'save', 'reload', 'transition'],
                    default=['menu', 'campaign', 'save', 'reload', 'transition'])
    ap.add_argument('--seconds', type=int, default=180)
    args = ap.parse_args()
    app = args.app.resolve()
    with (app/'Contents/Info.plist').open('rb') as stream:
        data = Path(plistlib.load(stream)['BHLDataDirectory'])
    if not data.is_relative_to(ROOT/'work'):
        ap.error('Acceptance tests require isolated data inside ignored work/')
    executable = app/'Contents/MacOS/BrutalHalfLife'
    out = ROOT/'work/reports/packaged'; out.mkdir(exist_ok=True)
    wait = 'wait\n'
    scripts = {
        'menu': wait*20+'echo BHL_PACKAGE_MENU\nquit\n',
        'campaign': 'map start\n'+wait*35+'snapshot scrshots/native-package-start.png\n'+wait*3+'echo BHL_PACKAGE_CAMPAIGN\nquit\n',
        'save': 'map _zoo\n'+wait*30+'sv_cheats 1\ngod\nimpulse 101\n'+wait*10+'save native-package\n'+wait*5+'echo BHL_PACKAGE_SAVE\nquit\n',
        'reload': 'load native-package\n'+wait*35+'echo BHL_PACKAGE_RELOAD\nquit\n',
        'transition': 'map c0a0\n'+wait*30+'changelevel c0a0a c0a0toa\n'+wait*35+'save native-transition\n'+wait*5+'load native-transition\n'+wait*30+'echo BHL_PACKAGE_TRANSITION\nquit\n',
    }
    config = data/'bhl/config.cfg'
    before = hashlib.sha256(config.read_bytes()).hexdigest()
    report = dict(app_build=json.loads((app/'Contents/Resources/Build.json').read_text()), stages={}, passed=False)
    for stage in args.stages:
        name = 'native-package-'+stage+'.cfg'
        (data/'bhl'/name).write_text(scripts[stage])
        if stage == 'save': (data/'bhl/save/native-package.sav').unlink(missing_ok=True)
        capture = data/'bhl/scrshots/native-package-start.png'
        if stage == 'campaign': capture.unlink(missing_ok=True)
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
        if stage != 'menu':
            checks.update(connected='client connected at' in text, frames='BHL v3 completed 10 server frames' in text)
        if stage == 'save':
            saved = data/'bhl/save/native-package.sav'; checks['save_written'] = saved.exists() and saved.stat().st_size > 0
        if stage == 'transition': checks['three_map_activations'] = text.count('BHL v3 ServerActivate completed') >= 3
        report['stages'][stage] = dict(checks=checks, failures=failures, seconds=round(time.monotonic()-began, 2), passed=all(checks.values()))
        report['passed'] = all(r['passed'] for r in report['stages'].values()) and stage == args.stages[-1]
        (out/'acceptance.json').write_text(json.dumps(report, indent=2)+'\n')
        print(stage, json.dumps(report['stages'][stage]), flush=True)
        if not report['stages'][stage]['passed']: raise SystemExit(1)


if __name__ == '__main__':
    main()
