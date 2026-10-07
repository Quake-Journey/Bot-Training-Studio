"""Conservative candidate holdouts: keep an entire named opponent pair together.

This deliberately holds out more than one match. It cannot certify cross-demo
identity when a player used unrelated aliases; ambiguous identities quarantine.
"""
import argparse,hashlib,json,re
from collections import Counter,defaultdict
from pathlib import Path
from .analyzer import atomic_json,io_path

KNOWN={'damiah':'damiah','>purri':'purri','purri':'purri','david':'david','syanid':'syanid'}

def identity(aliases):
    names=set()
    for name in aliases:
        name=''.join(chr(ord(ch)&127) if ord(ch)<256 else ch for ch in name)
        name=re.sub(r'\^[0-9]','',name).strip().casefold()
        if name:names.add(KNOWN.get(name,name))
    return next(iter(names)) if len(names)==1 else None

def donor_name(aliases):
    """Beta 9 (--donor-identity): a recognised donor by its decorated
    aliases; otherwise the old single normalised name; a track whose name
    changes to something unrelated is 'unresolved' - a stable placeholder, so
    a well-identified donor's match is not quarantined because the OPPONENT
    renamed himself, and both POVs of it still land in one group."""
    from opentdm_x_trainer.data import nickname
    names = {nickname(value) for value in aliases or [] if nickname(value)}
    return next(iter(names)) if len(names) == 1 else 'unresolved'

def groups(snapshot,data,name=None):
    name=name or identity
    rows=[];counts=Counter()
    for record in snapshot['records']:
        if record['status']!='decoded':continue
        facts=json.loads((data/record['path']/'facts.json').read_text(encoding='utf-8'))
        tracks={f['track_id']:f for f in facts if f['kind']=='track'}
        segs={f['segment']:f for f in facts if f['kind']=='segment_facts'}
        parts=defaultdict(list)
        for f in facts:
            if f['kind']=='participant' and (f['active'] or f.get('kills',0)>0 or f.get('deaths',0)>0):
                parts[f['epoch_id']].append(f)
        for epoch in (f for f in facts if f['kind']=='epoch'):
            players=[tracks.get(p['track_id'],{}) for p in parts[epoch['epoch_id']]]
            names=[name(p.get('aliases',[])) for p in players]
            eligible=epoch['start_conf']==2 and len(names)==2 and all(names) and len(set(names))==2 and names!=['unresolved']*2
            pair=sorted(names) if eligible else []
            key=json.dumps([segs[epoch['segment']]['map'],pair],separators=(',',':'))
            group=hashlib.sha256(key.encode()).hexdigest() if eligible else None
            # Stable across input renames, new recordings and changed frame
            # counts. Every match of the same pair has the same assignment.
            bucket=int(group[:8],16)%10 if group else -1
            split='quarantine' if not eligible else 'validation' if bucket==8 else 'test' if bucket==9 else 'train'
            rows.append(dict(recording_id=record['recording_id'],segment=epoch['segment'],epoch_id=epoch['epoch_id'],
                map=segs[epoch['segment']]['map'],pair=pair,group=group,split=split))
            counts[split]+=1
    return rows,counts

def main(a):
    data=io_path(a.data);snapshot=json.loads((data/'current.json').read_text(encoding='utf-8'))
    rows,counts=groups(snapshot,data,donor_name if a.donor_identity else None)
    report=dict(schema=1,source_run=snapshot['run_id'],method='all matches of the same normalized named pair stay together',
        transform_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),groups=rows,counts=dict(counts),
        unique_pairs=len({r['group'] for r in rows if r['group']}),qualified_split=False,
        limits=['Name evidence is not certified real-person identity; unrelated alias changes may require merging groups.',
                'Empty or ambiguous identities and non-duel participant sets are quarantined.',
                'No frame-random split; paired POVs with consistent names cannot cross partitions.',
                'Per-style/weapon holdout coverage must be measured before accuracy claims.'])
    a.out=io_path(a.out);a.out.parent.mkdir(parents=True,exist_ok=True)
    if a.out.exists():raise ValueError('Use a new evidence output path')
    atomic_json(a.out,report)
    print(json.dumps({k:report[k] for k in ('source_run','counts','unique_pairs')}))

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--data',required=True,type=Path);ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--donor-identity',action='store_true')
    main(ap.parse_args())
