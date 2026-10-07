"""Multi-task sequence learner with safe checkpoints, replay and resource adaptation."""
import contextlib
import json
from pathlib import Path
import time
import uuid

import numpy as np

from .data import WEAPONS, sha
from .learning import atomic_json, device_for, writer_lock
from .models import PROFILES, create
from .sequences import CONTINUOUS, INPUTS, LANDMARKS, VERSION, load

WIDTH = len(CONTINUOUS)
WEAPON_END = WIDTH + len(WEAPONS)
OUTPUTS = WEAPON_END + len(LANDMARKS) + 1


def replay(old, new, cap=6000):
    seen, result = {}, {}
    for data in (old, new):
        if data is None:
            continue
        for split in ("train", "validation", "test"):
            for group in set(data[split + "_group"]):
                if group in seen and seen[group] != split:
                    raise ValueError("Replay group crossed partition")
                seen[group] = split
    for split in ("train", "validation", "test"):
        rows = {}
        for data in (old, new):
            if data is None:
                continue
            for i, key in enumerate(data[split + "_identity"]):
                if key in rows:
                    previous, j = rows[key]
                    if any(not np.array_equal(previous[split + "_" + f][j], data[split + "_" + f][i]) for f in ("x", "y", "weapon", "landmark", "group", "map")):
                        raise ValueError("Conflicting duplicate sequence")
                rows[key] = (data, i)
        maps = sorted({str(data[split + "_map"][i]) for data, i in rows.values()})
        chosen = []
        for name in maps:
            keys = [key for key, (data, i) in rows.items() if data[split + "_map"][i] == name]
            # Hash gives a stable, order-independent bounded replay per map.
            import hashlib
            chosen += sorted(keys, key=lambda k: hashlib.sha256(k.encode()).digest())[:cap // len(maps)]
        for field in ("x", "y", "weapon", "landmark", "identity", "group", "map", "recording"):
            result[split + "_" + field] = np.asarray([rows[k][0][split + "_" + field][rows[k][1]] for k in chosen])
    return result


def active(root):
    root = Path(root)
    ref = json.loads((root / "active.json").read_text())
    name = ref["generation"]
    from .learning import re_safe_generation
    if not re_safe_generation(name):
        raise ValueError("Invalid generation ID")
    folder = root / name
    meta = json.loads((folder / "manifest.json").read_text())
    if meta["feature_version"] != VERSION or meta.get("may_activate_in_game") is not False:
        raise ValueError("Incompatible model generation")
    for filename, digest in meta["files"].items():
        if Path(filename).name != filename or sha(folder / filename) != digest:
            raise ValueError("Model generation integrity failure")
    return folder, meta


def precision(torch, device):
    enabled = device.type == "cuda" and torch.cuda.is_bf16_supported()
    return (lambda: torch.autocast(device_type="cuda", dtype=torch.bfloat16)) if enabled else contextlib.nullcontext


def predict(model, x, device, batch=128):
    import torch
    model.eval(); chunks=[]; offset=0
    autocast = precision(torch, device)
    while offset < len(x):
        try:
            with torch.no_grad(), autocast():
                value = model(torch.as_tensor(x[offset:offset+batch], device=device)).float().cpu().numpy()
            chunks.append(value); offset += len(value)
        except torch.OutOfMemoryError:
            if batch <= 1:
                raise
            batch //= 2
            if device.type == "cuda": torch.cuda.empty_cache()
    return np.concatenate(chunks)


def evaluate(model, data, device, split):
    result={}; x=data[split+"_x"]; y=data[split+"_y"]
    pred=predict(model,x,device); names=data[split+"_map"]
    for name in ["all"] + sorted(set(names)):
        mask=np.ones(len(x),dtype=bool) if name=="all" else names==name
        weapon=data[split+"_weapon"][mask]; valid=weapon>=0
        current_onehot=x[mask,-1,15:26]
        valid &= current_onehot.sum(axis=1)>0
        current_weapon=current_onehot.argmax(axis=1)
        predicted_weapon=pred[mask,WIDTH:WEAPON_END].argmax(axis=1)
        switched=(weapon!=current_weapon)&valid
        predicted_switch=(predicted_weapon!=current_weapon)&valid
        training_landmarks=data['train_landmark'][data['train_map']==name] if name!='all' else data['train_landmark']
        majority=int(np.bincount(training_landmarks,minlength=len(LANDMARKS)+1).argmax()) if len(training_landmarks) else -1
        result[name]=dict(samples=int(mask.sum()),
            motion_rmse=float(np.sqrt(np.mean(((pred[mask,:3]-y[mask,:3])*320)**2))),
            persistence_rmse=float(np.sqrt(np.mean(((x[mask,-1,:3]-y[mask,:3])*320)**2))),
            destination_rmse=float(np.sqrt(np.mean(((pred[mask,3:6]-y[mask,3:6])*1280)**2))),
            stationary_destination_rmse=float(np.sqrt(np.mean((y[mask,3:6]*1280)**2))),
            resource_mae=float(np.abs(pred[mask,6:8]-y[mask,6:8]).mean()),
            resource_nochange_mae=float(np.abs(y[mask,6:8]).mean()),
            weapon_accuracy=float((pred[mask,WIDTH:WEAPON_END].argmax(axis=1)[valid]==weapon[valid]).mean()) if valid.any() else None,
            weapon_persistence_accuracy=float((current_weapon[valid]==weapon[valid]).mean()) if valid.any() else None,
            weapon_switch_samples=int(switched.sum()),
            weapon_switch_recall=float((predicted_switch&switched).sum()/switched.sum()) if switched.any() else None,
            weapon_switch_precision=float((predicted_switch&switched).sum()/predicted_switch.sum()) if predicted_switch.any() else None,
            weapon_after_switch_accuracy=float((predicted_weapon[switched]==weapon[switched]).mean()) if switched.any() else None,
            landmark_accuracy=float((pred[mask,WEAPON_END:].argmax(axis=1)==data[split+"_landmark"][mask]).mean()),
            landmark_majority_accuracy=float((majority==data[split+"_landmark"][mask]).mean()))
    return result


def qualify_heads(metrics):
    """Only named-map validation gates capabilities; tests never select them."""
    rows=[r for name,r in metrics.items() if name!='all']
    return dict(
        motion=bool(rows) and all(r['motion_rmse']<r['persistence_rmse'] for r in rows),
        destination=bool(rows) and all(r['destination_rmse']<r['stationary_destination_rmse'] for r in rows),
        resources=bool(rows) and all(r.get('resource_nochange_mae') is not None and r['resource_mae']<r['resource_nochange_mae'] for r in rows),
        weapon=bool(rows) and all(r.get('weapon_accuracy') is not None and r.get('weapon_persistence_accuracy') is not None
            and r['weapon_accuracy']>r['weapon_persistence_accuracy'] and (r.get('weapon_after_switch_accuracy') or 0)>.25 for r in rows),
        landmark=bool(rows) and all(r.get('landmark_majority_accuracy') is not None and r['landmark_accuracy']>r['landmark_majority_accuracy'] for r in rows))


def save_checkpoint(root, model, optimizer, state, improved=False):
    from safetensors.torch import save_file
    import torch
    name="checkpoint-"+uuid.uuid4().hex
    folder=root/name;folder.mkdir()
    weights={k:v.detach().cpu().contiguous() for k,v in model.state_dict().items()}
    save_file(weights,str(folder/"weights.safetensors"))
    if improved:
        state["best_checkpoint"]=name
        state["best_sha256"]=sha(folder/"weights.safetensors")
    opt=optimizer.state_dict(); tensors={}; scalar={}
    for key, values in opt["state"].items():
        scalar[str(key)]={}
        for field,value in values.items():
            if torch.is_tensor(value): tensors[f"{key}/{field}"]=value.detach().cpu().contiguous()
            else: scalar[str(key)][field]=value
    save_file(tensors,str(folder/"optimizer.safetensors"))
    atomic_json(folder/"state.json",dict(state, optimizer_groups=opt["param_groups"], optimizer_scalars=scalar,
        files={f:sha(folder/f) for f in ("weights.safetensors","optimizer.safetensors")}))
    atomic_json(root/"resume.json",dict(checkpoint=name))
    # Bounded application-owned checkpoint rotation, only exact known files.
    previous=sorted(root.glob("checkpoint-*"),key=lambda p:p.stat().st_mtime,reverse=True)
    keep={p.name for p in previous[:2]} | {state.get("best_checkpoint")}
    for old in previous:
        if old.name in keep or old.is_symlink() or old.resolve().parent!=root.resolve():continue
        files=list(old.iterdir())
        if {p.name for p in files}!={"weights.safetensors","optimizer.safetensors","state.json"}:continue
        if any(p.is_symlink() or not p.is_file() for p in files):continue
        for file in files:file.unlink()
        old.rmdir()
    return folder


def restore_checkpoint(root, model, optimizer):
    from safetensors.torch import load_file
    import re
    name=json.loads((root/"resume.json").read_text())["checkpoint"]
    if not re.fullmatch(r"checkpoint-[a-f0-9]{32}",name): raise ValueError("Invalid checkpoint")
    folder=root/name; state=json.loads((folder/"state.json").read_text())
    for file,digest in state["files"].items():
        if Path(file).name!=file or sha(folder/file)!=digest: raise ValueError("Checkpoint integrity failure")
    model.load_state_dict(load_file(str(folder/"weights.safetensors")))
    values={int(k):v for k,v in state["optimizer_scalars"].items()}
    for key,value in load_file(str(folder/"optimizer.safetensors")).items():
        index,field=key.split("/",1);values.setdefault(int(index),{})[field]=value
    optimizer.load_state_dict(dict(state=values,param_groups=state["optimizer_groups"]))
    return state


def train(dataset, store, profile="compact", backend="cpu", mode="fresh", epochs=20,
          batch_size=64, event=None, cancelled=None):
    import torch
    from safetensors.torch import load_file, save_file
    if mode not in ("fresh","update","resume") or not 1<=epochs<=200 or profile=="reference":
        raise ValueError("Invalid temporal training mode/profile/epoch count")
    if not 1<=batch_size<=1024: raise ValueError("Batch size must be 1..1024")
    torch.set_num_threads(4); torch.manual_seed(23)
    root=Path(store); data,meta=load(dataset)
    if meta["context"]>PROFILES[profile].context: raise ValueError("Dataset history exceeds model context")
    with writer_lock(root):
        device,name=device_for(backend)
        model=create(profile,len(INPUTS),OUTPUTS).to(device)
        model.activation_checkpointing=profile in ("large","xl")
        optimizer=torch.optim.AdamW(model.parameters(),lr=.0003,foreach=False)
        parent=None; old=None; previous=None
        if mode=="update":
            folder,parentmeta=active(root)
            if any(parentmeta.get(k)!=v for k,v in dict(profile=profile,context=meta["context"],donor=meta["donor"]).items()):
                raise ValueError("Cannot update across family/history/donor; use fresh store")
            model.load_state_dict(load_file(str(folder/"weights.safetensors")))
            with np.load(folder/"replay.npz",allow_pickle=False) as f: old={k:f[k] for k in f.files}
            previous=evaluate(model,old,device,"validation");parent=folder.name
        work=root/"work";work.mkdir(exist_ok=True)
        rng=np.random.default_rng(23)
        state=dict(profile=profile,context=meta["context"],feature_version=VERSION,donor=meta["donor"],
            dataset_sha256=meta["samples_sha256"],epoch=0,history=[],best_loss=None,best_checkpoint=None,
            rng=rng.bit_generator.state,parent=parent,previous=previous,batch_size=batch_size)
        if mode=="resume":
            state=restore_checkpoint(work,model,optimizer)
            if any(state.get(k)!=v for k,v in dict(profile=profile,context=meta["context"],donor=meta["donor"],dataset_sha256=meta["samples_sha256"]).items()):
                raise ValueError("Resume dataset/profile mismatch")
            with np.load(work/"replay.npz",allow_pickle=False) as f: data={k:f[k] for k in f.files}
            rng.bit_generator.state=state["rng"];batch_size=state["batch_size"];parent=state["parent"];previous=state["previous"]
            if parent:
                folder,_=active(root)
                if folder.name!=parent: raise ValueError("Active parent changed since interrupted training")
                with np.load(folder/"replay.npz",allow_pickle=False) as f: old={k:f[k] for k in f.files}
        else:
            data=replay(old,data)
            np.savez_compressed(work/"replay.npz",**data)
            state["replay_sha256"]=sha(work/"replay.npz")
            if old is not None:
                # The previous teacher is evaluated before any update. Distill
                # only retained maps; new-map examples have no old-teacher target.
                keep=np.isin(data['train_map'],old['train_map'])
                targets=np.zeros((len(keep),6),np.float32)
                targets[keep]=predict(model,data['train_x'][keep],device)[:,:6]
                np.savez_compressed(work/'teacher.npz',mask=keep,targets=targets)
                state['teacher_sha256']=sha(work/'teacher.npz')
            save_checkpoint(work,model,optimizer,state)
        if sha(work/"replay.npz")!=state["replay_sha256"]: raise ValueError("Resume replay integrity failure")
        teacher=None
        if state.get('teacher_sha256'):
            if sha(work/'teacher.npz')!=state['teacher_sha256']:raise ValueError('Retention teacher integrity failure')
            with np.load(work/'teacher.npz',allow_pickle=False) as f:teacher={k:f[k] for k in f.files}
        autocast=precision(torch,device);start=time.monotonic()
        while state["epoch"]<epochs:
            if cancelled and cancelled(): raise InterruptedError("Training paused; completed epoch can be resumed")
            epoch_rng=rng.bit_generator.state
            try:
                model.train(); losses=[]
                indices=rng.permutation(len(data["train_x"]))
                for offset in range(0,len(indices),batch_size):
                    if cancelled and cancelled(): raise InterruptedError("Training paused; partial epoch will restart")
                    ids=indices[offset:offset+batch_size]
                    x=torch.as_tensor(data["train_x"][ids],device=device)
                    y=torch.as_tensor(data["train_y"][ids],device=device)
                    weapon=torch.as_tensor(data["train_weapon"][ids],device=device)
                    landmark=torch.as_tensor(data["train_landmark"][ids],device=device)
                    optimizer.zero_grad(set_to_none=True)
                    with autocast():
                        pred=model(x)
                        loss=(pred[:,:WIDTH]-y).square().mean()
                        if (weapon>=0).any(): loss=loss+.15*torch.nn.functional.cross_entropy(pred[:,WIDTH:WEAPON_END],weapon,ignore_index=-1)
                        loss=loss+.1*torch.nn.functional.cross_entropy(pred[:,WEAPON_END:],landmark)
                        if teacher is not None:
                            keep=torch.as_tensor(teacher['mask'][ids],device=device)
                            if keep.any():
                                target=torch.as_tensor(teacher['targets'][ids],device=device)
                                loss=loss+2.*(pred[keep,:6]-target[keep]).square().mean()
                    if not torch.isfinite(loss).item(): raise ValueError("Nonfinite training loss")
                    loss.backward();torch.nn.utils.clip_grad_norm_(model.parameters(),1.);optimizer.step()
                    losses.append(float(loss.detach().cpu()))
                validation=evaluate(model,data,device,"validation")
                score=validation["all"]["motion_rmse"]
                state["epoch"]+=1;state["rng"]=rng.bit_generator.state;state["batch_size"]=batch_size
                state["history"].append(dict(epoch=state["epoch"],loss=float(np.mean(losses)),validation=validation))
                eligible=True
                if previous:
                    for name,reference in previous.items():
                        if name=='all':continue
                        current=validation[name]
                        eligible &= current['motion_rmse']<=reference['motion_rmse']*1.01 and current['destination_rmse']<=reference['destination_rmse']*1.03
                state['history'][-1]['retention_eligible']=bool(eligible)
                improved=(eligible and not state.get('best_eligible')) or (eligible==state.get('best_eligible',False) and (state["best_loss"] is None or score<state["best_loss"]))
                if improved:state['best_eligible']=bool(eligible)
                if improved: state["best_loss"]=score
                save_checkpoint(work,model,optimizer,state,improved=improved)
                if event: event(dict(stage="temporal_training",epoch=state["epoch"],epochs=epochs,progress=state["epoch"]/epochs,
                    loss=float(np.mean(losses)),validation_rmse=score,batch_size=batch_size))
            except torch.OutOfMemoryError:
                if batch_size<=1: raise RuntimeError("GPU memory exhausted at batch 1; resume with CPU or a smaller model in a fresh store")
                optimizer.zero_grad(set_to_none=True)
                if device.type=="cuda": torch.cuda.empty_cache()
                state=restore_checkpoint(work,model,optimizer);rng.bit_generator.state=state["rng"]
                batch_size//=2
                if event: event(dict(stage="memory_retry",batch_size=batch_size,message="Restored last completed epoch"))
        if cancelled and cancelled(): raise InterruptedError("Paused before publishing model")
        best=work/state["best_checkpoint"]/"weights.safetensors"
        if sha(best)!=state["best_sha256"]: raise ValueError("Best checkpoint changed")
        model.load_state_dict(load_file(str(best)))
        validation=evaluate(model,data,device,"validation");test=evaluate(model,data,device,"test")
        retained=evaluate(model,old,device,"validation") if old is not None else None
        failures=[]
        for name,metrics in validation.items():
            if name!="all" and metrics["motion_rmse"]>=metrics["persistence_rmse"]:failures.append("motion_baseline:"+name)
        if retained:
            for name,metrics in retained.items():
                if metrics["motion_rmse"]>previous[name]["motion_rmse"]*1.01:failures.append("old_map_motion_regression:"+name)
                if metrics["destination_rmse"]>previous[name]["destination_rmse"]*1.03:failures.append("old_map_destination_regression:"+name)
        generation="generation-"+uuid.uuid4().hex;folder=root/generation;folder.mkdir()
        save_file({k:v.detach().cpu().contiguous() for k,v in model.state_dict().items()},str(folder/"weights.safetensors"))
        np.savez_compressed(folder/"replay.npz",**data)
        manifest=dict(schema=1,feature_version=VERSION,profile=profile,context=meta["context"],donor=meta["donor"],
            parent=parent,validation=validation,test=test,retention_before=previous,retention_after=retained,
            history=state["history"],backend=backend,device=str(device),accepted=not failures,failures=failures,
            dataset=meta,seconds=time.monotonic()-start,may_activate_in_game=False,
            qualified_observation_heads=qualify_heads(validation),
            files={f:sha(folder/f) for f in ("weights.safetensors","replay.npz")})
        atomic_json(folder/"manifest.json",manifest)
        if cancelled and cancelled(): raise InterruptedError("Paused before activation; previous model preserved")
        if not failures: atomic_json(root/"active.json",dict(generation=generation,scope="offline_temporal_only"))
        return dict(generation=generation,accepted=not failures,failures=failures,validation=validation,test=test,
                    profile=profile,context=meta["context"],epochs=state["epoch"],runtime_qualified=False,
                    qualified_observation_heads=qualify_heads(validation))
