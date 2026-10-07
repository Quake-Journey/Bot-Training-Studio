"""Publish only a verified release asset manifest, using Git's credential helper.

No credentials are stored or printed. Uploads stay in a draft until every asset
has a matching GitHub SHA-256 digest, size and uploaded state.
"""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REPO = 'Quake-Journey/Bot-Training-Studio'
ENV = dict(os.environ, GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='never')


def git(*args):
    p = subprocess.run(['git', *args], cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise RuntimeError('Git operation failed: '+args[0])
    return p.stdout.strip()


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--assets',type=Path,required=True)
    parser.add_argument('--notes',type=Path,required=True)
    parser.add_argument('--publish',action='store_true')
    args=parser.parse_args()
    folder=args.assets.resolve().parent
    manifest=json.loads(args.assets.read_text(encoding='utf-8'))
    version=manifest['version']; tag='v'+version
    assets=manifest['assets']
    for asset in assets:
        if Path(asset['name']).name != asset['name']:
            raise ValueError('Invalid asset name')
        file=folder/asset['name']
        if file.stat().st_size != asset['size'] or sha(file)!=asset['sha256']:
            raise ValueError('Asset mismatch: '+asset['name'])
    if len({a['name'] for a in assets}) != len(assets): raise ValueError('Duplicate assets')
    if not args.publish:
        print(json.dumps(dict(verified=True,version=version,assets=len(assets))));return
    if git('status','--porcelain'): raise RuntimeError('Commit and verify source before publication')
    if git('remote','get-url','origin') != 'https://github.com/'+REPO+'.git':
        raise RuntimeError('Unexpected origin')
    head=git('rev-parse','HEAD')
    credentials=subprocess.run(['git','credential','fill'],input='protocol=https\nhost=github.com\n\n',
        cwd=ROOT,env=ENV,text=True,capture_output=True,timeout=30)
    fields=dict(line.split('=',1) for line in credentials.stdout.splitlines() if '=' in line)
    if credentials.returncode or not fields.get('password'):
        raise RuntimeError('GitHub authentication unavailable; credential output withheld')
    token=fields['password']
    headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json',
        'User-Agent':'BotTrainingStudio-release','X-GitHub-Api-Version':'2022-11-28'}

    def api(path,method='GET',body=None):
        req=urllib.request.Request('https://api.github.com/repos/'+REPO+path,
            data=None if body is None else json.dumps(body).encode(),method=method,
            headers=dict(headers,**{'Content-Type':'application/json'}))
        try:
            with urllib.request.urlopen(req,timeout=90) as response:return json.load(response)
        except urllib.error.HTTPError as error:
            raise RuntimeError('GitHub API HTTP '+str(error.code)) from None

    if api('/commits/main')['sha']!=head: raise RuntimeError('Push verified source first')
    if tag not in git('tag','--list',tag).splitlines():git('tag',tag,head)
    if git('rev-parse',tag)!=head:raise RuntimeError('Existing tag identifies different source')
    git('push','origin','refs/tags/'+tag)
    releases=api('/releases?per_page=100')
    found=[r for r in releases if r['tag_name']==tag]
    release=found[0] if found else api('/releases','POST',dict(tag_name=tag,target_commitish=head,
        name='Bot Training Studio '+version,body=args.notes.read_text(encoding='utf-8'),
        draft=True,prerelease='-' in version))
    if not release['draft']:raise RuntimeError('Published versions are immutable; use a new version')
    release=api('/releases/'+str(release['id']),'PATCH',dict(body=args.notes.read_text(encoding='utf-8')))
    existing={a['name']:a for a in api('/releases/'+str(release['id'])+'/assets?per_page=100')}
    wanted={a['name'] for a in assets}
    if set(existing)-wanted:raise RuntimeError('Draft contains unexpected assets')

    def matches(remote,asset):
        return remote.get('state')=='uploaded' and remote['size']==asset['size'] and remote.get('digest')=='sha256:'+asset['sha256']

    def upload(asset):
        old=existing.get(asset['name'])
        if old:
            if matches(old,asset):return asset['name']
            raise RuntimeError('Draft asset differs: '+asset['name'])
        path='/repos/'+REPO+'/releases/'+str(release['id'])+'/assets?name='+urllib.parse.quote(asset['name'])
        connection=http.client.HTTPSConnection('uploads.github.com',timeout=180)
        try:
            connection.putrequest('POST',path)
            for key,value in headers.items():connection.putheader(key,value)
            connection.putheader('Content-Type','application/octet-stream')
            connection.putheader('Content-Length',str(asset['size']));connection.endheaders()
            sent=0;last=time.monotonic()
            with (folder/asset['name']).open('rb') as stream:
                while block:=stream.read(1024*1024):
                    connection.send(block);sent+=len(block)
                    if time.monotonic()-last>25:
                        print(json.dumps(dict(upload=asset['name'],percent=round(sent*100/asset['size']))),flush=True);last=time.monotonic()
            response=connection.getresponse()
            if response.status!=201:raise RuntimeError('Asset upload HTTP '+str(response.status))
            result=json.load(response)
            if not matches(result,asset):raise RuntimeError('Uploaded asset verification failed: '+asset['name'])
            print(json.dumps(dict(uploaded=asset['name'],verified=True)),flush=True)
            return asset['name']
        finally:connection.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(upload,assets))
    final={a['name']:a for a in api('/releases/'+str(release['id'])+'/assets?per_page=100')}
    if set(final)!=wanted or any(not matches(final[a['name']],a) for a in assets):
        raise RuntimeError('Final draft verification failed')
    if api('/git/ref/tags/'+tag)['object']['sha']!=head:raise RuntimeError('Remote tag mismatch')
    published=api('/releases/'+str(release['id']),'PATCH',dict(draft=False))
    receipt=dict(version=version,source=head,url=published['html_url'],assets=len(assets),
        bytes=sum(a['size'] for a in assets),sha256_verified=True,prerelease=published['prerelease'])
    (folder/'publication.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps(receipt),flush=True)


if __name__=='__main__':
    main()
