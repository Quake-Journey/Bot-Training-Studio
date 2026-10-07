"""Cache the pinned official CPython archive at build time, never at user setup."""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def fetch():
    lock = json.loads((ROOT / 'packaging/runtime-cpu-win-x64.lock.json').read_text())
    target = ROOT / 'dist/python-embed' / lock['python_url'].rsplit('/', 1)[-1]
    target.parent.mkdir(parents=True, exist_ok=True)
    def digest(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    if not target.exists() or digest(target) != lock['python_sha256']:
        temporary = target.with_suffix('.download')
        try:
            with urllib.request.urlopen(lock['python_url'], timeout=90) as response, temporary.open('wb') as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            if digest(temporary) != lock['python_sha256']:
                raise ValueError('Official CPython archive SHA256 mismatch')
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    print(target)


if __name__ == '__main__':
    fetch()
