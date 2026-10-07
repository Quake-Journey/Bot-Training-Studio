"""Reimport cached observations and compare independent shot/pickup models."""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import projects,decisions,decision_learning
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--projects',nargs='+',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--reimport',action='store_true')
    ap.add_argument('--profiles',nargs='+',default=['compact'])
    ap.add_argument('--backend',default='cpu',choices=['cpu','cuda'])
    ap.add_argument('--context',type=int,default=16)
    ap.add_argument('--limit',type=int,default=6000)
    ap.add_argument('--epochs',type=int,default=20)
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True);tick=[0.]
    def progress(row):
        if time.monotonic()-tick[0]>15 or row.get('stage')=='decision_training':
            print(json.dumps(row),flush=True);tick[0]=time.monotonic()
    sources=[]
    for folder in a.projects:
        if a.reimport:print(json.dumps(projects.import_project(folder/'map.bsp',[],folder,event=progress)),flush=True)
        meta=json.loads((folder/'project.json').read_text());rev=folder/'revisions'/meta['active_revision']
        sources.append(dict(combat=rev/'combat.parquet',groups=rev/'groups.json',bsp=folder/'map.bsp',map=meta['map']))
    dataset=a.output/'dataset'
    if not dataset.exists():
        meta=decisions.prepare(sources,dataset,context=a.context,limit=a.limit,event=progress)
        print(json.dumps(meta['splits']),flush=True)
    for profile in a.profiles:
        result=decision_learning.train(dataset,a.output/profile,profile=profile,backend=a.backend,epochs=a.epochs,event=progress)
        atomic_json(a.output/(profile+'.json'),result);print(json.dumps(result),flush=True)


if __name__=='__main__':main()
