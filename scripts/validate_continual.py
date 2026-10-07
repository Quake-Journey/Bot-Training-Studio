"""Real-map sequential learning with immutable source datasets and retention."""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import sequences, temporal
from opentdm_x_trainer.data import sha
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--dataset',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True);ap.add_argument('--maps',nargs='+',required=True)
    ap.add_argument('--backend',choices=['cpu','cuda'],default='cpu')
    ap.add_argument('--epochs',type=int,default=20);args=ap.parse_args()
    data,meta=sequences.load(args.dataset);results=[]
    for i,name in enumerate(args.maps):
        folder=args.output/name;folder.mkdir(parents=True)
        arrays={}
        for split in ('train','validation','test'):
            mask=data[split+'_map']==name
            if not mask.any():raise ValueError('Map has no independent '+split+' observations')
            arrays.update({split+'_'+field:data[split+'_'+field][mask] for field in sequences.FIELDS})
        np.savez_compressed(folder/'samples.npz',**arrays)
        atomic_json(folder/'manifest.json',dict(meta,samples_sha256=sha(folder/'samples.npz'),
            splits={s:dict(samples=len(arrays[s+'_x']),groups=len(set(arrays[s+'_group']))) for s in ('train','validation','test')}))
        result=temporal.train(folder,args.output/'model',backend=args.backend,epochs=args.epochs,batch_size=128,
            mode='fresh' if i==0 else 'update',event=lambda r:print(json.dumps(dict(r,map=name)),flush=True))
        atomic_json(args.output/(name+'.json'),result);results.append(result)
        if not result['accepted']:break
    atomic_json(args.output/'summary.json',dict(results=results,complete=len(results)==len(args.maps) and all(r['accepted'] for r in results)))
    print([(r['accepted'],r['failures']) for r in results],flush=True)


if __name__=='__main__':main()
