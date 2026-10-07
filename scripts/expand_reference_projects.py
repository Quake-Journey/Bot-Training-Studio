"""Bounded local corpus expansion, preserving revisions and recording provenance."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer.projects import inventory,import_project
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--project',type=Path,required=True);ap.add_argument('--references',type=Path,required=True)
    ap.add_argument('--additional',type=int,default=32);ap.add_argument('--receipt',type=Path,required=True)
    a=ap.parse_args()
    if not 1<=a.additional<=200:raise ValueError('Use 1..200 additional recordings per batch')
    meta=json.loads((a.project/'project.json').read_text());name=meta['map']
    rows=inventory([a.references],name)
    previous={(str(Path(r['source']['path']).resolve()),r['source'].get('member')) for r in meta['recordings']}
    candidates=[]
    for r in rows:
        key=(str(Path(r['path']).resolve()),r.get('member'))
        if key in previous:continue
        # Trick sessions use a separate workflow, not ordinary duel supervision.
        if any('trick' in part.casefold() for part in Path(r['path']).parts):continue
        candidates.append(r)
    # Stable selection without looking at results, winners or holdout labels.
    candidates.sort(key=lambda r:hashlib.sha256((Path(r['path']).name+'|'+str(r.get('member'))).encode()).digest())
    selected=candidates[:a.additional];tick=[0.]
    def progress(row):
        if shutil.disk_usage(a.project).free<20*1024**3:
            raise OSError('Corpus expansion stopped before consuming the 20 GiB free-space reserve')
        if time.monotonic()-tick[0]>15:
            print(json.dumps(row),flush=True);tick[0]=time.monotonic()
    result=import_project(a.project/'map.bsp',[],a.project,selected=selected,event=progress)
    a.receipt.parent.mkdir(parents=True,exist_ok=True)
    atomic_json(a.receipt,dict(result,selected=selected,inventory=len(rows),remaining_candidates=len(candidates)-len(selected)))
    print(json.dumps(result),flush=True)


if __name__=='__main__':main()
