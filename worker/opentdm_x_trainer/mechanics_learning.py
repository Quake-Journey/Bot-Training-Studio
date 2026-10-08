"""Offline mechanism expert training; never changes installed factory models."""
import json
from pathlib import Path
import time
import uuid

import numpy as np

from .data import sha
from .learning import atomic_json, device_for, writer_lock
from .mechanics_sequences import INPUTS, EVENTS, VERSION, load
from .models import PROFILES, create
from .temporal import predict, precision, save_checkpoint, restore_checkpoint


def evaluate(model, data, device, split):
    from sklearn.metrics import average_precision_score
    predictions=predict(model,data[split+'_x'],device)
    result={}
    for name in ['all']+sorted(set(data[split+'_map'])):
        chosen=np.ones(len(predictions),bool) if name=='all' else data[split+'_map']==name
        training=np.ones(len(data['train_x']),bool) if name=='all' else data['train_map']==name
        x=data[split+'_x'][chosen];y=data[split+'_y'][chosen];p=predictions[chosen]
        known=data[split+'_y_mask'][chosen].astype(bool)
        valid=known[:,:3].all(axis=1)
        def error(values,scale):return float(np.sqrt(np.mean(np.square(values[valid]*scale)))) if valid.any() else None
        metrics=dict(samples=int(chosen.sum()),groups=len(set(data[split+'_group'][chosen])),
            destination_samples=int(valid.sum()),destination_rmse=error(p[:,:3]-y[:,:3],1280),
            stationary_rmse=error(y[:,:3],1280),linear_rmse=error(x[:,-1,:3]*1000/1280-y[:,:3],1280),
            velocity_rmse=error(p[:,3:6]-y[:,3:6],1000),events={})
        for i,event in enumerate(EVENTS):
            mask=data[split+'_event_mask'][chosen,i].astype(bool)
            labels=data[split+'_events'][chosen,i][mask]
            probabilities=1/(1+np.exp(-np.clip(p[mask,6+i],-30,30)))
            trainmask=training & data['train_event_mask'][:,i].astype(bool)
            prior=float(data['train_events'][trainmask,i].mean()) if trainmask.any() else 0.
            positive=int(labels.sum());negative=len(labels)-positive
            predicted=probabilities>=.5
            qualified_groups=len(set(data[split+'_group'][chosen][mask & (data[split+'_events'][chosen,i]>0)]))
            metrics['events'][event]=dict(samples=len(labels),positive=positive,negative=negative,
                positive_groups=qualified_groups,train_prior=prior,
                brier=float(np.square(probabilities-labels).mean()) if len(labels) else None,
                prior_brier=float(np.square(prior-labels).mean()) if len(labels) else None,
                average_precision=float(average_precision_score(labels,probabilities)) if positive and negative else None,
                prevalence=positive/len(labels) if len(labels) else None,
                recall=float(predicted[labels>0].mean()) if positive else None,
                precision=float(labels[predicted].mean()) if predicted.any() else None)
            eventvalid=valid & (data[split+'_events'][chosen,i]>0)
            metrics[event+'_destination_rmse']=float(np.sqrt(np.mean(np.square((p[eventvalid,:3]-y[eventvalid,:3])*1280)))) if eventvalid.any() else None
        result[name]=metrics
    return result


def quality(metrics):
    heads={}
    for event in EVENTS:
        supported=[m['events'][event] for name,m in metrics.items() if name!='all' and m['events'][event]['positive']]
        heads[event]=bool(supported) and all(m['positive']>=10 and m['negative']>=10 and
            m['positive_groups']>=2 and m['brier']<m['prior_brier'] and
            m['average_precision']>m['prevalence'] and
            m['recall'] is not None and m['recall']>=.25 and
            m['precision'] is not None and m['precision']>=.5 for m in supported)
    return heads


def selection_score(metrics):
    """Validation only, equal weighting per map; no test-dependent tuning."""
    parts=[]
    for name,m in metrics.items():
        if name=='all':continue
        if m['destination_rmse'] is not None:parts.append(m['destination_rmse']/max(1,m['linear_rmse']))
        for e in m['events'].values():
            if e['positive'] and e['negative']:parts.append(e['brier']/max(.0001,e['prior_brier']))
    return float(np.mean(parts)) if parts else float('inf')


def train(dataset,store,profile='compact',backend='cpu',mode='fresh',epochs=20,batch_size=64,
          event=None,cancelled=None):
    import torch
    from safetensors.torch import load_file,save_file
    if mode not in ('fresh','resume') or profile not in PROFILES or profile=='reference' or not 1<=epochs<=200 or not 1<=batch_size<=1024:
        raise ValueError('Invalid mechanism training parameters')
    data,meta=load(dataset)
    code={name:sha(Path(__file__).with_name(name)) for name in ('transitions.py','maps.py','mechanics_sequences.py','mechanics_learning.py','models.py')}
    if meta['context']>PROFILES[profile].context:raise ValueError('History exceeds model profile')
    root=Path(store);torch.set_num_threads(4);torch.manual_seed(23)
    with writer_lock(root):
        device,name=device_for(backend);model=create(profile,len(INPUTS),8).to(device)
        model.activation_checkpointing=profile in ('large','xl')
        optimizer=torch.optim.AdamW(model.parameters(),lr=.0003,foreach=False)
        work=root/'work';work.mkdir(exist_ok=True)
        rng=np.random.default_rng(23)
        identity=dict(feature_version=VERSION,dataset_sha256=meta['samples_sha256'],profile=profile,context=meta['context'])
        state=dict(identity,epoch=0,history=[],best_loss=None,best_checkpoint=None,batch_size=batch_size,rng=rng.bit_generator.state)
        if mode=='resume':
            state=restore_checkpoint(work,model,optimizer)
            if any(state.get(k)!=v for k,v in identity.items()):raise ValueError('Resume dataset/profile mismatch')
            rng.bit_generator.state=state['rng'];batch_size=state['batch_size']
        elif (work/'resume.json').exists() or (root/'active.json').exists():
            raise ValueError('Fresh mechanism training requires a new store; resume the existing job')
        else:save_checkpoint(work,model,optimizer,state)
        casts=precision(torch,device);started=time.monotonic()
        positives=(data['train_events']*data['train_event_mask']).sum(axis=0)
        negatives=((1-data['train_events'])*data['train_event_mask']).sum(axis=0)
        # Bounded TRAIN-only rare-event emphasis; never resample holdouts.
        positive_weight=torch.as_tensor(np.minimum(16.,np.sqrt(negatives/np.maximum(1.,positives))),device=device)
        while state['epoch']<epochs:
            try:
                model.train();losses=[]
                ids=rng.permutation(len(data['train_x']))
                for offset in range(0,len(ids),batch_size):
                    if cancelled and cancelled():raise InterruptedError('Mechanism job paused; resume from completed epoch')
                    batch=ids[offset:offset+batch_size]
                    tensor=lambda key:torch.as_tensor(data['train_'+key][batch],device=device)
                    optimizer.zero_grad(set_to_none=True)
                    with casts():
                        pred=model(tensor('x'));known=tensor('y_mask')
                        loss=((pred[:,:6]-tensor('y')).square()*known).sum()/known.sum().clamp_min(1)
                        mask=tensor('event_mask')
                        bce=torch.nn.functional.binary_cross_entropy_with_logits(pred[:,6:],tensor('events'),
                            pos_weight=positive_weight,reduction='none')
                        loss=loss+.5*(bce*mask).sum()/mask.sum().clamp_min(1)
                    if not torch.isfinite(loss).item():raise ValueError('Nonfinite mechanism loss')
                    loss.backward();torch.nn.utils.clip_grad_norm_(model.parameters(),1.);optimizer.step()
                    losses.append(float(loss.detach().cpu()))
                validation=evaluate(model,data,device,'validation');score=selection_score(validation)
                state['epoch']+=1;state['rng']=rng.bit_generator.state;state['batch_size']=batch_size
                state['history'].append(dict(epoch=state['epoch'],loss=float(np.mean(losses)),validation=validation))
                improved=state['best_loss'] is None or score<state['best_loss']
                if improved:state['best_loss']=score
                save_checkpoint(work,model,optimizer,state,improved=improved)
                if event:event(dict(stage='mechanism_training',epoch=state['epoch'],epochs=epochs,progress=state['epoch']/epochs,
                    loss=float(np.mean(losses)),validation_score=score,batch_size=batch_size))
            except torch.OutOfMemoryError:
                if batch_size<=1:raise RuntimeError('GPU memory exhausted at batch 1; resume with CPU or smaller profile')
                optimizer.zero_grad(set_to_none=True)
                if device.type=='cuda':torch.cuda.empty_cache()
                state=restore_checkpoint(work,model,optimizer);rng.bit_generator.state=state['rng'];batch_size//=2
                if event:event(dict(stage='memory_retry',batch_size=batch_size))
        if cancelled and cancelled():raise InterruptedError('Paused before publishing expert; old generation preserved')
        if any(sha(Path(__file__).with_name(name))!=digest for name,digest in code.items()):
            raise ValueError('Mechanism training code changed; candidate not published')
        best=work/state['best_checkpoint']/'weights.safetensors'
        if sha(best)!=state['best_sha256']:raise ValueError('Expert checkpoint changed')
        model.load_state_dict(load_file(str(best)))
        validation=evaluate(model,data,device,'validation');test=evaluate(model,data,device,'test')
        heads=quality(validation);generation='generation-'+uuid.uuid4().hex
        folder=root/generation;folder.mkdir()
        save_file({k:v.detach().cpu().contiguous() for k,v in model.state_dict().items()},str(folder/'weights.safetensors'))
        np.savez_compressed(folder/'replay.npz',**data)
        manifest=dict(identity,generation=generation,backend=backend,device=name,epochs=state['epoch'],
            validation=validation,test=test,history=state['history'],qualified_observation_heads=heads,
            accepted=all(heads.values()),dataset=meta,seconds=time.monotonic()-started,
            may_activate_in_game=False,runtime_qualified=False,
            files={file:sha(folder/file) for file in ('weights.safetensors','replay.npz')},
            source_sha256=code)
        atomic_json(folder/'manifest.json',manifest)
        if cancelled and cancelled():raise InterruptedError('Paused before activation; old generation preserved')
        if manifest['accepted']:atomic_json(root/'active.json',dict(generation=generation,scope='offline_mechanisms_only'))
        return {key:manifest[key] for key in ('generation','accepted','qualified_observation_heads','validation','test','epochs','runtime_qualified')}
