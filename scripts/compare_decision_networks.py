"""Fixed tabular neural controls for decision representation/architecture diagnosis.

Uses the same grouped data as temporal models. No test-based epoch selection,
server export or inherited qualification. Unweighted CE avoids changing priors.
"""
import argparse
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import decisions as ds,decision_learning as learn
from opentdm_x_trainer.learning import atomic_json,device_for
from opentdm_x_trainer.data import sha


def main():
    import torch
    from torch import nn
    from safetensors.torch import save_file,load_file
    ap=argparse.ArgumentParser();ap.add_argument('--dataset',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--backend',default='cpu');ap.add_argument('--epochs',type=int,default=30)
    a=ap.parse_args()
    if a.output.exists():raise ValueError('Use a new comparison directory')
    if not 1<=a.epochs<=100:raise ValueError('Use 1..100 epochs')
    a.output.mkdir(parents=True);data,meta=ds.load(a.dataset);device,_=device_for(a.backend)
    torch.set_num_threads(4)
    x={s:np.concatenate((data[s+'_x'][:,-1],data[s+'_x'][:,-1]-data[s+'_x'][:,-2]),axis=1)
       for s in ('train','validation','test')}
    mean=x['train'].mean(axis=0);scale=np.maximum(x['train'].std(axis=0),.05)
    np.savez_compressed(a.output/'input-scaling.npz',mean=mean,scale=scale)
    x={s:torch.as_tensor(np.clip((v-mean)/scale,-10,10),device=device) for s,v in x.items()}
    labels={h:torch.as_tensor(data['train_'+h],device=device) for h in ('weapon','pickup')}
    summary={}
    for width in (128,512):
        torch.manual_seed(23);rng=np.random.default_rng(23)
        model=nn.Sequential(nn.Linear(x['train'].shape[1],width),nn.GELU(),nn.Dropout(.15),
            nn.Linear(width,width),nn.GELU(),nn.Dropout(.15),nn.Linear(width,learn.OUTPUTS)).to(device)
        opt=torch.optim.AdamW(model.parameters(),lr=.001,weight_decay=.01)
        best=None;history=[];folder=a.output/str(width);folder.mkdir()
        def evaluate(split):
            model.eval()
            with torch.no_grad():
                pred=np.concatenate([model(v).cpu().numpy() for v in x[split].split(512)])
            return {name:learn.metrics(pred,data,split,np.ones(len(pred),bool) if name=='all' else data[split+'_map']==name)
                for name in ['all']+sorted(set(data[split+'_map']))}
        for epoch in range(a.epochs):
            model.train();losses=[]
            for ids in np.array_split(rng.permutation(len(x['train'])),max(1,(len(x['train'])+255)//256)):
                idx=torch.as_tensor(ids,device=device);opt.zero_grad(set_to_none=True);pred=model(x['train'][idx])
                loss=pred.sum()*0
                for head,start,end in (('weapon',0,learn.WIDTH),('pickup',learn.WIDTH,learn.OUTPUTS)):
                    target=labels[head][idx]
                    if (target>=0).any():loss+=nn.functional.cross_entropy(pred[:,start:end],target,ignore_index=-1)
                loss.backward();nn.utils.clip_grad_norm_(model.parameters(),1);opt.step();losses.append(loss.item())
            validation=evaluate('validation');scores=[]
            for name,heads in validation.items():
                if name=='all':continue
                for head,m in heads.items():
                    if not m['samples']:continue
                    scores.append(.5*m['changed_cross_entropy']+.5*m['same_cross_entropy']
                        if head=='weapon' and m.get('changed_cross_entropy') is not None else m['cross_entropy'])
            score=float(np.mean(scores));history.append(dict(epoch=epoch+1,validation_score=score,loss=float(np.mean(losses))))
            if best is None or score<best:
                best=score;save_file({k:v.detach().cpu().contiguous() for k,v in model.state_dict().items()},str(folder/'best.safetensors'))
            if (epoch+1)%5==0:print(str(width)+' '+str(history[-1]),flush=True)
        model.load_state_dict(load_file(str(folder/'best.safetensors')))
        validation=evaluate('validation');test=evaluate('test')
        result=dict(validation=validation,test=test,history=history,dataset_sha256=meta['samples_sha256'],
            weights_sha256=sha(folder/'best.safetensors'),input_scaling_sha256=sha(a.output/'input-scaling.npz'),
            input_scaling='train-only mean/std; current frame plus delta',
            qualified_observation_heads=learn.qualified(validation,test),may_activate_in_game=False)
        atomic_json(folder/'result.json',result);summary[str(width)]=result
    atomic_json(a.output/'summary.json',summary)
    print(str(a.output/'summary.json'),flush=True)


if __name__=='__main__':main()
