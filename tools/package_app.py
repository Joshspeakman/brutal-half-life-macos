"""Build a local native app and a separate, owned game-data installation.

The bundle contains generated code and prepared images; never distribute it
or commit it. Existing user settings and saves are preserved on rebuild.
"""
import argparse
import fcntl
import hashlib
import json
import os
import plistlib
import shutil
import stat
import subprocess
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from inputs import ROOT
from prepare_session import ARCHIVE_SHA256


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def dependencies(path):
    return [line.strip().split(' (compatibility')[0]
            for line in run('otool', '-L', path).splitlines()[1:]]


def bundle_runtime(target):
    full = ROOT/'work/upstream/xash3d-fwgs/build-client'
    inputs = {name: full/source for name, source in {
        'xash3d': 'game_launch/xash3d', 'libxash.dylib': 'engine/libxash.dylib',
        'libmenu.dylib': '3rdparty/mainui/libmenu.dylib',
        'libref_gl.dylib': 'ref/gl/libref_gl.dylib',
        'filesystem_stdio.dylib': 'filesystem/filesystem_stdio.dylib',
    }.items()}
    inputs.update({
        'hl_arm64.dylib': ROOT/'work/bridge/hl_arm64.dylib',
        'client_arm64.dylib': ROOT/'work/bridge/client_arm64.dylib',
        'libbhl.dylib': ROOT/'work/native/libbhl.dylib',
    })
    # sdl2-compat loads SDL3 dynamically, so it is absent from otool -L.
    sdl = Path(run('pkg-config', '--variable=libdir', 'sdl2').strip())/'libSDL2.dylib'
    if 'libSDL3.dylib' in run('strings', sdl):
        sdl3 = Path(run('pkg-config', '--variable=libdir', 'sdl3').strip())/'libSDL3.dylib'
        inputs['libSDL3.dylib'] = sdl3
    pending = list(inputs.items())
    originals = {}
    while pending:
        name, source = pending.pop()
        if name in originals:
            if source.resolve() != originals[name].resolve():
                raise RuntimeError(f'Conflicting dependency: {name}')
            continue
        originals[name] = source
        shutil.copy2(source, target/name)
        own_ids = run('otool', '-D', source).splitlines()[1:] if name.endswith('.dylib') else []
        for dependency in dependencies(source):
            if dependency in own_ids:
                continue
            if dependency.startswith(('/System/', '/usr/lib/')):
                continue
            if dependency.startswith('@'):
                if Path(dependency).name not in inputs:
                    raise RuntimeError(f'Unresolved dependency: {dependency}')
                continue
            dep = Path(dependency)
            if dep.resolve() != source.resolve():
                pending.append((dep.name, dep))
    # A flat Runtime directory allows every dependency to be relative to its
    # own loader. No Homebrew prefix or development checkout is needed to run.
    for name, source in originals.items():
        binary = target/name
        edits = []
        if name.endswith('.dylib'):
            edits += ['-id', '@loader_path/'+name]
        own_id = run('otool', '-D', binary).splitlines()[1:] if name.endswith('.dylib') else []
        for dependency in dependencies(binary):
            if dependency in own_id or dependency.startswith(('/System/', '/usr/lib/')):
                continue
            depname = Path(dependency).name
            if depname not in originals:
                raise RuntimeError(f'Missing bundled library: {dependency}')
            edits += ['-change', dependency, '@loader_path/'+depname]
        if edits:
            run('install_name_tool', *edits, binary)
        run('codesign', '--force', '--sign', '-', binary)
        run('codesign', '--verify', '--strict', binary)
        if 'arm64' not in run('lipo', '-archs', binary).split():
            raise RuntimeError(f'Not an ARM64 binary: {name}')
        for dep in dependencies(binary):
            if not dep.startswith(('/System/', '/usr/lib/', '@loader_path/')):
                raise RuntimeError(f'Nonportable dependency: {dep}')
    return {name: digest(target/name) for name in sorted(originals)}


def prepare_data(data, archive, valve):
    marker = data/'native-install.json'
    if data.exists():
        if not marker.is_file() or json.loads(marker.read_text()).get('archive_sha256') != ARCHIVE_SHA256:
            raise RuntimeError('Refusing to reuse an unrelated or incomplete data folder')
        return
    data.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=data.name+'.stage-', dir=data.parent))
    try:
        shutil.copytree(valve, staging/'valve', ignore=shutil.ignore_patterns(
            'SAVE', 'save', '*.sav', '*.log', 'config.cfg', 'video.cfg', 'screenshots', 'scrshots'))
        mod = staging/'bhl'
        mod.mkdir()
        with zipfile.ZipFile(archive) as source:
            for info in source.infolist():
                p = PurePosixPath(info.filename.replace('\\', '/'))
                if p.is_absolute() or '..' in p.parts or stat.S_ISLNK(info.external_attr >> 16):
                    raise RuntimeError(f'Unsafe archive entry: {info.filename}')
                # Prepared PE data live in the app; these Windows libraries
                # are not installed as executable game plugins.
                if p.suffix.lower() in ('.dll', '.exe'):
                    continue
                out = mod/p
                if info.is_dir():
                    out.mkdir(parents=True, exist_ok=True)
                else:
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(info) as src, out.open('wb') as dst:
                        shutil.copyfileobj(src, dst)
        (mod/'gameinfo.txt').write_text('''basedir "valve"
gamedir "bhl"
title "Brutal Half-Life v3"
startmap "start"
trainmap "_zoo"
max_edicts "2048"
gamemode "singleplayer_only"
''')
        (staging/'native-install.json').write_text(json.dumps(dict(
            format=1, release='Brutal Half-Life v3', archive_sha256=ARCHIVE_SHA256), indent=2)+'\n')
        staging.rename(data)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--valve', type=Path, required=True)
    ap.add_argument('--app', type=Path, default=ROOT/'work/package/Brutal Half-Life.app')
    ap.add_argument('--data-dir', type=Path, default=ROOT/'work/app-data')
    ap.add_argument('--replace', action='store_true', help='Replace an existing app from this project, preserving its separate user data')
    args = ap.parse_args()
    archive, valve = args.archive.expanduser().resolve(), args.valve.expanduser().resolve()
    app, data = args.app.expanduser().absolute(), args.data_dir.expanduser().resolve()
    if digest(archive) != ARCHIVE_SHA256:
        raise RuntimeError('Archive does not match the pinned BHL v3 release')
    for relative in ('maps/c0a0.bsp', 'halflife.wad', 'models/v_9mmhandgun.mdl'):
        if not (valve/relative).is_file():
            raise RuntimeError(f'Missing Half-Life data: {relative}')
    if data == valve or valve in data.parents or data in valve.parents:
        raise RuntimeError('The data destination must be separate from the source game')
    for record in ('build.json', 'client-build.json'):
        for name, expected in json.loads((ROOT/'work/bridge'/record).read_text()).items():
            if digest(ROOT/name) != expected:
                raise RuntimeError(f'Stale build: {name}')
    if app.exists():
        if not args.replace:
            raise RuntimeError('App destination exists; use --replace to update this local app')
        with (app/'Contents/Info.plist').open('rb') as stream:
            existing = plistlib.load(stream)
        if existing.get('CFBundleIdentifier') != 'io.github.brutal-half-life-macos' or not (app/'Contents/Resources/Build.json').is_file():
            raise RuntimeError('Refusing to replace an unrelated application')
        if Path(existing.get('BHLDataDirectory', '')).resolve() != data:
            raise RuntimeError('Use the existing app data directory when replacing it so saves remain available')
    prepare_data(data, archive, valve)
    session_lock = (data/'.game.lock').open('a')
    fcntl.flock(session_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    app.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.bhl-app-', dir=app.parent))
    try:
        macos = staging/'Contents/MacOS'; macos.mkdir(parents=True)
        resources = staging/'Contents/Resources'; resources.mkdir()
        runtime = resources/'Runtime'; runtime.mkdir()
        hashes = bundle_runtime(runtime)
        shutil.copy2(ROOT/'work/upstream/xash3d-fwgs/build-client/3rdparty/extras/extras.pk3', resources/'extras.pk3')
        images = resources/'Images'; images.mkdir()
        for name in ('hl.dll', 'client.dll', 'fmod.dll', 'vgui.dll'):
            shutil.copy2(ROOT/'work/images'/name, images/name)
        run('swiftc', '-O', '-target', 'arm64-apple-macos14.0', '-framework', 'AppKit',
            *sorted((ROOT/'launcher').glob('*.swift')), '-o', macos/'BrutalHalfLife')
        info = dict(CFBundleExecutable='BrutalHalfLife', CFBundleIdentifier='io.github.brutal-half-life-macos',
                    CFBundleName='Brutal Half-Life', CFBundleDisplayName='Brutal Half-Life',
                    CFBundlePackageType='APPL', CFBundleShortVersionString='0.1', CFBundleVersion='1',
                    LSMinimumSystemVersion='14.0', NSHighResolutionCapable=True,
                    NSPrincipalClass='NSApplication', CFBundleIconFile='BrutalHalfLife', BHLDataDirectory=str(data))
        with (staging/'Contents/Info.plist').open('wb') as stream:
            plistlib.dump(info, stream)
        iconset = staging/'BrutalHalfLife.iconset'
        run(macos/'BrutalHalfLife', '--write-iconset', iconset)
        run('iconutil', '-c', 'icns', iconset, '-o', resources/'BrutalHalfLife.icns')
        shutil.rmtree(iconset)
        shutil.copy2(ROOT/'NOTICE.txt', resources/'NOTICE.txt')
        (resources/'Build.json').write_text(json.dumps(dict(
            revision=run('git', '-C', ROOT, 'rev-parse', 'HEAD').strip(),
            source_dirty=bool(run('git', '-C', ROOT, 'status', '--porcelain').strip()),
            architecture='arm64', runtime=hashes,
            images={p.name: digest(p) for p in images.iterdir()}), indent=2)+'\n')
        run('codesign', '--force', '--sign', '-', staging)
        run('codesign', '--verify', '--deep', '--strict', staging)
        if app.exists():
            backup = Path(tempfile.mkdtemp(prefix='.bhl-previous-', dir=app.parent))/'Previous.app'
            app.rename(backup)
            try:
                staging.rename(app)
            except BaseException:
                backup.rename(app)
                backup.parent.rmdir()
                raise
            else:
                shutil.rmtree(backup)
                backup.parent.rmdir()
        else:
            staging.rename(app)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    print(f'Local app: {app}\nSeparate data and saves: {data}')


if __name__ == '__main__':
    main()
