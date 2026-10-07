"""Reproducible offline outcome experiment. Paths are supplied by the local user."""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import projects, sequences, temporal
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--projects',nargs='+',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--reimport',action='store_true')
    ap.add_argument('--profiles',nargs='+',default=['compact','balanced'])
    ap.add_argument('--backend',default='cpu',choices=['cpu','cuda'])
    ap.add_argument('--limit',type=int,default=6000)
    ap.add_argument('--epochs',type=int,default=20)
    args=ap.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    tick=[0.]
    def progress(event):
        if time.monotonic()-tick[0]>15 or event.get('stage')=='temporal_training':
            print(json.dumps(event),flush=True);tick[0]=time.monotonic()
    sources=[]
    for project in args.projects:
        if args.reimport:
            receipt=projects.import_project(project/'map.bsp',[],project,event=progress)
            print(json.dumps(receipt),flush=True)
        meta=json.loads((project/'project.json').read_text())
        revision=project/'revisions'/meta['active_revision']
        sources.append(dict(combat=str(revision/'combat.parquet'),groups=str(revision/'groups.json'),
            bsp=str(project/'map.bsp'),map=meta['map']))
    dataset=args.output/'dataset'
    if not dataset.exists():
        result=sequences.prepare(sources,dataset,limit=args.limit,event=progress)
        print(json.dumps(dict(splits=result['splits'],counts=result['counts'])),flush=True)
    results={}
    for profile in args.profiles:
        store=args.output/profile
        if (store/'active.json').exists():raise ValueError('Use a fresh experiment output to avoid overwriting results')
        result=temporal.train(dataset,store,profile=profile,backend=args.backend,epochs=args.epochs,
            batch_size=128,event=progress)
        results[profile]=result
        atomic_json(args.output/(profile+'.json'),result)
        print(json.dumps(result),flush=True)
    atomic_json(args.output/'comparison.json',dict(results=results,
        scope='Observation models, not causal tactics or online qualification. Test groups never select epochs.'))


if __name__=='__main__':main()
