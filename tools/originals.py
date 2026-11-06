"""The original Brutal Half-Life v3 release, read from its ZIP or from the kept originals.

install.sh keeps the few original files the build reads (the mod's three game DLLs and its stock config.cfg) in
<data>/game-files with their checksums, so updates rebuild without the ZIP and the ZIP can be deleted after the
first install. The rest of the release is already installed in <data>/bhl. Only a new data folder or a development
session (--check) needs the whole ZIP again.

    originals.py keep <bhl_v3.zip> <folder>   extracts the kept originals into <folder> (checked, atomic)
"""
import hashlib
import json
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from inputs import PROFILE

ARCHIVE_SHA256 = PROFILE['archive_sha256']
MANIFEST = 'originals.json'
KEPT = ['config.cfg'] + [spec['member'] for spec in PROFILE['modules'].values() if 'member' in spec]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


class Originals:
    """A verified source of the release's files: the ZIP itself, or a folder that keep() wrote."""

    def __init__(self, path):
        self.path = Path(path).expanduser().resolve()
        if self.path.is_dir():
            manifest = self.path/MANIFEST
            if not manifest.is_file():
                raise RuntimeError(f'{self.path} is not a folder of kept Brutal Half-Life v3 originals (no {MANIFEST})')
            record = json.loads(manifest.read_text())
            if record.get('archive_sha256') != ARCHIVE_SHA256:
                raise RuntimeError(f'{self.path} holds files from another release than the pinned BHL v3')
            for member in KEPT:
                expected = record.get('files', {}).get(member)
                if expected is None or sha256(self.member_path(member)) != expected:
                    raise RuntimeError(f'Kept original {member} in {self.path} is missing or changed')
            self.zip = None
        elif self.path.is_file():
            if sha256(self.path) != ARCHIVE_SHA256:
                raise RuntimeError('Archive does not match the original BHL v3 release')
            self.zip = self.path
        else:
            raise RuntimeError(f'No Brutal Half-Life v3 archive or kept originals at {self.path}')

    def member_path(self, member):
        return self.path.joinpath(*PurePosixPath(member).parts)

    def read(self, member):
        if self.zip:
            with zipfile.ZipFile(self.zip) as archive:
                return archive.read(member)
        if member not in KEPT:
            raise RuntimeError(f'{member} is not among the kept originals in {self.path}')
        return self.member_path(member).read_bytes()

    def archive(self, purpose):
        """The whole ZIP, for what needs every file of the release."""
        if not self.zip:
            raise RuntimeError(f'{purpose} needs the original bhl_v3.zip (pass --archive /path/to/bhl_v3.zip): '
                               f'{self.path} keeps only the files a rebuild reads')
        return self.zip

    def keep(self, folder):
        """Write the kept originals and their manifest into folder; an up-to-date folder is left as it is."""
        folder = Path(folder).expanduser().absolute()
        try:
            Originals(folder)
            return folder
        except RuntimeError:
            pass
        folder.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=folder.name+'.stage-', dir=folder.parent))
        try:
            files = {}
            for member in KEPT:
                payload = self.read(member)
                out = staging.joinpath(*PurePosixPath(member).parts)
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(payload)
                files[member] = hashlib.sha256(payload).hexdigest()
            for name, spec in PROFILE['modules'].items():
                if 'member' in spec and files[spec['member']] != spec['sha256']:
                    raise RuntimeError(f'{name}: unsupported input SHA256 {files[spec["member"]]}')
            (staging/MANIFEST).write_text(json.dumps(dict(
                format=1, release=PROFILE['release'], archive_sha256=ARCHIVE_SHA256, files=files), indent=2)+'\n')
            if folder.exists():
                shutil.rmtree(folder)
            staging.rename(folder)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
        Originals(folder)
        return folder


def main():
    if len(sys.argv) != 4 or sys.argv[1] != 'keep':
        raise SystemExit('usage: originals.py keep <bhl_v3.zip> <folder>')
    print(f'Kept the originals a rebuild needs in {Originals(sys.argv[2]).keep(sys.argv[3])}')


if __name__ == '__main__':
    main()
