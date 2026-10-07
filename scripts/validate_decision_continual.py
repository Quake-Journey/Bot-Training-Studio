"""Fixed q2duel5 -> ztn2dm3 update with old-map retention, never game activation."""
import argparse
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import decisions as ds,decision_learning as learn
from opentdm_x_trainer.data import sha
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--dataset',required=True,type=Path)
    ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--backend',default='cpu')
    ap.add_argument('--epochs',default=12,type=int)
    a=ap.parse_args();data,meta=ds.load(a.dataset)
    if a.output.exists():raise ValueError('Use a new experiment directory')
    a.output.mkdir(parents=True);reports=[]
    for index,name in enumerate(('q2duel5','ztn2dm3')):
        part={}
        for split in ('train','validation','test'):
            mask=data[split+'_map']==name
            if not mask.any():raise ValueError('Missing '+name+' '+split)
            for field in ds.FIELDS:part[split+'_'+field]=data[split+'_'+field][mask]
        ds.validate(part,meta['context'])
        folder=a.output/name;folder.mkdir()
        np.savez_compressed(folder/'samples.npz',**part)
        child=dict(meta,samples_sha256=sha(folder/'samples.npz'),parent_dataset_sha256=meta['samples_sha256'],
            splits={s:dict(samples=len(part[s+'_x']),groups=len(set(part[s+'_group']))) for s in ('train','validation','test')})
        atomic_json(folder/'manifest.json',child)
        def event(row):
            if row.get('stage')=='decision_training':print(name+' '+str(row),flush=True)
        result=learn.train(folder,a.output/'store',backend=a.backend,profile='compact',
            mode='fresh' if index==0 else 'update',epochs=a.epochs,event=event)
        atomic_json(a.output/(name+'.json'),result);reports.append(dict(map=name,result=result))
    atomic_json(a.output/'summary.json',dict(dataset_sha256=meta['samples_sha256'],runs=reports,
        scope='Observed decisions; fixed map order; no server activation'))
    print(str(a.output/'summary.json'),flush=True)


if __name__=='__main__':main()
