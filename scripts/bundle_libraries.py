"""Prepare end-user libraries from a qualified builder runtime, without shipping its Python."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def bundle(runtime: Path, output: Path):
    runtime, output = runtime.resolve(), output.resolve()
    if output.exists():
        raise ValueError('Choose a new library directory; active packages are never overwritten')
    receipt = json.loads((runtime / 'runtime.json').read_text(encoding='utf8'))
    backend = receipt['backend']
    root = Path(__file__).resolve().parents[1]
    lock = json.loads((root/'packaging'/f'runtime-{backend}-win-x64.lock.json').read_text())
    if receipt['python_sha256'] != lock['python_sha256'] or receipt['packages'] != lock['packages']:
        raise ValueError('Builder runtime does not match pinned package manifest')
    source = runtime / 'Lib' / 'site-packages'
    shutil.copytree(source, output, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    code = "import sys,json; sys.path.insert(0,sys.argv[1]); import torch,numpy,pyarrow,safetensors,sklearn,packaging,rarfile; x=torch.tensor([2.0],requires_grad=True); (x*x).sum().backward(); assert x.grad.item()==4; print(json.dumps(dict(python=sys.version.split()[0],torch=torch.__version__,cuda=torch.cuda.is_available())))"
    probe = subprocess.run([str(runtime/'python.exe'), '-I', '-S', '-c', code, str(output)], check=True, text=True, capture_output=True)
    files = list(p for p in output.rglob('*') if p.is_file())
    metadata = dict(schema=1, backend=backend, python_minor='3.13', packages=lock['packages'],
        bytes=sum(p.stat().st_size for p in files), files=len(files), probe=json.loads(probe.stdout), python_included=False)
    (output/'studio-libraries.json').write_text(json.dumps(metadata, indent=2), encoding='utf8')
    print(json.dumps({k: metadata[k] for k in ['backend','bytes','files','probe','python_included']}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    bundle(args.runtime, args.out)
