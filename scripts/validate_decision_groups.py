"""Fixed train-only group folds for shot/pickup heads. Never choose a best fold."""
import argparse
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
sys.path.insert(0,str(Path(__file__).resolve().parent))
from opentdm_x_trainer import decisions as ds, decision_learning as learn
from opentdm_x_trainer.data import sha
from opentdm_x_trainer.learning import atomic_json
from validate_outcome_groups import fold_assignments


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--dataset',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--folds',type=int,default=5);ap.add_argument('--epochs',type=int,default=20)
    ap.add_argument('--backend',choices=['cpu','cuda'],default='cpu')
    a=ap.parse_args()
    if not 3<=a.folds<=10:raise ValueError('Use 3..10 fixed folds')
    data,meta=ds.load(a.dataset);assignments=fold_assignments(data,a.folds)
    assignment=np.asarray([assignments[g] for g in data['train_group']]);results=[]
    for i in range(a.folds):
        out=a.output/str(i);folder=out/'dataset';folder.mkdir(parents=True,exist_ok=False)
        masks=dict(train=(assignment!=i)&(assignment!=(i+1)%a.folds),validation=assignment==i,test=assignment==(i+1)%a.folds)
        arrays={s+'_'+f:data['train_'+f][mask] for s,mask in masks.items() for f in ds.FIELDS}
        np.savez_compressed(folder/'samples.npz',**arrays)
        atomic_json(folder/'manifest.json',dict(meta,samples_sha256=sha(folder/'samples.npz'),
            original_holdouts_used=False,internal_training_group_fold=i,
            splits={s:dict(samples=int(m.sum()),groups=len(set(data['train_group'][m]))) for s,m in masks.items()}))
        result=learn.train(folder,out/'model',backend=a.backend,epochs=a.epochs,batch_size=128,
            event=lambda r:print(f"fold={i} epoch={r.get('epoch')} score={r.get('validation_score')}",flush=True))
        atomic_json(out/'result.json',result);results.append(result)
    summary={}
    for name in sorted(set(data['train_map'])):
        summary[name]={}
        for head in ('weapon','pickup'):
            rows=[r['test'][name][head] for r in results]
            keys=('accuracy','comparable_accuracy','persistence_accuracy','changed_shot_accuracy','same_weapon_accuracy') if head=='weapon' else (
                'accuracy','majority_accuracy','observed_edge_accuracy','nearest_item_accuracy','macro_f1')
            summary[name][head]={k:float(np.mean([r[k] for r in rows if r.get(k) is not None]))
                                  for k in keys if any(r.get(k) is not None for r in rows)}
            summary[name][head]['groups']=[r['groups'] for r in rows]
    atomic_json(a.output/'summary.json',dict(folds=a.folds,summary=summary,original_holdouts_used=False,
        scope='Observed choices; does not prove winning policy or control'))
    print(summary,flush=True)


if __name__=='__main__':main()
