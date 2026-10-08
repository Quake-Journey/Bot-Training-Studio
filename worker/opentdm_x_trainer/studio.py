"""One isolated UI job, versioned JSON-lines events, no shell commands/network."""
from __future__ import annotations

import argparse
import json
import os
import time
from pathlib import Path

from .learning import atomic_json


PROTOCOL = 1


def hardware():
    import torch
    devices = [dict(backend="cpu", name="CPU", available=True)]
    if torch.cuda.is_available():
        free, total = torch.cuda.mem_get_info()
        devices.append(dict(backend="rocm" if torch.version.hip else "cuda",
                            name=torch.cuda.get_device_name(0), available=True,
                            free_gib=free / 2**30, total_gib=total / 2**30))
    if hasattr(torch, "xpu") and torch.xpu.is_available():
        devices.append(dict(backend="xpu", name=torch.xpu.get_device_name(0), available=True))
    return devices


def run(request_path):
    request_path = Path(request_path).resolve()
    request = json.loads(request_path.read_text(encoding="utf-8-sig"))
    if request.get("protocol") != PROTOCOL:
        raise ValueError("Unsupported job protocol")
    job = request_path.parent
    job_id = request["job_id"]
    sequence = 0

    def emit(kind, **fields):
        nonlocal sequence
        sequence += 1
        event = dict(protocol=PROTOCOL, job_id=job_id, seq=sequence, type=kind, **fields)
        print(json.dumps(event, allow_nan=False), flush=True)
        with (job / "events.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps(event, allow_nan=False) + "\n")

    def cancelled():
        return (job / "cancel.request").exists()

    emit("started", action=request["action"], pid=os.getpid())
    try:
        if cancelled():
            raise InterruptedError("Cancelled before starting")
        action = request["action"]
        if action == "inventory":
            from .projects import inventory
            rows = inventory(request["inputs"], request.get("map"))
            atomic_json(job / "inventory.json", dict(recordings=rows))
            result = dict(recordings=rows[:200], total=len(rows), inventory=str(job / "inventory.json"))
        elif action == "import_project":
            from .projects import import_project, inventory
            selected=request.get("selected")
            if selected is None and request.get("tricks"):
                selected=inventory(request["inputs"])+[dict(row,role="tricks") for row in inventory(request["tricks"])]
            result = import_project(request["bsp"], request["inputs"], request["project"],
                                    request.get("mode", "update"), event=lambda row: emit("progress", **row),
                                    cancelled=cancelled, selected=selected)
        elif action == "fit_movement":
            from .movement import fit
            result=fit(request["project"],request.get("donor") or None,int(request.get("limit",200)),
                event=lambda row:emit("progress",**row),cancelled=cancelled)
        elif action == "analyze_project":
            from .knowledge import analyze
            result=analyze(request["project"],request.get("donor") or None,
                event=lambda row:emit("progress",**row),cancelled=cancelled)
        elif action == 'prepare_mechanisms':
            from .mechanics_sequences import prepare
            result=prepare(request['projects'],request['dataset'],int(request.get('context',16)),
                int(request.get('limit',12000)),event=lambda row:emit('progress',**row),cancelled=cancelled)
            result=dict(dataset=request['dataset'],context=result['context'],splits=result['splits'],runtime_qualified=False)
        elif action == 'train_mechanisms':
            import torch
            from .mechanics_learning import train
            from .model_layers import inside
            if request.get('factory_catalog') and inside(request['store'],request['factory_catalog']):
                raise ValueError('User training cannot write into factory Models')
            backend=request['backend']
            if backend=='auto':backend=next((d['backend'] for d in hardware() if d['backend']!='cpu'),'cpu')
            options=dict(profile=request.get('profile','compact'),backend=backend,
                mode=request.get('mode','fresh'),epochs=int(request.get('epochs',20)),batch_size=int(request.get('batch_size',64)),
                event=lambda row:emit('progress',**row),cancelled=cancelled)
            try:
                result=train(request['dataset'],request['store'],**options)
            except (torch.OutOfMemoryError,RuntimeError) as error:
                memory_error=isinstance(error,torch.OutOfMemoryError) or str(error).startswith('GPU memory exhausted at batch 1')
                if request['backend']!='auto' or backend=='cpu' or not memory_error:raise
                emit('progress',stage='cpu_fallback',message='GPU memory insufficient; continue locally on CPU')
                options['backend']='cpu';options['mode']='resume'
                result=train(request['dataset'],request['store'],**options)
        elif action in ("prepare_sequences", "prepare_decisions"):
            if action == 'prepare_decisions':
                from .decisions import prepare
            else:
                from .sequences import prepare
            sources=[]
            for value in request["projects"]:
                folder=Path(value);meta=json.loads((folder/"project.json").read_text())
                revision=folder/"revisions"/meta["active_revision"]
                sources.append(dict(combat=str(revision/"combat.parquet"),groups=str(revision/"groups.json"),bsp=str(folder/"map.bsp"),map=meta["map"]))
            result=prepare(sources,request["dataset"],int(request.get("context",16)),int(request.get("limit",2000)),
                donor=request.get("donor") or None,event=lambda row:emit("progress",**row),cancelled=cancelled)
            result=dict(dataset=request["dataset"],context=result["context"],splits=result["splits"],runtime_qualified=False)
        elif action in ("train_sequences", "train_decisions"):
            if action == 'train_decisions':
                from .decision_learning import train
            else:
                from .temporal import train
            import torch
            backend=request["backend"]
            if backend=="auto":backend=next((d["backend"] for d in hardware() if d["backend"]!="cpu"),"cpu")
            options=dict(profile=request.get("profile","compact"),backend=backend,
                mode=request.get("mode","fresh"),epochs=int(request.get("epochs",20)),batch_size=int(request.get("batch_size",64)),
                event=lambda row:emit("progress",**row),cancelled=cancelled)
            if request.get('factory_catalog'):
                from .model_layers import inside
                if inside(request['store'],request['factory_catalog']):
                    raise ValueError('User training cannot write into factory Models')
                if action=='train_sequences': options['factory_catalog']=request['factory_catalog']
            try:
                result=train(request["dataset"],request["store"],**options)
            except (torch.OutOfMemoryError, RuntimeError) as error:
                memory_error=isinstance(error,torch.OutOfMemoryError) or str(error).startswith('GPU memory exhausted at batch 1')
                if request["backend"]!='auto' or backend=='cpu' or not memory_error:raise
                emit('progress',stage='cpu_fallback',message='GPU memory insufficient; continue locally on CPU')
                options['backend']='cpu'
                if (Path(request['store'])/'work/resume.json').exists():options['mode']='resume'
                result=train(request["dataset"],request["store"],**options)
        elif action == 'factory_status':
            from .model_layers import catalog
            doc=catalog(request['factory_catalog'])
            result=dict(integrity_valid=True,scope='offline_observation_only',game_installable=False,
                models=[dict(profile=row['profile'],maps=row['maps'],context=row['context']) for row in doc['models']])
        elif action == "sequence_status":
            from .temporal import active
            folder,meta=active(request["store"])
            result=dict(generation=folder.name,validation=meta["validation"],test=meta["test"],
                profile=meta["profile"],context=meta["context"],factory_base=meta.get('factory_base'),runtime_qualified=False)
        elif action == "compile_package":
            from .packages import compile
            result=compile(request["project"],request["knowledge"],request["output"],request.get("model_store") or None,
                include_chat=bool(request.get("include_chat",False)))
        elif action == "verify_package":
            from .packages import validate
            manifest,_=validate(request["package"])
            result=dict(map=manifest["map"],id=manifest["id"],integrity_valid=True,server_installable=False)
        elif action == "install_offline":
            from .packages import install_offline
            result=install_offline(request["package"],request["library"])
        elif action == "hardware":
            from .models import catalog, choose_profile
            devices = hardware()
            selected = next((d for d in devices if d["backend"] != "cpu"), devices[0])
            result = dict(devices=devices, profiles=catalog(),
                          suggested_profile=choose_profile(selected.get("free_gib", 0), selected["backend"]))
        elif action in ("calibrate", "calibrate_decisions"):
            from .resources import calibrate
            backend=request["backend"]
            if backend=="auto":backend=next((d["backend"] for d in hardware() if d["backend"]!="cpu"),"cpu")
            result=calibrate(request.get("profile","compact"),backend,int(request.get("context",16)),
                event=lambda row:emit("progress",**row),cancelled=cancelled,
                family='decisions' if action=='calibrate_decisions' else 'sequences')
        elif action == "probe":
            from .learning import device_for, model_new
            from .models import PROFILES
            import torch
            backend = request["backend"]
            if backend == "auto":
                backend = next((d["backend"] for d in hardware() if d["backend"] != "cpu"), "cpu")
            profile = request.get("profile", "reference")
            device, name = device_for(backend)
            p = PROFILES[profile]
            emit("progress", stage="probe", progress=.1, message="Forward, backward and optimizer update")
            torch.set_num_threads(4)
            if backend in ("cuda", "rocm"):
                torch.cuda.reset_peak_memory_stats()
            model = model_new(profile).to(device)
            context = p.context
            x = torch.randn(2, context, 26, device=device) if profile != "reference" else torch.randn(2, 26, device=device)
            target = torch.randn(2, 3, device=device)
            before = next(model.parameters()).detach().clone()
            optimizer = torch.optim.Adam(model.parameters(), lr=.0001, foreach=False)
            start = time.perf_counter()
            loss = (model(x) - target).square().mean()
            loss.backward()
            optimizer.step()
            changed = not torch.equal(before, next(model.parameters()))
            result = dict(backend=backend, device=name, profile=profile, context=context,
                          parameters=sum(p.numel() for p in model.parameters()),
                          weights_changed=changed, seconds=time.perf_counter() - start,
                          loss=float(loss.detach().cpu()), gameplay_qualified=False)
            if backend in ("cuda", "rocm"):
                result["peak_allocated_mib"] = torch.cuda.max_memory_allocated() / 2**20
            if not changed or not torch.isfinite(loss).item():
                raise ValueError("Model backward/update did not pass")
        elif action == "train":
            from .learning import train
            backend = request["backend"]
            if backend == "auto":
                backend = next((d["backend"] for d in hardware() if d["backend"] != "cpu"), "cpu")
            emit("progress", stage="preparing", progress=0., backend=backend)
            result = train(request["dataset"], request["store"], backend=backend,
                           mode=request.get("mode", "fresh"), epochs=int(request.get("epochs", 20)),
                           profile=request.get("profile", "reference"),
                           event=lambda row: emit("progress", **row), cancelled=cancelled)
        elif action == "status":
            from .learning import read_generation
            folder, manifest = read_generation(request["store"])
            result = dict(generation=folder.name, manifest=manifest)
        else:
            raise ValueError("Unsupported action: " + str(action))
        # Mutating actions own their cancellation/commit boundary. A request
        # arriving after the atomic commit must not relabel success as cancelled.
        if action in ("hardware", "probe", "status", "inventory", "verify_package") and cancelled():
            raise InterruptedError("Cancelled")
        atomic_json(job / "result.json", result)
        emit("completed", result=result)
        return 0
    except InterruptedError as error:
        emit("cancelled", message=str(error))
        return 2
    except Exception as error:
        emit("failed", message=str(error), error_type=type(error).__name__)
        return 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--request", type=Path, required=True)
    args = ap.parse_args()
    raise SystemExit(run(args.request))
