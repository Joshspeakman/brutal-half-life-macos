"""Unpack the user's verified FMOD DLL without changing the original archive."""
import hashlib
import json
import shutil
import subprocess
import pefile
from inputs import ROOT, verify_module

EXPECTED = "be6fa673f52616db7bff1852fa7501300b3778bd218dc631f7c704f6c1db3dd8"


def verified_unpacked():
    verify_module("fmod.dll")
    path = ROOT / "work/unpacked/fmod.dll"
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != EXPECTED:
        raise RuntimeError("FMOD unpacked input missing/changed; run tools/unpack_audio.py")
    return path


def main():
    original = verify_module("fmod.dll")
    upx = shutil.which("upx") or "/opt/homebrew/bin/upx"
    out = ROOT / "work/unpacked"
    out.mkdir(parents=True, exist_ok=True)
    staged = out / "fmod.new.dll"
    staged.unlink(missing_ok=True)
    subprocess.run([upx, "-d", str(original), "-o", str(staged)], check=True)
    if hashlib.sha256(staged.read_bytes()).hexdigest() != EXPECTED:
        staged.unlink()
        raise RuntimeError("Unexpected UPX output; refusing an unverified audio image")
    pe = pefile.PE(str(staged))
    if any(s.Name.startswith(b"UPX") for s in pe.sections):
        raise RuntimeError("Audio image remains packed")
    staged.replace(out / "fmod.dll")
    report = dict(sha256=EXPECTED, tool=subprocess.check_output([upx, "--version"], text=True).splitlines()[0],
                  imports={d.dll.decode(): [i.name.decode() if i.name else f"#{i.ordinal}" for i in d.imports]
                           for d in pe.DIRECTORY_ENTRY_IMPORT},
                  exports=len(pe.DIRECTORY_ENTRY_EXPORT.symbols), gameplay_tested=False)
    (ROOT / "work/reports/audio-unpacked.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified unpacked audio: {sum(map(len, report['imports'].values()))} imports")


if __name__ == "__main__":
    main()
