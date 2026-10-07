"""CPU tree control: compare representation limits independently of a Transformer."""
import argparse
from pathlib import Path
import sys

import numpy as np
from sklearn.ensemble import ExtraTreesClassifier

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import decisions as ds, decision_learning as learn
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--dataset',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    data,meta=ds.load(a.dataset)
    x={s:np.concatenate((data[s+'_x'][:,-1],data[s+'_x'][:,-1]-data[s+'_x'][:,-2]),axis=1)
       for s in ('train','validation','test')}
    predictions={s:np.zeros((len(x[s]),learn.OUTPUTS),np.float32) for s in ('validation','test')}
    for field,start,end in (('weapon',0,learn.WIDTH),('pickup',learn.WIDTH,learn.OUTPUTS)):
        valid=data['train_'+field]>=0
        model=ExtraTreesClassifier(n_estimators=128,min_samples_leaf=4,max_features=.8,n_jobs=4,random_state=23)
        model.fit(x['train'][valid],data['train_'+field][valid])
        for s in predictions:
            p=np.full((len(x[s]),end-start),1e-9,np.float32)
            p[:,model.classes_]=model.predict_proba(x[s])
            predictions[s][:,start:end]=np.log(np.maximum(p,1e-9))
        print(field+' baseline fitted',flush=True)
    result={s:{name:learn.metrics(pred,data,s,np.ones(len(pred),bool) if name=='all' else data[s+'_map']==name)
               for name in ['all']+sorted(set(data[s+'_map']))} for s,pred in predictions.items()}
    a.output.parent.mkdir(parents=True,exist_ok=True)
    atomic_json(a.output,dict(result,dataset_sha256=meta['samples_sha256'],
        scope='Fixed CPU control, no holdout tuning, no gameplay qualification'))
    print(str(a.output),flush=True)


if __name__=='__main__':main()
