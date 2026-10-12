"""Prepare an isolated engine test tree from owned Half-Life data and BHL v3."""
import argparse
import hashlib
import json
import shutil
import stat
import zipfile
from pathlib import Path, PurePosixPath
from inputs import ROOT

ARCHIVE_SHA256='29759d1c91fbe00b42e56df2f277663ef89eab8a715542a4722884558bb80cac'


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--valve',type=Path,required=True)
    ap.add_argument('--archive',type=Path,required=True)
    ap.add_argument('--assets-only',action='store_true')
    ap.add_argument('--client',action='store_true',help='Also stage the development client and full engine')
    args=ap.parse_args()
    for name in ('maps/c0a0.bsp','halflife.wad','models/v_9mmhandgun.mdl'):
        if not (args.valve/name).is_file():raise RuntimeError(f'Missing Half-Life data: {name}')
    if hashlib.sha256(args.archive.read_bytes()).hexdigest()!=ARCHIVE_SHA256:
        raise RuntimeError('Archive does not match the original BHL v3 release')
    out=ROOT/'work/session'
    out.mkdir(parents=True,exist_ok=True)
    if not (out/'valve').exists():
        staging=out/'valve.new'
        if staging.exists():raise RuntimeError('An interrupted valve.new staging copy needs review')
        shutil.copytree(args.valve,staging,ignore=shutil.ignore_patterns('SAVE','save','*.sav','*.log','config.cfg','video.cfg'))
        staging.rename(out/'valve')
    if not (out/'bhl').exists():
        staging=out/'bhl.new';staging.mkdir(exist_ok=False)
        with zipfile.ZipFile(args.archive) as archive:
            for info in archive.infolist():
                parts=PurePosixPath(info.filename.replace('\\','/'))
                if parts.is_absolute() or '..' in parts.parts or stat.S_ISLNK(info.external_attr>>16):
                    raise RuntimeError(f'Unsafe archive entry: {info.filename}')
                target=staging/parts
                if info.is_dir():target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    with archive.open(info) as source,target.open('wb') as dest:shutil.copyfileobj(source,dest)
        staging.rename(out/'bhl')
    (out/'bhl/gameinfo.txt').write_text('''basedir "valve"
gamedir "bhl"
title "Brutal Half-Life v3 native development"
startmap "start"
trainmap "_zoo"
gamedll_osx "dlls/hl.dylib"
clientlib "cl_dlls/client.dylib"
max_edicts "2048"
gamemode "singleplayer_only"
''')
    if not args.assets_only:
        record=json.loads((ROOT/'work/bridge/build.json').read_text())
        for filename,digest in record.items():
            if hashlib.sha256((ROOT/filename).read_bytes()).hexdigest()!=digest:
                raise RuntimeError(f'Bridge is stale: {filename}; rebuild before preparing a session')
        upstream=ROOT/'work/upstream/xash3d-fwgs/build'
        shutil.copy2(upstream/'engine/xash',out/'xash')
        shutil.copy2(upstream/'filesystem/filesystem_stdio.dylib',out/'filesystem_stdio.dylib')
        shutil.copy2(ROOT/'work/bridge/hl_arm64.dylib',out/'bhl/dlls/hl_arm64.dylib')
        shutil.copy2(ROOT/'work/native/libbhl.dylib',out/'bhl/dlls/libbhl.dylib')
        if args.client:
            record=json.loads((ROOT/'work/bridge/client-build.json').read_text())
            for filename,digest in record.items():
                if hashlib.sha256((ROOT/filename).read_bytes()).hexdigest()!=digest:
                    raise RuntimeError(f'Client bridge is stale: {filename}')
            full=ROOT/'work/upstream/xash3d-fwgs/build-client'
            for source,target in [('game_launch/xash3d','xash3d'),('engine/libxash.dylib','libxash.dylib'),('3rdparty/mainui/libmenu.dylib','libmenu.dylib'),('ref/gl/libref_gl.dylib','libref_gl.dylib'),('ref/soft/libref_soft.dylib','libref_soft.dylib'),('ref/null/libref_null.dylib','libref_null.dylib')]:
                shutil.copy2(full/source,out/target)
            shutil.copy2(ROOT/'work/bridge/client_arm64.dylib',out/'bhl/cl_dlls/client_arm64.dylib')
    print('Isolated session prepared under work/session; installed games and saves unchanged')


if __name__=='__main__':main()
