"""Bounded DATA-only candidate packages and an offline rollback library."""
import hashlib
import json
from pathlib import Path
import tempfile
import uuid
import zipfile

import numpy as np

from .learning import atomic_json, writer_lock

SCHEMA = "bts-knowledge-candidate-v1"
MEMBERS = {"map.json", "routes.json", "style.json", "policy.json", "validation.json"}


def encode(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf8")


def validate(path):
    with zipfile.ZipFile(path) as archive:
        names=archive.namelist()
        if len(names)!=len(set(names)) or set(names)!=MEMBERS|{"manifest.json"}:
            raise ValueError("Unexpected package member or duplicate")
        if sum(f.file_size for f in archive.infolist())>32*1024**2:
            raise ValueError("Package exceeds unpacked limit")
        blobs={name:archive.read(name) for name in names}
    manifest=json.loads(blobs.pop("manifest.json"))
    if manifest.get("schema")!=SCHEMA or manifest.get("runtime_qualified") is not False or manifest.get("server_installable") is not False:
        raise ValueError("Unsupported package or false gameplay qualification")
    if set(manifest.get("files",{}))!=MEMBERS:raise ValueError("Missing hash inventory")
    for name,content in blobs.items():
        if hashlib.sha256(content).hexdigest()!=manifest["files"][name]:raise ValueError("Package integrity failure")
    docs={name:json.loads(value) for name,value in blobs.items()}
    routes=docs["routes.json"]
    nodes=routes["nodes"];links=routes["links"]
    if len(nodes)>8192 or len(links)>131072:raise ValueError("Navigation count exceeds limit")
    if [n["id"] for n in nodes]!=list(range(len(nodes))):raise ValueError("Invalid node IDs")
    if any(len(n["origin"])!=3 or not np.isfinite(n["origin"]).all() for n in nodes):raise ValueError("Invalid node position")
    if any(not (0<=l["source"]<len(nodes) and 0<=l["target"]<len(nodes)) for l in links):raise ValueError("Dangling route")
    if any(not np.isfinite(l["seconds"]) or not 0 < l["seconds"] <= 60 for l in links):raise ValueError("Invalid traversal time")
    evidence=routes.get('observed_mechanisms',{}).get('witnesses',[])
    if len(evidence)>4096:raise ValueError('Mechanism witness limit')
    mechanisms=docs['map.json'].get('transitions',[])
    if len(mechanisms)>4096 or len({m['id'] for m in mechanisms})!=len(mechanisms):raise ValueError('Invalid mechanism inventory')
    ids={m['id'] for m in mechanisms}
    for witness in evidence:
        edge=witness.get('mechanism',{})
        if witness.get('runtime_qualified') is not False or edge.get('kind') not in ('teleport','push') or edge.get('id') not in ids:
            raise ValueError('Invalid or falsely qualified mechanism witness')
        if any(len(witness[k])!=3 or not np.isfinite(witness[k]).all() for k in ('origin','destination','velocity')):
            raise ValueError('Invalid mechanism witness vector')
    policy=docs["policy.json"]
    if policy is not None:
        tree=policy["nodes"];count=policy["features"]
        if not 0<len(tree)<=2047 or not 1<=count<=20000:raise ValueError("Policy limit")
        reached=set()
        def walk(index,depth,ancestors):
            if depth>16 or not 0<=index<len(tree) or index in ancestors:raise ValueError("Policy graph invalid")
            reached.add(index);node=tree[index]
            if not np.isfinite(node["value"]).all() or len(node["value"])>64:raise ValueError("Policy value invalid")
            if node["feature"]>=0:
                if node["feature"]>=count or not np.isfinite(node["threshold"]):raise ValueError("Policy feature invalid")
                walk(node["left"],depth+1,ancestors|{index});walk(node["right"],depth+1,ancestors|{index})
        walk(0,0,set())
        if len(reached)!=len(tree):raise ValueError("Unreachable policy nodes")
    import re
    if not re.fullmatch(r"[a-f0-9]{64}",manifest["bsp_sha256"]):raise ValueError("Invalid BSP digest")
    if docs["map.json"]["bsp_sha256"]!=manifest["bsp_sha256"] or docs["map.json"]["map"]!=manifest["map"]:raise ValueError("Map identity differs")
    return manifest,docs


def compile(project, knowledge, output, model_store=None, include_chat=False):
    from .maps import inspect
    from .data import sha
    project=Path(project);report=json.loads(Path(knowledge).read_text(encoding="utf8"))
    projectmeta=json.loads((project/"project.json").read_text(encoding="utf8"))
    world=inspect(project/"map.bsp",projectmeta["map"])
    if world["bsp_sha256"]!=projectmeta["bsp_sha256"]:raise ValueError("Cached BSP integrity failure")
    if report["bsp_sha256"]!=world["bsp_sha256"]:raise ValueError("Knowledge belongs to another BSP")
    style=dict(report["style"])
    if not include_chat:style["phrases"]=[]
    policy=None;validation=dict(runtime_qualified=False,limits=report["limits"])
    if model_store:
        from . import temporal
        from .models import create
        from .sequences import INPUTS
        from sklearn.tree import DecisionTreeRegressor
        from safetensors.torch import load_file
        import torch
        folder,meta=temporal.active(model_store)
        if not meta["accepted"]:raise ValueError("Rejected model cannot be compiled")
        if world["map"] not in meta["validation"]:raise ValueError("Model has no validation for this map")
        if meta.get("donor") != style.get("donor"):
            raise ValueError("Model and style donor differ; select matching evidence")
        model=create(meta["profile"],len(INPUTS),temporal.OUTPUTS)
        from .model_layers import load_effective
        model.load_state_dict(load_effective(model_store,folder,meta))
        with np.load(folder/"replay.npz",allow_pickle=False) as arrays:
            data={key:arrays[key] for key in arrays.files}
        teacher=temporal.predict(model,data["train_x"],torch.device("cpu"))
        qualified=temporal.qualify_heads(meta['validation'])
        # Experimental auxiliary heads must not ride into the candidate as if
        # their accuracy had been certified by the movement head's success.
        outputs=[]
        if qualified['motion']:outputs+=list(range(3))
        if qualified['destination']:outputs+=list(range(3,6))
        if qualified['resources']:outputs+=list(range(6,8))
        if not outputs:raise ValueError('No continuous observation head passed validation')
        teacher=teacher[:,outputs]
        tree=DecisionTreeRegressor(max_depth=10,max_leaf_nodes=512,min_samples_leaf=8,random_state=23)
        tree.fit(data["train_x"].reshape(len(teacher),-1),teacher)
        native=tree.tree_
        policy=dict(schema=1,context=meta["context"],features=len(INPUTS)*meta["context"],input_names=INPUTS,
            outputs=[temporal.CONTINUOUS[i] for i in outputs],scope='validated observational predictions, NOT game commands',
            nodes=[dict(feature=int(native.feature[i]),threshold=float(np.float32(native.threshold[i])),
                left=int(native.children_left[i]),right=int(native.children_right[i]),
                value=native.value[i,:,0].astype(np.float32).tolist()) for i in range(native.node_count)])
        student=tree.predict(data["test_x"].reshape(len(data["test_x"]),-1))
        original=temporal.predict(model,data["test_x"],torch.device("cpu"))[:,outputs]
        validation.update(model_generation=folder.name,model_validation=meta["validation"],
            distillation_rmse=float(np.sqrt(np.mean((student-original)**2))),
            qualified_observation_heads=qualified,
            policy_scope="offline observation prior",model_sha256=sha(folder/"weights.safetensors"),
            factory_base=meta.get('factory_base'),user_overlay_sha256=meta['files'].get('user-delta.safetensors'))
    docs={"map.json":world,"routes.json":{k:report[k] for k in ("nodes","links","items","control_candidates")},
          "style.json":style,"policy.json":policy,"validation.json":validation}
    if 'observed_mechanisms' in report:
        docs['routes.json']['observed_mechanisms']=report['observed_mechanisms']
    blobs={name:encode(value) for name,value in docs.items()}
    manifest=dict(schema=SCHEMA,id=uuid.uuid4().hex,map=world["map"],bsp_sha256=world["bsp_sha256"],
        runtime_qualified=False,server_installable=False,requires_capabilities=["bts-candidate-reader-v1"],
        files={name:hashlib.sha256(value).hexdigest() for name,value in blobs.items()})
    output=Path(output)
    if output.exists():raise ValueError("Package destination exists")
    output.parent.mkdir(parents=True,exist_ok=True)
    pending=output.with_name(output.name+".pending")
    with zipfile.ZipFile(pending,"w",compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json",encode(manifest))
        for name,value in blobs.items():archive.writestr(name,value)
    validate(pending);pending.replace(output)
    return dict(package=str(output),bytes=output.stat().st_size,sha256=sha(output),server_installable=False)


def install_offline(package, library):
    """Import into the Studio library only; cannot point at a server destination."""
    import shutil
    from .data import sha
    library=Path(library)
    manifest,_=validate(package)
    digest=sha(package)
    with writer_lock(library):
        target=library/(digest+".btsknowledge")
        if not target.exists():
            pending=library/(digest+".pending");shutil.copyfile(package,pending)
            if sha(pending)!=digest:raise ValueError("Package changed while copying")
            pending.replace(target)
        elif sha(target)!=digest:
            raise ValueError("Existing library package integrity failure")
        indexpath=library/"library.json"
        index=json.loads(indexpath.read_text()) if indexpath.exists() else dict(schema=1,maps={},history=[])
        key=manifest["bsp_sha256"]
        index["history"].append(dict(map=key,previous=index["maps"].get(key),next=digest))
        index["maps"][key]=digest
        atomic_json(indexpath,index)
    return dict(library=str(library),generation=digest,server_installed=False)


def rollback(library,bsp_sha256):
    library=Path(library)
    with writer_lock(library):
        index=json.loads((library/"library.json").read_text())
        change=next((r for r in reversed(index["history"]) if r["map"]==bsp_sha256 and r["next"]==index["maps"].get(bsp_sha256)),None)
        if not change or not change["previous"]:raise ValueError("No previous generation")
        validate(library/(change["previous"]+".btsknowledge"))
        index["maps"][bsp_sha256]=change["previous"]
        index["history"].append(dict(map=bsp_sha256,previous=change["next"],next=change["previous"]))
        atomic_json(library/"library.json",index)
    return dict(generation=change["previous"],server_installed=False)
