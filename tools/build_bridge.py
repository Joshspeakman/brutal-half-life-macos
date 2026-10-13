"""Link the in-development server ABI adapter to the compiled game modules."""
import subprocess
import hashlib
import json
from inputs import ROOT
from generate_bridge import main as generate


def main():
    generate()
    upstream=ROOT/'work/upstream/xash3d-fwgs'
    out=ROOT/'work/bridge'
    includes=[ROOT/'runtime',out]+[upstream/p for p in ['common','engine','public','pm_shared','3rdparty/library_suffix/include']]
    command=['clang','-arch','arm64','-std=gnu11','-O2','-g','-dynamiclib','-Wno-c2x-extensions']
    for path in includes:command+=['-I',str(path)]
    staged=out/'hl_arm64.new.dylib'
    command += [str(ROOT/'runtime/server_bridge.c'),'-L',str(ROOT/'work/native'),'-lbhl',
                '-Wl,-rpath,@loader_path','-Wl,-rpath,@loader_path/../native','-o',str(staged)]
    subprocess.run(command,check=True)
    subprocess.run(['codesign','--force','--sign','-',str(staged)],check=True)
    staged.replace(out/'hl_arm64.dylib')
    paths=[ROOT/'runtime/server_bridge.c',ROOT/'runtime/guest_format.inc',ROOT/'runtime/model_bridge.inc',ROOT/'runtime/movement_bridge.inc',ROOT/'runtime/save_bridge.inc',ROOT/'tools/generate_bridge.py',ROOT/'tools/build_bridge.py',ROOT/'work/native/libbhl.dylib',out/'hl_arm64.dylib']
    paths.append(ROOT/'runtime/v3_lifetime.inc')
    paths.append(ROOT/'runtime/borrowed_strings.inc')
    paths.append(ROOT/'runtime/bridge_profile.inc')
    (out/'build.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')


if __name__=='__main__':main()
