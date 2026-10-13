"""Fetch the standalone ARM64 ANGLE libraries from a pinned official Electron build.

Only ANGLE and its license notices are retained; Electron is not run or bundled.
"""
import hashlib
import json
import os
import urllib.request
import zipfile
from inputs import ROOT

VERSION = '43.7.7'
ARCHIVE = f'electron-v{VERSION}-darwin-arm64.zip'
SHA256 = '9327d8ba5bc9e279d1a2f7da90235301c65a2e80eb4ad3bc5610d28d483340f9'
URL = f'https://github.com/electron/electron/releases/download/v{VERSION}/{ARCHIVE}'


def fetch():
    destination = ROOT/'work/angle'
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination/ARCHIVE
    if not archive.exists():
        staged = archive.with_suffix('.download')
        urllib.request.urlretrieve(URL, staged)
        if hashlib.sha256(staged.read_bytes()).hexdigest() != SHA256:
            staged.unlink()
            raise RuntimeError('ANGLE archive checksum mismatch')
        staged.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Cached ANGLE archive checksum mismatch')
    files = {}
    with zipfile.ZipFile(archive) as source:
        for name in ('libEGL.dylib', 'libGLESv2.dylib', 'LICENSE', 'LICENSES.chromium.html'):
            matches = [p for p in source.namelist() if p.rsplit('/', 1)[-1] == name]
            if len(matches) != 1:
                raise RuntimeError(f'Expected one {name}, found {matches}')
            payload = source.read(matches[0])
            staged = destination/(name+'.stage')
            staged.write_bytes(payload)
            if name.endswith('.dylib'):
                os.chmod(staged, 0o755)
            staged.replace(destination/name)
            files[name] = hashlib.sha256(payload).hexdigest()
    (destination/'manifest.json').write_text(json.dumps(dict(
        source=URL, archive_sha256=SHA256, files=files), indent=2)+'\n')
    print(f'ANGLE ARM64 libraries ready in {destination}')
    return destination


if __name__ == '__main__':
    fetch()
