"""Verify borrowed-memory materialization through translated access helpers."""
import subprocess
from native_harness import ROOT


def main():
    out=ROOT/'work/boundary-tests';out.mkdir(exist_ok=True)
    exe=out/'memory-watch'
    subprocess.run(['clang','-arch','arm64','-O2','-I',str(ROOT/'runtime'),
                    str(ROOT/'tests/memory_watch_fixture.c'),str(ROOT/'runtime/memory_watch.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


if __name__=='__main__':main()
