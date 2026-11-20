#!/usr/bin/env python3
"""dropin_build.py --game DIR --out DIR --work DIR --setup DIR --llvm DIR: the drag-and-drop app's build of the player's
own Brutal Half-Life on their Mac (recomp's setup.py runs it as game.json's "pipeline"; tools/make_dropin.sh makes the
app). No Xcode tools there: the app's own clang, ld64.lld, headers (setup/include), Python and UPX (Resources/bin).

The game folder is a Half-Life folder with Brutal Half-Life v3 in it (bhl/ beside valve/: bhl_v3.zip extracted into it, as
on a PC; or the zip dropped on the app, with Steam's Half-Life found beside). Steps, in this project's own tools (shipped
in setup/bhl and copied into the work folder, where they write):
  1. the mod's three DLLs, checked against config/modules.json, and VGUI from the pinned SDK revision (downloaded, checked);
  2. FMOD unpacked (UPX), the images relocated, the four modules translated (vendor/lifter, tools/lift_module.py);
  3. tools/build_runtime.py --game-only: their code and tables as <out>/libgame.dylib, which the app's runtime binds;
     the prepared images beside it (<out>/Images);
  4. the data folder (<out>/..): Half-Life's valve and the mod's files (no DLLs), as tools/package_app.py lays it out.
Progress lines "@progress <0..1> <text>" go to stdout for the setup window."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

MODULES = ['hl.dll', 'client.dll', 'fmod.dll', 'vgui.dll']


def say(frac, text):
    print('@progress %.3f %s' % (frac, text), flush=True)


def main():
    ap = argparse.ArgumentParser()
    for a in ('--game', '--out', '--work', '--setup', '--llvm', '--macos'):
        ap.add_argument(a, type=Path, required=True)
    a = ap.parse_args()
    game, out = a.game, a.out
    valve, mod = game / 'valve', game / 'bhl'
    if not (valve / 'liblist.gam').exists() and not (valve / 'halflife.wad').exists():
        sys.exit("Half-Life's valve folder wasn't found: drop your Half-Life folder with Brutal Half-Life extracted into it")
    root = a.work / 'bhl'
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(a.setup / 'bhl', root, symlinks=True)
    site = a.work / 'site'
    say(0.0, 'Getting the tools ready')
    r = subprocess.run([sys.executable, '-m', 'pip', 'install', '-q', '--disable-pip-version-check', '--no-index', '--find-links',
                        str(a.setup / 'wheels'), '--target', str(site), 'pefile'], capture_output=True, text=True)
    if r.returncode:
        sys.exit('the tools could not be installed: ' + (r.stderr or r.stdout).strip()[-300:])
    env = dict(os.environ, PYTHONPATH=os.pathsep.join([str(site), str(root / 'tools')]), CC=str(a.llvm / 'clang'),
               BHL_LD=str(a.llvm / 'ld64.lld'), BHL_RUNTIME_LIB=str(a.macos.parent / 'Resources/Runtime/libbhl.dylib'),
               BHL_CFLAGS='--target=arm64-apple-macos14 -nostdlibinc -isystem "%s"' % (a.setup / 'include'))
    profile = json.loads((root / 'config/modules.json').read_text())['modules']

    # 1. the inputs
    say(0.02, "Checking the mod's files")
    inputs = root / 'work/inputs'
    inputs.mkdir(parents=True, exist_ok=True)
    for name, spec in profile.items():
        if 'member' in spec:
            src = mod.joinpath(*spec['member'].split('/'))
            data = src.read_bytes() if src.exists() else b''
            if hashlib.sha256(data).hexdigest() != spec['sha256']:
                sys.exit(f"{spec['member']} in the bhl folder isn't Brutal Half-Life v3's (or is missing)")
            (inputs / name).write_bytes(data)
    spec = profile['vgui.dll']
    url = 'https://raw.githubusercontent.com/%s/%s/%s' % (spec['sdk_repository'].split('github.com/')[1].removesuffix('.git'),
                                                          spec['sdk_revision'], spec['sdk_path'])
    try:
        with urllib.request.urlopen(url, timeout=120) as resp:
            data = resp.read()
    except Exception as e:
        sys.exit(f'VGUI could not be downloaded ({e}): is this Mac online?')
    if hashlib.sha256(data).hexdigest() != spec['sha256']:
        sys.exit('the downloaded VGUI is not the pinned one')
    (inputs / 'vgui.dll').write_bytes(data)
    (root / 'work/reports').mkdir(parents=True, exist_ok=True)

    def run(frac, text, *cmd):
        say(frac, text)
        r = subprocess.run([sys.executable, *map(str, cmd)], cwd=root, env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit('%s failed: %s' % (Path(str(cmd[0])).name, (r.stderr or r.stdout).strip()[-400:]))

    # 2. unpack, relocate, translate
    run(0.05, 'Unpacking the sound library', root / 'tools/unpack_audio.py')
    run(0.08, 'Placing the modules', root / 'tools/relocate.py')
    sys.path.insert(0, str(site))
    import pefile
    first = 0
    for n, module in enumerate(MODULES):
        binary = root / 'work/images' / module
        analysis = root / 'work/reports' / (module + '-analysis.json')
        run(0.10 + 0.08 * n, "Reading the game's code", root / 'vendor/lifter/analyze.py', binary, analysis)
        run(0.14 + 0.08 * n, "Translating the game's code", root / 'tools/lift_module.py', module, analysis, '--prepared', '--imp-base', first)
        first += sum(len(d.imports) for d in pefile.PE(str(binary)).DIRECTORY_ENTRY_IMPORT)
    # 3. the game's library and its images
    run(0.45, 'Building the game for your Mac', root / 'tools/build_runtime.py', *MODULES, '--game-only', '--out', out)
    say(0.92, 'Putting it together')
    shutil.rmtree(out / 'game-obj', ignore_errors=True)
    for name in ('tables.c', 'modules.c', 'nls_tables.h'):   # (the build's own intermediates)
        (out / name).unlink(missing_ok=True)
    images = out / 'Images'
    shutil.rmtree(images, ignore_errors=True)
    shutil.copytree(root / 'work/images', images)
    # 4. the data folder, as tools/package_app.py prepares it (no DLLs: the prepared images live with the library)
    data = out.parent
    for sub in ('valve', 'bhl'):
        shutil.rmtree(data / sub, ignore_errors=True)
    if subprocess.run(['/bin/cp', '-cR', str(valve), str(data / 'valve')], capture_output=True).returncode:
        shutil.copytree(valve, data / 'valve')
    for junk in ('SAVE', 'save', 'config.cfg', 'video.cfg', 'screenshots', 'scrshots'):
        p = data / 'valve' / junk
        shutil.rmtree(p, ignore_errors=True) if p.is_dir() else p.unlink(missing_ok=True)
    shutil.copytree(mod, data / 'bhl', ignore=shutil.ignore_patterns('*.dll', '*.DLL', '*.exe', '*.EXE'))
    (data / 'bhl/gameinfo.txt').write_text('basedir "valve"\ngamedir "bhl"\ntitle "Brutal Half-Life v3"\nstartmap "start"\n'
                                           'trainmap "_zoo"\nmax_edicts "2048"\ngamemode "singleplayer_only"\n')
    (data / 'native-install.json').write_text(json.dumps(dict(format=1, release='Brutal Half-Life v3', source='drag-and-drop setup'),
                                                         indent=2) + '\n')
    say(1.0, 'Built')


if __name__ == '__main__':
    main()
