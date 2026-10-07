"""Fixed group folds drawn ONLY from the training partition.

The original validation/test recordings are excluded from this experiment.
Every fold contains independent whole-match groups for both maps. No frame
shuffle or best-fold selection. Results are descriptive, not gameplay proof.
"""
import argparse
import hashlib
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import sequences, temporal
from opentdm_x_trainer.data import sha
from opentdm_x_trainer.learning import atomic_json


def fold_assignments(data, folds):
    assignments={}
    for name in sorted(set(data['train_map'])):
        groups=sorted(set(data['train_group'][data['train_map']==name]),
                      key=lambda s:hashlib.sha256(str(s).encode()).digest())
        if len(groups)<folds:raise ValueError('Not enough independent training groups for '+name)
        for i,group in enumerate(groups):
            if group in assignments and assignments[group]!=i%folds:
                raise ValueError('Group occurs in conflicting maps')
            assignments[group]=i%folds
    return assignments


def prepare_fold(data, meta, assignments, index, folds, out):
    assignment=np.asarray([assignments[g] for g in data['train_group']])
    masks=dict(train=(assignment!=index)&(assignment!=(index+1)%folds),
               validation=assignment==index,test=assignment==(index+1)%folds)
    arrays={split+'_'+field:data['train_'+field][mask] for split,mask in masks.items() for field in sequences.FIELDS}
    out.mkdir(parents=True)
    np.savez_compressed(out/'samples.npz',**arrays)
    doc=dict(meta,samples_sha256=sha(out/'samples.npz'),
        internal_training_group_fold=index,original_holdouts_used=False,
        splits={s:dict(samples=int(m.sum()),groups=len(set(data['train_group'][m]))) for s,m in masks.items()})
    atomic_json(out/'manifest.json',doc)


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--dataset',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True);ap.add_argument('--folds',type=int,default=5)
    ap.add_argument('--epochs',type=int,default=20);ap.add_argument('--backend',choices=['cuda','cpu'],default='cpu')
    args=ap.parse_args()
    if not 3<=args.folds<=10:raise ValueError('Use 3..10 folds')
    data,meta=sequences.load(args.dataset);assignments=fold_assignments(data,args.folds)
    results=[]
    for index in range(args.folds):
        out=args.output/str(index)
        prepare_fold(data,meta,assignments,index,args.folds,out/'dataset')
        result=temporal.train(out/'dataset',out/'model',backend=args.backend,epochs=args.epochs,batch_size=128,
            event=lambda r:print(f"fold={index} epoch={r.get('epoch')} score={r.get('validation_score')}",flush=True))
        atomic_json(out/'result.json',result);results.append(result)
    summary={}
    for name in sorted(set(data['train_map'])):
        summary[name]={}
        for event in sequences.EVENTS:
            rows=[r['test'][name]['events'][event] for r in results]
            summary[name][event]=dict(folds=len(rows),brier_wins=sum(r['brier']<r['prior_brier'] for r in rows),
                brier_mean=float(np.mean([r['brier'] for r in rows])),baseline_mean=float(np.mean([r['prior_brier'] for r in rows])),
                positives=[r['positive'] for r in rows])
    atomic_json(args.output/'summary.json',dict(folds=args.folds,summary=summary,original_holdouts_used=False,
        scope='Fixed observational group folds, not a causal gameplay evaluation'))
    print(summary,flush=True)


if __name__=='__main__':main()
