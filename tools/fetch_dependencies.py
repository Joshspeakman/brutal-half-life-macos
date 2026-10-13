"""Fetch pinned engine/SDK dependencies into ignored work/, never system game folders."""
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


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-engine',action='store_true')
    ap.add_argument('--build-client-engine',action='store_true')
    args=ap.parse_args()
    upstream=ROOT/'work/upstream';upstream.mkdir(parents=True,exist_ok=True)
    engine=upstream/'xash3d-fwgs'
    checkout('https://github.com/FWGS/xash3d-fwgs.git',ENGINE_REVISION,engine,recursive=True)
    for patch in sorted((ROOT/'patches').glob('xash-*.patch')):
        already=subprocess.run(['git','apply','--reverse','--check',str(patch)],cwd=engine,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
        if not already:
            subprocess.run(['git','apply','--check',str(patch)],cwd=engine,check=True)
            subprocess.run(['git','apply',str(patch)],cwd=engine,check=True)
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
        subprocess.run(['./waf','configure','--out=build','--dedicated','--disable-werror','--64bits','--prefix='+str(ROOT/'work/engine-dedicated')],cwd=engine,check=True)
        subprocess.run(['./waf','build','-j4'],cwd=engine,check=True)
    if args.build_client_engine:
        subprocess.run(['./waf','configure','--out=build-client','--disable-werror','--64bits','--enable-soft','--enable-null','--sdl-use-pkgconfig','--prefix='+str(ROOT/'work/engine-client')],cwd=engine,check=True)
        subprocess.run(['./waf','build','-j4'],cwd=engine,check=True)
    print(f'Pinned dependencies ready; verified all {len(imports)} client VGUI imports')


if __name__=='__main__':main()
