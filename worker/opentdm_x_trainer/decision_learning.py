"""Independent shot/pickup observation learner; never activates server tactics."""
import json
from pathlib import Path
import time
import uuid

import numpy as np

from . import decisions as ds
from .data import WEAPONS, sha
from .learning import atomic_json, device_for, writer_lock
from .models import PROFILES, create
from .temporal import precision, predict, save_checkpoint, restore_checkpoint

WIDTH = len(WEAPONS)
OUTPUTS = WIDTH + len(ds.ITEMS) + 1
SOURCE_HASHES = {name:sha(Path(__file__).with_name(name)) for name in ('decisions.py','decision_learning.py','models.py')}


def switch_factor(data):
    train=data['train_weapon'];equipped=data['train_current_weapon']
    known=(train>=0)&(equipped>=0);changed=known&(train!=equipped)
    return min(8.,max(1.,float(known.sum())/max(1,int(changed.sum()))))


def weights(data, field, classes):
    values = data['train_'+field]; counts = np.bincount(values[values>=0],minlength=classes)
    present = counts>0
    result = np.ones(classes,np.float32)
    if present.any(): result[present] = np.clip(np.sqrt(counts[present].mean()/counts[present]),.25,4)
    return result


def loss(pred, data, ids, device, class_weights):
    import torch
    result = pred.sum()*0
    for field, start, end in (('weapon',0,WIDTH),('pickup',WIDTH,OUTPUTS)):
        y = torch.as_tensor(data['train_'+field][ids],device=device)
        if (y>=0).any():
            ce = torch.nn.functional.cross_entropy(pred[:,start:end],y,ignore_index=-1,reduction='none',
                weight=torch.as_tensor(class_weights[field],device=device))
            importance=(y>=0).float()
            if field=='weapon':
                current=torch.as_tensor(data['train_current_weapon'][ids],device=device)
                # Switching is rare but essential; compute its bounded weight
                # on training observations only. Holdout distribution is intact.
                factor=switch_factor(data)
                importance=importance*torch.where((current>=0)&(y!=current),factor,1.)
            result = result + (ce*importance).sum()/importance.sum().clamp_min(1)
    return result


def corrected_logits(pred, data, split, calibration=None):
    """Undo loss reweighting before interpreting logits as observation probabilities.

    Weighted categorical CE fits q(y|x) proportional to w(y,x)*p(y|x).
    A training emphasis on switching is not evidence that switching is likelier.
    This correction uses train counts and current inputs, never holdout labels.
    """
    result=pred.copy()
    training=data if calibration is None else calibration
    result[:,:WIDTH]-=np.log(weights(training,'weapon',WIDTH))[None]
    result[:,WIDTH:]-=np.log(weights(training,'pickup',len(ds.ITEMS)+1))[None]
    current=data[split+'_current_weapon']
    changed=(current[:,None]>=0)&(np.arange(WIDTH)[None]!=current[:,None])
    result[:,:WIDTH]-=changed*np.log(switch_factor(training))
    return result


def metrics(pred, data, split, mask):
    from sklearn.metrics import f1_score
    result = {}
    training = np.isin(data['train_map'],np.unique(data[split+'_map'][mask]))
    for field,start,end in (('weapon',0,WIDTH),('pickup',WIDTH,OUTPUTS)):
        y = data[split+'_'+field]; valid = mask & (y>=0)
        if not valid.any(): result[field] = dict(samples=0); continue
        logits = pred[valid,start:end]; logits = logits-logits.max(axis=1,keepdims=True)
        p = np.exp(logits); p /= p.sum(axis=1,keepdims=True)
        target = y[valid]; selected = p.argmax(axis=1)
        train = data['train_'+field][training]; train = train[train>=0]
        majority = int(np.bincount(train,minlength=end-start).argmax()) if len(train) else 0
        m = dict(samples=len(target),accuracy=float((selected==target).mean()),
            majority_accuracy=float((target==majority).mean()),
            cross_entropy=float(-np.log(np.maximum(p[np.arange(len(target)),target],1e-9)).mean()),
            macro_f1=float(f1_score(target,selected,labels=np.unique(target),average='macro',zero_division=0)),
            groups=len(set(data[split+'_group'][valid])))
        if field=='weapon':
            current = data[split+'_current_weapon'][valid]
            known = current>=0; changed = known & (target!=current); stayed = known & ~changed
            m.update(persistence_accuracy=float((target[known]==current[known]).mean()) if known.any() else None,
                comparable_accuracy=float((selected[known]==target[known]).mean()) if known.any() else None,
                changed_shots=int(changed.sum()),
                changed_shot_accuracy=float((selected[changed]==target[changed]).mean()) if changed.any() else None,
                same_weapon_accuracy=float((selected[stayed]==target[stayed]).mean()) if stayed.any() else None,
                changed_cross_entropy=float(-np.log(np.maximum(p[np.arange(len(target))[changed],target[changed]],1e-9)).mean()) if changed.any() else None,
                same_cross_entropy=float(-np.log(np.maximum(p[np.arange(len(target))[stayed],target[stayed]],1e-9)).mean()) if stayed.any() else None)
        else:
            actual = target<ds.NONE
            m.update(observed_edges=int(actual.sum()),
                observed_edge_accuracy=float((selected[actual]==target[actual]).mean()) if actual.any() else None,
                nearest_item_accuracy=float((data[split+'_nearest_item'][valid][actual]==target[actual]).mean()) if actual.any() else None,
                edge_recall=float((selected[actual]<ds.NONE).mean()) if actual.any() else None,
                by_item={ds.ITEMS[k]:dict(samples=int((target==k).sum()),recall=float((selected[target==k]==k).mean()))
                         for k in range(ds.NONE) if (target==k).any()})
        result[field] = m
    return result


def evaluate(model,data,device,split,calibration=None):
    pred = corrected_logits(predict(model,data[split+'_x'],device),data,split,calibration)
    return {name:metrics(pred,data,split,np.ones(len(pred),bool) if name=='all' else data[split+'_map']==name)
            for name in ['all']+sorted(set(data[split+'_map']))}


def qualified(validation, test):
    """Both independent heads need multiple held-out matches; no inherited pass."""
    passed = dict(weapon=True,pickup=True)
    for report in (validation,test):
        for name,m in report.items():
            if name=='all': continue
            w = m['weapon']; p = m['pickup']
            passed['weapon'] &= bool(w.get('groups',0)>=3 and w.get('changed_shots',0)>=20
                and w.get('persistence_accuracy') is not None
                and w['comparable_accuracy']>w['persistence_accuracy']
                and (w.get('changed_shot_accuracy') or 0)>=.3 and (w.get('same_weapon_accuracy') or 0)>=.8)
            passed['pickup'] &= bool(p.get('groups',0)>=3 and p.get('observed_edges',0)>=50
                and p['accuracy']>p['majority_accuracy']
                and (p.get('observed_edge_accuracy') or 0)>(p.get('nearest_item_accuracy') or 0)
                and p.get('macro_f1',0)>.4)
    return passed


def experiment(root,generation=None):
    from .learning import re_safe_generation
    root=Path(root)
    if generation is None:generation=json.loads((root/'latest-experiment.json').read_text())['generation']
    if not re_safe_generation(generation):raise ValueError('Invalid decision generation')
    folder=root/generation;meta=json.loads((folder/'manifest.json').read_text())
    if meta['feature_version']!=ds.VERSION or meta.get('may_activate_in_game') is not False:
        raise ValueError('Incompatible decision generation')
    if 'replay.npz' not in meta['files']:
        raise ValueError('This older experiment has no replay evidence; train a fresh generation first')
    for name,digest in meta['files'].items():
        if Path(name).name!=name or sha(folder/name)!=digest:raise ValueError('Decision generation integrity failure')
    with np.load(folder/'replay.npz',allow_pickle=False) as raw:old={k:raw[k] for k in raw.files}
    ds.validate(old,meta['context'])
    return folder,meta,old


def retention(before,after):
    """Report old-map regressions independently for both observation heads."""
    result={}
    for split in before:
        result[split]={}
        for name,heads in before[split].items():
            if name=='all':continue
            result[split][name]={}
            for field,prior in heads.items():
                current=after[split][name][field]
                checks={}
                for metric in ('accuracy','changed_shot_accuracy','observed_edge_accuracy','macro_f1'):
                    if prior.get(metric) is not None:
                        delta=current[metric]-prior[metric]
                        checks[metric]=dict(before=prior[metric],after=current[metric],delta=delta,retained=delta>=-.03)
                result[split][name][field]=dict(metrics=checks,retained=bool(checks) and all(v['retained'] for v in checks.values()))
    return result


def train(dataset, store, profile='compact', backend='cpu', mode='fresh', epochs=20,
          batch_size=128, event=None, cancelled=None):
    import torch
    from safetensors.torch import load_file, save_file
    if mode not in ('fresh','update','resume') or profile not in PROFILES or profile=='reference' or not 1<=epochs<=200 or not 1<=batch_size<=1024:
        raise ValueError('Unsupported decision training options')
    data,meta = ds.load(dataset)
    if meta['context']>PROFILES[profile].context: raise ValueError('Decision context exceeds selected model profile')
    torch.set_num_threads(4); torch.manual_seed(23)
    root = Path(store)
    with writer_lock(root):
        if mode=='fresh' and (root/'work'/'resume.json').exists():
            raise ValueError('Use resume or a new decision model store')
        work = root/'work'; work.mkdir(exist_ok=True)
        parent=None;old=None;before=None
        if mode=='update':parent=json.loads((root/'latest-experiment.json').read_text())['generation']
        elif mode=='resume':parent=json.loads((work/'lineage.json').read_text()).get('parent')
        if parent:
            parent_folder,parent_meta,old=experiment(root,parent)
            if parent_meta['profile']!=profile or parent_meta['context']!=meta['context']:
                raise ValueError('Decision update profile/context mismatch')
            if parent_meta['dataset'].get('donor')!=meta.get('donor'):
                raise ValueError('Decision update donor mismatch')
            data=ds.replay(old,data)
        device,_ = device_for(backend)
        model = create(profile,len(ds.INPUTS),OUTPUTS).to(device)
        model.activation_checkpointing = profile in ('large','xl')
        if parent:
            model.load_state_dict(load_file(str(parent_folder/'weights.safetensors')))
            before={s:evaluate(model,old,device,s) for s in ('validation','test')}
        optimizer = torch.optim.AdamW(model.parameters(),lr=.0003,foreach=False)
        rng = np.random.default_rng(23)
        identity = dict(feature_version=ds.VERSION,profile=profile,context=meta['context'],
            dataset_sha256=meta['samples_sha256'],source_hashes=SOURCE_HASHES,
            probability_correction='training-loss-weights-v1',parent_generation=parent)
        state = dict(identity,epoch=0,history=[],best_loss=None,best_checkpoint=None,rng=rng.bit_generator.state,batch_size=batch_size)
        if mode=='resume':
            state = restore_checkpoint(work,model,optimizer)
            if any(state.get(k)!=v for k,v in identity.items()): raise ValueError('Decision resume provenance mismatch')
            rng.bit_generator.state = state['rng']; batch_size = state['batch_size']
        else:
            atomic_json(work/'lineage.json',dict(parent=parent))
            save_checkpoint(work,model,optimizer,state)
        class_weights = dict(weapon=weights(data,'weapon',WIDTH),pickup=weights(data,'pickup',len(ds.ITEMS)+1))
        autocast = precision(torch,device); start = time.monotonic()
        while state['epoch']<epochs:
            try:
                model.train(); losses=[]; indices=rng.permutation(len(data['train_x']))
                for offset in range(0,len(indices),batch_size):
                    if cancelled and cancelled(): raise InterruptedError('Decision training paused; completed epoch can be resumed')
                    ids=indices[offset:offset+batch_size]; optimizer.zero_grad(set_to_none=True)
                    with autocast():
                        pred=model(torch.as_tensor(data['train_x'][ids],device=device))
                        value=loss(pred,data,ids,device,class_weights)
                    if not torch.isfinite(value).item(): raise ValueError('Nonfinite decision loss')
                    value.backward(); torch.nn.utils.clip_grad_norm_(model.parameters(),1.); optimizer.step()
                    losses.append(float(value.detach().cpu()))
                validation=evaluate(model,data,device,'validation')
                values=[]
                for name,m in validation.items():
                    if name=='all':continue
                    for field,head in m.items():
                        if not head['samples']:continue
                        if field=='weapon' and head.get('changed_cross_entropy') is not None:
                            values.append(.5*head['changed_cross_entropy']+.5*(head.get('same_cross_entropy') or 0))
                        else:values.append(head['cross_entropy'])
                if not values: raise ValueError('No observed validation decisions')
                score=float(np.mean(values)); improved=state['best_loss'] is None or score<state['best_loss']
                retention_passed=True
                if old is not None:
                    old_validation=evaluate(model,old,device,'validation',calibration=data)
                    check=retention({'validation':before['validation']},{'validation':old_validation})
                    retention_passed=all(head['retained'] for heads in check['validation'].values() for head in heads.values())
                    improved &= retention_passed
                if improved: state['best_loss']=score
                state['epoch']+=1;state['rng']=rng.bit_generator.state;state['batch_size']=batch_size
                state['history'].append(dict(epoch=state['epoch'],validation=validation,loss=float(np.mean(losses)),retention_passed=retention_passed))
                save_checkpoint(work,model,optimizer,state,improved=improved)
                if event:event(dict(stage='decision_training',epoch=state['epoch'],epochs=epochs,validation_score=score,loss=float(np.mean(losses)),retention_passed=retention_passed))
            except torch.OutOfMemoryError:
                if batch_size<=1: raise RuntimeError('GPU memory exhausted at batch 1; resume on CPU')
                optimizer.zero_grad(set_to_none=True)
                if device.type=='cuda': torch.cuda.empty_cache()
                state=restore_checkpoint(work,model,optimizer);rng.bit_generator.state=state['rng'];batch_size//=2
                if event:event(dict(stage='memory_retry',batch_size=batch_size))
        if cancelled and cancelled(): raise InterruptedError('Paused before decision generation publication')
        if state['best_checkpoint'] is None:
            raise ValueError('No update epoch retained prior validation quality; previous experiment preserved')
        best=work/state['best_checkpoint']/'weights.safetensors'
        if sha(best)!=state['best_sha256']:raise ValueError('Decision checkpoint integrity failure')
        model.load_state_dict(load_file(str(best)))
        validation=evaluate(model,data,device,'validation');test=evaluate(model,data,device,'test')
        retained=None
        if old is not None:
            after={s:evaluate(model,old,device,s,calibration=data) for s in ('validation','test')}
            retained=retention(before,after)
        heads=qualified(validation,test)
        if retained is not None:
            for maps in retained.values():
                for prior_heads in maps.values():
                    for field,check in prior_heads.items():heads[field]&=check['retained']
        generation='generation-'+uuid.uuid4().hex;folder=root/generation;folder.mkdir()
        save_file({k:v.detach().cpu().contiguous() for k,v in model.state_dict().items()},str(folder/'weights.safetensors'))
        np.savez_compressed(folder/'replay.npz',**data)
        report=dict(identity,generation=generation,dataset=meta,history=state['history'],validation=validation,test=test,
            qualified_observation_heads=heads,seconds=time.monotonic()-start,backend=backend,
            loss_weights={field:value.tolist() for field,value in class_weights.items()},switch_factor=switch_factor(data),
            retention=retained,replay_samples={s:len(data[s+'_x']) for s in ('train','validation','test')},
            may_activate_in_game=False,files={name:sha(folder/name) for name in ('weights.safetensors','replay.npz')})
        atomic_json(folder/'manifest.json',report)
        # A receipt, not an active.json consumed by the compiler or game.
        if cancelled and cancelled():raise InterruptedError('Paused before decision result publication')
        atomic_json(root/'latest-experiment.json',dict(generation=generation,scope='offline_observation_only'))
        return {k:report[k] for k in ('generation','validation','test','qualified_observation_heads','retention','seconds','may_activate_in_game')}
