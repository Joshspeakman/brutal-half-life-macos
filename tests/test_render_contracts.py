"""Compare actual GPU pixels for fog, lightmaps, blending and depth on both paths."""
import fcntl
import json
import os
import shlex
import subprocess
from native_harness import ROOT


def main():
    session = ROOT/'work/session'
    lock = (session/'.session.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    probe = ROOT/'work/native/render-contracts.dylib'
    staged = probe.with_suffix('.stage.dylib')
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    subprocess.run(['clang', '-arch', 'arm64', '-O2', '-dynamiclib', '-Wno-deprecated-declarations', *flags,
                    '-L', str(ROOT/'work/native'), '-lbhl', '-Wl,-rpath,@loader_path',
                    '-o', str(staged), str(ROOT/'tests/render_contracts.c')], check=True)
    staged.replace(probe)
    for renderer in ('gl', 'metal'):
        out = ROOT/'work/reports'/('render-contracts-'+renderer+'.json')
        out.unlink(missing_ok=True)
        env = {k:v for k,v in os.environ.items() if not k.startswith(('SDL_', 'BHL_', 'DYLD_', 'ANGLE_'))}
        env.update(BHL_IMAGE_DIR=str(ROOT/'work/images'), SDL_VIDEODRIVER='cocoa', SDL_AUDIODRIVER='dummy',
                   DYLD_LIBRARY_PATH=str(session)+':'+str(ROOT/'work/native'), DYLD_INSERT_LIBRARIES=str(probe),
                   BHL_RENDER_CONTRACTS=str(out))
        command = ['./xash3d', '-noch', '-game', 'bhl', '-ref', renderer, '-hidden', '-windowed',
                   '-width', '640', '-height', '480', '-nomouse', '-dev', '3', '-noip', '-noip6',
                   '-nomaster', '-nowriteconfig']
        if renderer == 'metal':
            env.update(SDL_EGL_LIBRARY=str(session/'libEGL.dylib'), SDL_OPENGL_LIBRARY=str(session/'libGLESv2.dylib'),
                       ANGLE_DEFAULT_PLATFORM='metal')
            command.insert(1, '-metal')
        with out.with_suffix('.log').open('w') as stream:
            result = subprocess.run(command, cwd=session, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=60)
        report = json.loads(out.read_text()) if out.exists() else {'passed':False}
        text = out.with_suffix('.log').read_text(errors='replace')
        if renderer == 'metal':
            report['backend_verified'] = 'BHL Metal backend verified: ANGLE' in text
            report['passed'] = report['passed'] and report['backend_verified']
        report['passed'] = report['passed'] and result.returncode == 0
        out.write_text(json.dumps(report, indent=2)+'\n')
        print(renderer, json.dumps(report), flush=True)
        if not report['passed']:
            raise SystemExit(1)


if __name__ == '__main__': main()
