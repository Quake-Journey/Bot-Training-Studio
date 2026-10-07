"""Offline experimental motion learner, continual replay and bounded CPU export."""
from __future__ import annotations

import contextlib
import json
import os
import time
import uuid
from pathlib import Path

import numpy as np

from .data import FEATURES, TARGETS, VERSION, load_dataset, sha


def atomic_json(path, doc):
    path = Path(path)
    pending = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with pending.open("x", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, allow_nan=False)
        f.flush()
        os.fsync(f.fileno())
    os.replace(pending, path)


@contextlib.contextmanager
def writer_lock(root):
    root.mkdir(parents=True, exist_ok=True)
    path = root / ".writer.lock"
    with path.open("x", encoding="utf-8") as f:
        f.write(str(os.getpid()))
    try:
        yield
    finally:
        path.unlink()


def device_for(backend):
    import torch
    if backend == "cpu":
        return torch.device("cpu"), "CPU (explicit)"
    if backend in ("cuda", "rocm"):
        is_rocm = bool(torch.version.hip)
        if not torch.cuda.is_available() or (backend == "rocm") != is_rocm:
            raise ValueError("Requested GPU backend unavailable: " + backend)
        return torch.device("cuda"), torch.cuda.get_device_name(0)
    if backend == "xpu":
        if not hasattr(torch, "xpu") or not torch.xpu.is_available():
            raise ValueError("Intel XPU unavailable")
        return torch.device("xpu"), torch.xpu.get_device_name(0)
    if backend == "directml":
        import torch_directml
        if torch_directml.device_count() < 1:
            raise ValueError("DirectML unavailable")
        return torch_directml.device(), "DirectML (experimental)"
    raise ValueError("Unknown backend; no silent CPU fallback")


def model_new(profile="reference"):
    from .models import create
    return create(profile, len(FEATURES), len(TARGETS))


def doctor(backend):
    import torch
    device, name = device_for(backend)
    torch.manual_seed(23)
    model = model_new().to(device)
    x = torch.randn(32, len(FEATURES)).to(device)
    target = torch.randn(32, len(TARGETS)).to(device)
    before = next(model.parameters()).detach().cpu().clone()
    optimizer = torch.optim.Adam(model.parameters(), lr=.001, foreach=False)
    loss = (model(x) - target).square().mean()
    loss.backward()
    optimizer.step()
    changed = not torch.equal(before, next(model.parameters()).detach().cpu())
    if not changed or not torch.isfinite(loss).item():
        raise RuntimeError("GPU backward/update probe failed")
    return dict(backend=backend, device=name, torch=torch.__version__,
                loss=float(loss.detach().cpu()), weights_changed=changed,
                parameters=sum(p.numel() for p in model.parameters()))


def merge_samples(old, new, cap=24000):
    """Stable bounded replay plus new observations; dedup before learning.

    Retain at most cap per partition. A group or ID changing its partition is
    fatal, including when separately prepared corpora accidentally disagree.
    """
    import hashlib
    group_splits, id_splits, result = {}, {}, {}
    for source in (old, new):
        if source is None:
            continue
        for split in ("train", "validation", "test"):
            for field, seen in (("group", group_splits), ("identity", id_splits)):
                for value in source[split + "_" + field]:
                    if value in seen and seen[value] != split:
                        raise ValueError("Split leakage: " + field)
                    seen[value] = split
    for split in ("train", "validation", "test"):
        rows = {}
        for source in (old, new):
            if source is None:
                continue
            for i, identity in enumerate(source[split + "_identity"]):
                if identity in rows:
                    previous, j = rows[identity]
                    for field in ("x", "y", "group", "map"):
                        if not np.array_equal(previous[split + "_" + field][j], source[split + "_" + field][i]):
                            raise ValueError("Conflicting observation for duplicate ID")
                rows[identity] = (source, i)
        # Stratify replay by map so a large new map cannot erase small old ones.
        bymap = {}
        for identity, (source, i) in rows.items():
            bymap.setdefault(str(source[split + "_map"][i]), []).append(identity)
        selected = []
        for mapname in sorted(bymap):
            ids = sorted(bymap[mapname], key=lambda s: hashlib.sha256(s.encode()).digest())
            selected.extend(ids[:cap // max(1, len(bymap))])
        if not selected:
            raise ValueError("Empty " + split)
        for field in ("x", "y", "identity", "group", "map", "recording"):
            result[split + "_" + field] = np.array([rows[s][0][split + "_" + field][rows[s][1]] for s in selected])
    return result


def predict(model, x, device):
    import torch
    chunks = []
    model.eval()
    with torch.no_grad():
        for start in range(0, len(x), 2048):
            chunks.append(model(torch.tensor(x[start:start + 2048], dtype=torch.float32, device=device)).cpu().numpy())
    return np.concatenate(chunks)


def score(y, prediction):
    # Units are Quake units/sec, so the reported error is interpretable.
    error = (np.asarray(y) - np.asarray(prediction)) * 320.
    return dict(rmse=float(np.sqrt(np.mean(error ** 2))), mae=float(np.mean(np.abs(error))))


def metrics(model, data, device, split):
    x, y = data[split + "_x"], data[split + "_y"]
    prediction = predict(model, x, device)
    result = {}
    maps = data[split + "_map"]
    for name in ["all"] + sorted(set(maps)):
        select = np.ones(len(y), dtype=bool) if name == "all" else maps == name
        result[name] = dict(samples=int(select.sum()), model=score(y[select], prediction[select]),
                            persistence=score(y[select], x[select, :3]),
                            stationary=score(y[select], np.zeros_like(y[select])))
    return result


def read_generation(root, generation=None):
    root = Path(root)
    if generation is None:
        generation = json.loads((root / "active.json").read_text(encoding="utf-8"))["generation"]
    if not re_safe_generation(generation):
        raise ValueError("Invalid generation ID")
    folder = root / generation
    doc = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    required = {"weights.safetensors", "replay.npz", "motion-prior.json", "motion-prior.c"}
    if (doc.get("schema") != 1 or doc.get("feature_version") != VERSION
            or set(doc.get("files", {})) != required
            or doc.get("may_activate_in_game") is not False):
        raise ValueError("Incomplete or incompatible generation manifest")
    for filename, expected in doc["files"].items():
        if Path(filename).name != filename or sha(folder / filename) != expected:
            raise ValueError("Generation integrity failure: " + filename)
    return folder, doc


def re_safe_generation(value):
    import re
    return isinstance(value, str) and bool(re.fullmatch(r"generation-[0-9a-f]{32}", value))


def rollback(root, generation):
    root = Path(root)
    with writer_lock(root):
        folder, doc = read_generation(root, generation)
        if not doc["experimental_model_accepted"]:
            raise ValueError("Rejected candidate cannot become the active offline model")
        atomic_json(root / "active.json", dict(generation=folder.name, scope="offline_motion_model_only"))
    return dict(generation=folder.name, runtime_qualified=False)


def export_tree(model, data, device, out):
    from sklearn.tree import DecisionTreeRegressor
    teacher = predict(model, data["train_x"], device)
    tree = DecisionTreeRegressor(max_depth=8, min_samples_leaf=24, random_state=23)
    tree.fit(data["train_x"], teacher)
    t = tree.tree_
    nodes = [dict(feature=int(t.feature[i]), threshold=float(np.float32(t.threshold[i])),
                  left=int(t.children_left[i]), right=int(t.children_right[i]),
                  value=[float(np.float32(v)) for v in t.value[i, :, 0]]) for i in range(t.node_count)]
    policy = dict(schema=1, kind="observational_motion_prior", feature_version=VERSION,
                  features=FEATURES, targets=TARGETS, nodes=nodes,
                  runtime_qualified=False, may_activate_in_game=False,
                  reason="No native movement/route/combat qualification; this predicts observations, not commands")
    atomic_json(out / "motion-prior.json", policy)
    # Exact same float32 thresholds/values in JSON, Python evaluator and plain C.
    def cfloat(v):
        text = format(v, ".9g")
        return (text if "." in text or "e" in text else text + ".0") + "f"
    declarations = []
    for n in nodes:
        declarations.append("{%d,%d,%d,%s,{%s}}" % (n["feature"], n["left"], n["right"], cfloat(n["threshold"]), ",".join(cfloat(v) for v in n["value"])))
    c = """/* Research-only motion PRIOR, NOT game movement commands. No ML runtime. */
#include <math.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
struct Node { int feature, left, right; float threshold, value[3]; };
static const struct Node nodes[] = {\n""" + ",\n".join(declarations) + """\n};
EXPORT int otx_motion_prior(const float *x, float *out) {
    int i = 0, n;
    if (!x || !out) return 0;
    for (n = 0; n < """ + str(len(FEATURES)) + """; ++n) if (!isfinite(x[n])) return 0;
    for (n = 0; n < 16; ++n) {
        const struct Node *p = &nodes[i];
        if (p->feature < 0) { out[0]=p->value[0]; out[1]=p->value[1]; out[2]=p->value[2]; return 1; }
        i = x[p->feature] <= p->threshold ? p->left : p->right;
    }
    return 0;
}
"""
    (out / "motion-prior.c").write_text(c, encoding="utf-8")
    fidelity = {}
    for split in ("validation", "test"):
        x = data[split + "_x"]
        student = tree_predict(policy, x)
        fidelity[split] = dict(teacher_error=score(predict(model, x, device), student),
                                observation_error=score(data[split + "_y"], student))
    return dict(nodes=len(nodes), max_depth=8, fidelity=fidelity)


def tree_predict(policy, x):
    x = np.asarray(x, dtype=np.float32)
    if x.ndim != 2 or x.shape[1] != len(FEATURES) or not np.isfinite(x).all():
        raise ValueError("Invalid features")
    output = []
    for row in x:
        index = 0
        for _ in range(16):
            n = policy["nodes"][index]
            if n["feature"] < 0:
                output.append(n["value"])
                break
            index = n["left"] if row[n["feature"]] <= n["threshold"] else n["right"]
        else:
            raise ValueError("Invalid/cyclic tree")
    return np.asarray(output, dtype=np.float32)


def train(dataset, root, backend="cuda", mode="fresh", epochs=60, seed=23,
          profile="reference", event=None, cancelled=None):
    import torch
    from safetensors.torch import load_file, save_file
    if mode not in ("fresh", "update") or not 1 <= epochs <= 500:
        raise ValueError("Invalid mode/epoch limit")
    root = Path(root)
    with writer_lock(root):
        if len(list(root.glob("generation-*"))) >= 12:
            raise ValueError("12-generation storage cap; archive/clean explicitly before adding more")
        torch.set_num_threads(4)
        torch.manual_seed(seed)
        np.random.seed(seed)
        device, name = device_for(backend)
        data, dataset_manifest = load_dataset(dataset)
        parent_id, parent_metrics, parent_data, parent_doc = None, None, None, None
        model = model_new(profile).to(device)
        if mode == "update":
            folder, parent_doc = read_generation(root)
            if parent_doc["feature_version"] != VERSION or parent_doc["donor"] != dataset_manifest["donor"]:
                raise ValueError("Cannot mix donor/schema scopes in one model store")
            if parent_doc.get("model_profile", "reference") != profile:
                raise ValueError("Model family differs; use a separate store and fresh training")
            model.load_state_dict(load_file(str(folder / "weights.safetensors")))
            with np.load(folder / "replay.npz", allow_pickle=False) as f:
                parent_data = {k: f[k] for k in f.files}
            parent_metrics = metrics(model, parent_data, device, "validation")
            parent_id = folder.name
        data = merge_samples(parent_data, data)
        initial = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
        optimizer = torch.optim.Adam(model.parameters(), lr=.001 if mode == "fresh" else .0003, foreach=False)
        tx = torch.tensor(data["train_x"], dtype=torch.float32, device=device)
        ty = torch.tensor(data["train_y"], dtype=torch.float32, device=device)
        history, best, best_loss, stale = [], None, float("inf"), 0
        start = time.perf_counter()
        for epoch in range(epochs):
            if cancelled and cancelled():
                raise InterruptedError("Cancelled before epoch; active model unchanged")
            model.train()
            indices = np.random.permutation(len(tx))
            losses = []
            for offset in range(0, len(tx), 512):
                if cancelled and cancelled():
                    raise InterruptedError("Cancelled between batches; active model unchanged")
                ix = torch.tensor(indices[offset:offset + 512], dtype=torch.long, device=device)
                optimizer.zero_grad(set_to_none=True)
                loss = (model(tx[ix]) - ty[ix]).square().mean()
                loss.backward()
                optimizer.step()
                losses.append(float(loss.detach().cpu()))
            validation = metrics(model, data, device, "validation")
            current_loss = validation["all"]["model"]["rmse"]
            if not np.isfinite(current_loss):
                raise ValueError("Nonfinite training result")
            history.append(dict(epoch=epoch + 1, loss=float(np.mean(losses)), validation_rmse=current_loss))
            if event:
                event(dict(stage="training", epoch=epoch + 1, epochs=epochs,
                           progress=(epoch + 1) / epochs, loss=float(np.mean(losses)),
                           validation_rmse=current_loss))
            if current_loss < best_loss:
                best_loss, stale = current_loss, 0
                best = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
            else:
                stale += 1
            if stale >= 12:
                break
        model.load_state_dict(best)
        changed = any(not torch.equal(initial[k], best[k]) for k in best)
        validations = metrics(model, data, device, "validation")
        tests = metrics(model, data, device, "test")  # Report only; never select epochs on test.
        retention = metrics(model, parent_data, device, "validation") if parent_data is not None else None
        failures = []
        if not changed:
            failures.append("weights_unchanged")
        for mapname, result in validations.items():
            if mapname != "all" and result["model"]["rmse"] >= result["persistence"]["rmse"]:
                failures.append("does_not_beat_persistence:" + mapname)
        if retention:
            for mapname, result in retention.items():
                if result["model"]["rmse"] > parent_metrics[mapname]["model"]["rmse"] * 1.01:
                    failures.append("old_validation_regression:" + mapname)
        generation = "generation-" + uuid.uuid4().hex
        if cancelled and cancelled():
            raise InterruptedError("Cancelled before export; active model unchanged")
        out = root / generation
        out.mkdir()
        save_file({k: v.contiguous() for k, v in best.items()}, str(out / "weights.safetensors"))
        np.savez_compressed(out / "replay.npz", **data)
        exported = export_tree(model, data, device, out)
        doc = dict(schema=1, feature_version=VERSION, donor=dataset_manifest["donor"], mode=mode,
                   model_profile=profile, observed_context=1,
                   parent=parent_id, backend=backend, device=name, torch=torch.__version__, seed=seed,
                   parameters=sum(p.numel() for p in model.parameters()), weights_changed=changed,
                   training_seconds=time.perf_counter() - start, epochs=history,
                   input_dataset=dataset_manifest, validation=validations, test=tests,
                   retention_before=parent_metrics, retention_after=retention, export=exported,
                   experimental_model_accepted=not failures, failures=failures,
                   runtime_qualified=False, may_activate_in_game=False,
                   source_sha256={p.name: sha(p) for p in Path(__file__).parent.glob("*.py")},
                   files={p.name: sha(p) for p in out.iterdir() if p.is_file()})
        atomic_json(out / "manifest.json", doc)
        if cancelled and cancelled():
            raise InterruptedError("Cancelled after export; candidate saved, active model unchanged")
        if not failures:
            # This pointer is an OFFLINE research checkpoint, never the live bot.
            atomic_json(root / "active.json", dict(generation=generation, scope="offline_motion_model_only"))
        return dict(generation=generation, accepted=not failures, failures=failures,
                    device=name, seconds=doc["training_seconds"], validation=validations,
                    test=tests, export=exported, runtime_qualified=False)
