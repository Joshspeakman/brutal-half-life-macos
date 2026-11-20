"""Link the in-development server ABI adapter to the compiled game modules."""
import subprocess
import hashlib
import json
from inputs import ROOT
import os
from pathlib import Path
# BHL_NATIVE_DIR / BHL_BRIDGE_DIR: another runtime build and output (tools/make_dropin.sh: the split runtime for macOS 14)
NATIVE=Path(os.environ.get('BHL_NATIVE_DIR', ROOT/'work/native')); BRIDGE=Path(os.environ.get('BHL_BRIDGE_DIR', ROOT/'work/bridge'))
from generate_bridge import main as generate



def split_game():
    """A split build (tools/build_runtime.py --split): the module tables the bridge reads are in the game's library, which is
    linked weakly (the drag-and-drop app's is built on the player's Mac)."""
    import json as _json
    native = NATIVE
    built = _json.loads((native / 'build.json').read_text()) if (native / 'build.json').exists() else {}
    return ['-Wl,-weak_library,' + str(native / 'libgame.dylib')] if built.get('split') else []

def main():
    generate()
    upstream=ROOT/'work/upstream/xash3d-fwgs'
    out=BRIDGE
    includes=[ROOT/'runtime',out,ROOT/'work/bridge']+[upstream/p for p in ['common','engine','public','pm_shared','3rdparty/library_suffix/include']]
    command=['clang','-arch','arm64','-std=gnu11','-O2','-g','-dynamiclib','-Wno-c2x-extensions']
    for path in includes:command+=['-I',str(path)]
    staged=out/'hl_arm64.new.dylib'
    command += [str(ROOT/'runtime/server_bridge.c'),'-L',str(NATIVE),'-lbhl', *split_game(),
                '-Wl,-rpath,@loader_path','-Wl,-rpath,@loader_path/../native','-o',str(staged)]
    subprocess.run(command,check=True)
    subprocess.run(['codesign','--force','--sign','-',str(staged)],check=True)
    staged.replace(out/'hl_arm64.dylib')
    paths=[ROOT/'runtime/server_bridge.c',ROOT/'runtime/guest_format.inc',ROOT/'runtime/model_bridge.inc',ROOT/'runtime/movement_bridge.inc',ROOT/'runtime/save_bridge.inc',ROOT/'tools/generate_bridge.py',ROOT/'tools/build_bridge.py',ROOT/'work/native/libbhl.dylib',out/'hl_arm64.dylib']
    paths.append(ROOT/'runtime/v3_lifetime.inc')
    paths.append(ROOT/'runtime/borrowed_strings.inc')
    paths.append(ROOT/'runtime/bridge_profile.inc')
    paths.append(ROOT/'runtime/server_entity_access.inc')
    paths.append(ROOT/'runtime/memory_watch.h')
    (out/'build.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')


if __name__=='__main__':main()
