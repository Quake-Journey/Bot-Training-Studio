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
        if action == "hardware":
            from .models import catalog, choose_profile
            devices = hardware()
            selected = next((d for d in devices if d["backend"] != "cpu"), devices[0])
            result = dict(devices=devices, profiles=catalog(),
                          suggested_profile=choose_profile(selected.get("free_gib", 0), selected["backend"]))
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
        if action != "train" and cancelled():
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
