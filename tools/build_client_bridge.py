"""Build the native client adapter, retaining explicit failures for missing calls."""
import hashlib
import json
import subprocess
import shlex
from inputs import ROOT
from generate_bridge import main as generate_server
from generate_client_bridge import main as generate_client



def split_game():
    """A split build (tools/build_runtime.py --split): the module tables the bridge reads are in the game's library, which is
    linked weakly (the drag-and-drop app's is built on the player's Mac)."""
    import json as _json
    native = ROOT / 'work/native'
    built = _json.loads((native / 'build.json').read_text()) if (native / 'build.json').exists() else {}
    return ['-Wl,-weak_library,' + str(native / 'libgame.dylib')] if built.get('split') else []

def main():
    generate_server()
    generate_client()
    engine = ROOT/'work/upstream/xash3d-fwgs'
    out = ROOT/'work/bridge'
    includes = [ROOT/'runtime',out]+[engine/p for p in ['common','engine','public','pm_shared','3rdparty/library_suffix/include']]
    command = ['clang','-arch','arm64','-std=gnu11','-O2','-g','-dynamiclib','-Wno-c2x-extensions']
    command += shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2'],text=True))
    for path in includes: command += ['-I',str(path)]
    staged = out/'client_arm64.new.dylib'
    command += shlex.split(subprocess.check_output(['pkg-config','--libs','sdl2'],text=True))
    command += [str(ROOT/'runtime/client_bridge.c'),str(ROOT/'runtime/vgui_bridge.c'),'-L',str(ROOT/'work/native'),'-lbhl', *split_game(),
                '-Wl,-rpath,@loader_path','-Wl,-rpath,@loader_path/../native','-Wl,-rpath,@loader_path/../dlls','-o',str(staged)]
    subprocess.run(command,check=True)
    subprocess.run(['codesign','--force','--sign','-',str(staged)],check=True)
    staged.replace(out/'client_arm64.dylib')
    paths=[ROOT/p for p in ['runtime/client_bridge.c','runtime/vgui_bridge.c','runtime/guest_format.inc','runtime/model_bridge.inc','runtime/movement_bridge.inc','runtime/effects_bridge.inc','runtime/particle_bridge.inc','runtime/studio_bridge.inc','tools/generate_client_bridge.py','tools/generate_bridge.py','tools/build_client_bridge.py','work/native/libbhl.dylib','work/bridge/client_arm64.dylib']]
    paths.append(ROOT/'runtime/borrowed_strings.inc')
    paths.append(ROOT/'runtime/bridge_profile.inc')
    paths.append(ROOT/'runtime/entity_access.inc')
    paths.append(ROOT/'runtime/memory_watch.h')
    paths.append(ROOT/'runtime/client_model_lookup.inc')
    (out/'client-build.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')


if __name__ == '__main__': main()
