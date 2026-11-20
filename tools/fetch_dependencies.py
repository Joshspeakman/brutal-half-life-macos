"""Fetch pinned engine/SDK dependencies into ignored work/, never system game folders."""
import os
import argparse
import hashlib
import json
import shutil
import subprocess
import pefile
from inputs import ROOT, PROFILE
from generate_bridge import ENGINE_REVISION


def checkout(url,revision,destination,recursive=False):
    if not destination.exists():
        subprocess.run(['git','clone',url,str(destination)],check=True)
        subprocess.run(['git','checkout','--detach',revision],cwd=destination,check=True)
    actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=destination,text=True).strip()
    if actual!=revision:raise RuntimeError(f'{destination.name}: unexpected revision; existing checkout left unchanged')
    if recursive:subprocess.run(['git','submodule','update','--init','--recursive'],cwd=destination,check=True)


def apply_patches(engine,patches):
    """Apply the project's engine patches exactly. A tree patched by another
    revision of this project (an app update, a branch switch) is reset to the
    pinned sources first: patches that edit each other's hunks cannot be
    detected or applied one by one. Ignored build output is kept."""
    def applied(patch):
        return subprocess.run(['git','apply','--reverse','--check',str(patch)],cwd=engine,
                              stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
    if patches and all(applied(p) for p in patches):return
    changed=subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=engine,text=True).strip()
    if changed:
        print('Engine sources differ from this revision\'s patches; restoring the pinned sources',flush=True)
        subprocess.run(['git','checkout','--','.'],cwd=engine,check=True)
    for name in subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=engine,text=True).split('\0'):
        if name:(engine/name).unlink()       # files a previous patch set created
    for patch in patches:
        subprocess.run(['git','apply','--check',str(patch)],cwd=engine,check=True)
        subprocess.run(['git','apply',str(patch)],cwd=engine,check=True)


def stale_lock(engine):   # (BHL_ENGINE_OUT: the client engine's output folder, build-client unless make_dropin.sh's build-dropin)
    """waf reads its directories back from its lock file before configuring: one written through another path to this
    checkout (a symlinked ~/Projects) makes every later build do nothing. Configure writes it again."""
    for lock in engine.glob('.lock-waf_*'):lock.unlink()
    deps=os.environ.get('BHL_DEPS')            # libraries built for the app's macOS (tools/build_deps.sh): theirs, not Homebrew's
    if deps:os.environ['PKG_CONFIG_LIBDIR']=os.path.join(deps,'lib','pkgconfig')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-engine',action='store_true')
    ap.add_argument('--build-client-engine',action='store_true')
    args=ap.parse_args()
    upstream=ROOT/'work/upstream';upstream.mkdir(parents=True,exist_ok=True)
    engine=upstream/'xash3d-fwgs'
    checkout('https://github.com/FWGS/xash3d-fwgs.git',ENGINE_REVISION,engine,recursive=True)
    apply_patches(engine,sorted((ROOT/'patches').glob('xash-*.patch')))
    spec=PROFILE['modules']['vgui.dll'];sdk=upstream/'vgui-dev'
    checkout(spec['sdk_repository'],spec['sdk_revision'],sdk)
    payload=(sdk/spec['sdk_path']).read_bytes()
    if hashlib.sha256(payload).hexdigest()!=spec['sha256']:raise RuntimeError('SDK VGUI input hash differs')
    target=ROOT/'work/inputs/vgui.dll';target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(payload)
    client=pefile.PE(str(ROOT/'work/inputs/client.dll'))
    imports={item.name for entry in client.DIRECTORY_ENTRY_IMPORT if entry.dll.lower()==b'vgui.dll' for item in entry.imports}
    exports={entry.name for entry in pefile.PE(data=payload).DIRECTORY_ENTRY_EXPORT.symbols}
    missing=imports-exports
    if missing:raise RuntimeError(f'VGUI is missing {len(missing)} required client exports')
    (ROOT/'work/reports/dependencies.json').write_text(json.dumps(dict(engine_revision=ENGINE_REVISION,vgui_revision=spec['sdk_revision'],vgui_sha256=spec['sha256'],vgui_imports_resolved=len(imports)),indent=2)+'\n')
    if args.build_engine:
        stale_lock(engine)
        subprocess.run(['./waf','configure','--out=build','--dedicated','--disable-werror','--64bits','--prefix='+str(ROOT/'work/engine-dedicated')],cwd=engine,check=True)
        subprocess.run(['./waf','build','-j4'],cwd=engine,check=True)
    if args.build_client_engine:
        from fetch_angle import fetch
        fetch()
        stale_lock(engine)
        subprocess.run(['./waf','configure','--out='+os.environ.get('BHL_ENGINE_OUT','build-client'),'--disable-werror','--64bits','--enable-soft','--enable-null','--enable-gles3compat','--sdl-use-pkgconfig','--prefix='+str(ROOT/'work/engine-client')],cwd=engine,check=True)
        subprocess.run(['./waf','build','-j4'],cwd=engine,check=True)
    print(f'Pinned dependencies ready; verified all {len(imports)} client VGUI imports')


if __name__=='__main__':main()
