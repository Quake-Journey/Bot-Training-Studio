"""Build an isolated Windows runtime for distribution; never modify system Python.

The published artifact vendors its wheels as CPython's embedding docs advise.
Package installation happens on the build machine only, not on a user's server.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import urllib.request
import zipfile

PYTHON_URL='https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip'
PYTHON_SHA='97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297'
ROOT=Path(__file__).resolve().parents[1]


def bundle(out,backend):
    if sys.platform!='win32':raise ValueError('This builder targets Windows x64')
    if out.exists():raise ValueError('Choose a new runtime directory; active runtimes are never overwritten')
    out.mkdir(parents=True)
    archive=out/'python-runtime.zip'
    with urllib.request.urlopen(PYTHON_URL,timeout=60) as response,archive.open('xb') as file:
        size=0
        while block:=response.read(1024**2):
            size+=len(block)
            if size>32*1024**2:raise ValueError('Unexpected Python archive size')
            file.write(block)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=PYTHON_SHA:raise ValueError('CPython archive digest mismatch')
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            if Path(name).is_absolute() or '..' in Path(name).parts:raise ValueError('Invalid runtime archive path')
        z.extractall(out)
    # An isolated embedded interpreter ignores global PYTHONPATH and user site.
    (out/'python313._pth').write_text('python313.zip\n.\nLib/site-packages\n../worker\nimport site\n',encoding='utf8')
    target=out/'Lib/site-packages'
    channel='cpu' if backend=='cpu' else 'cu128'
    command=[sys.executable,'-m','pip','install','--disable-pip-version-check','--only-binary=:all:',
        '--platform','win_amd64','--python-version','3.13','--implementation','cp','--abi','cp313',
        '--target',str(target),'--report',str(out/'pip-report.json')]
    lock=ROOT/'packaging'/('runtime-'+backend+'-win-x64.lock.json')
    if lock.exists():
        pinned=json.loads(lock.read_text())
        if pinned['python_sha256']!=PYTHON_SHA or pinned['backend']!=backend:raise ValueError('Runtime lock mismatch')
        requirements=out/'locked-requirements.txt'
        requirements.write_text('\n'.join(r['name']+' @ '+r['source']['url']+' --hash=sha256:'+r['source']['archive_info']['hashes']['sha256'] for r in pinned['packages'])+'\n',encoding='utf8')
        command+=['--no-deps','--require-hashes','-r',str(requirements)]
    else:
        command+=['--index-url','https://pypi.org/simple','--extra-index-url','https://download.pytorch.org/whl/'+channel,
            '-r',str(ROOT/'worker/requirements.txt'),'torch==2.10.0+'+channel]
    subprocess.run(command,check=True)
    # Verify the exact packaged interpreter, including its native dependencies.
    code="import json,torch,numpy,pyarrow,safetensors,sklearn,rarfile; print(json.dumps({'torch':torch.__version__,'cuda':torch.cuda.is_available(),'numpy':numpy.__version__}))"
    run=subprocess.run([str(out/'python.exe'),'-I','-c',code],check=True,capture_output=True,text=True)
    report=json.loads((out/'pip-report.json').read_text())
    packages=[dict(name=r['metadata']['name'],version=r['metadata']['version'],source=r['download_info']) for r in report['install']]
    receipt=dict(schema=1,backend=backend,python_url=PYTHON_URL,python_sha256=PYTHON_SHA,
        packages=packages,probe=json.loads(run.stdout),runtime_qualified=False)
    (out/'runtime.json').write_text(json.dumps(receipt,indent=2),encoding='utf8')
    print(json.dumps(dict(runtime=str(out),probe=receipt['probe'],packages=len(packages))))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--backend',choices=['cpu','cuda'],default='cpu')
    args=parser.parse_args();bundle(args.out.resolve(),args.backend)
