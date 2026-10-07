"""Release-only extraction of checked observation weights, never private replay."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()


def prepare(sources, output):
    output.mkdir(parents=True,exist_ok=True)
    entries=[]
    for profile,store in sources:
        ref=json.loads((store/'active.json').read_text())['generation']
        if not re.fullmatch(r'generation-[0-9a-f]{32}',ref):raise ValueError('Invalid generation')
        folder=store/ref;meta=json.loads((folder/'manifest.json').read_text())
        if meta['profile']!=profile or not meta['accepted'] or meta.get('may_activate_in_game') is not False:
            raise ValueError('Factory must be an accepted offline observation generation')
        for name,digest in meta['files'].items():
            if Path(name).name!=name or sha(folder/name)!=digest:raise ValueError('Generation integrity failure')
        weight_hash=sha(folder/'weights.safetensors')
        key=profile+'-'+weight_hash[:24]
        target=output/key;target.mkdir(exist_ok=True)
        shutil.copyfile(folder/'weights.safetensors',target/'weights.safetensors')
        maps=sorted(k for k in meta['validation'] if k!='all')
        if any(not re.fullmatch(r'[a-zA-Z0-9_-]{1,64}',name) for name in maps):raise ValueError('Invalid map name')
        public=dict(id=key,profile=profile,feature_version=meta['feature_version'],context=meta['context'],donor=meta['donor'],
            maps=maps,scope='offline_observation_only',may_activate_in_game=False,
            qualified_observation_heads=meta['qualified_observation_heads'],
            validation=meta['validation'],test=meta['test'],source_sha256=meta['source_sha256'],
            weights_sha256=weight_hash,license='MIT',replay_included=False)
        if public['donor'] is not None:raise ValueError('Factory must not contain private donor identity')
        (target/'model.json').write_text(json.dumps(public,indent=2,allow_nan=False)+'\n',encoding='utf8')
        entries.append(dict(id=key,profile=profile,context=meta['context'],maps=maps,
            metadata_sha256=sha(target/'model.json'),weights_sha256=weight_hash))
    (output/'catalog.json').write_text(json.dumps(dict(schema='bts-factory-models-v1',game_installable=False,models=entries),indent=2)+'\n',encoding='utf8')
    print(json.dumps(dict(profiles=[e['profile'] for e in entries],maps=maps,bytes=sum(f.stat().st_size for f in output.rglob('*') if f.is_file()))))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',action='append',required=True,help='profile=accepted temporal store')
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    prepare([(profile,Path(path)) for profile,path in (item.split('=',1) for item in args.source)],args.out)
