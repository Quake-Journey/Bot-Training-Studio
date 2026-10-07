"""Immutable factory bases and user-owned, base-bound weight overlays.

No factory replay recordings are distributed. Qualification of an observation
head is not qualification of game commands. Updates never silently rebase.
"""
import json
from pathlib import Path
import re
import shutil

from .data import sha
from .learning import atomic_json

SCHEMA = 'bts-factory-models-v1'


def inside(path, parent):
    return Path(path).resolve().is_relative_to(Path(parent).resolve())


def catalog(root):
    root = Path(root)
    doc = json.loads((root/'catalog.json').read_text(encoding='utf8'))
    if doc.get('schema') != SCHEMA or doc.get('game_installable') is not False:
        raise ValueError('Unsupported factory catalog')
    if not 0 < len(doc['models']) <= 16:
        raise ValueError('Factory catalog size is invalid')
    seen = set()
    for entry in doc['models']:
        key = entry['id']
        if not re.fullmatch(r'[a-z0-9-]{1,80}', key) or key in seen:
            raise ValueError('Invalid or duplicate factory model ID')
        seen.add(key)
        folder = root/key
        if not inside(folder, root) or folder.is_symlink():
            raise ValueError('Factory path escapes its catalog')
        meta = json.loads((folder/'model.json').read_text(encoding='utf8'))
        if sha(folder/'model.json') != entry['metadata_sha256']:
            raise ValueError('Factory metadata integrity failure')
        weights = folder/'weights.safetensors'
        if weights.is_symlink() or not inside(weights, root) or sha(weights) != entry['weights_sha256']:
            raise ValueError('Factory weights integrity failure')
        if meta['id'] != key or meta.get('may_activate_in_game') is not False:
            raise ValueError('Invalid factory model scope')
    return doc


def bind(root, destination, profile, context, donor):
    """Pin a verified factory snapshot BEFORE user training, without changing it."""
    from .sequences import VERSION
    root, destination = Path(root), Path(destination)
    if inside(destination, root):
        raise ValueError('User training cannot write into factory Models')
    doc = catalog(root)
    entries = [entry for entry in doc['models'] if entry['profile'] == profile]
    if len(entries) != 1:
        raise ValueError('No matching factory profile; select compact or balanced, or train from scratch')
    entry = entries[0]
    source = root/entry['id']
    meta = json.loads((source/'model.json').read_text(encoding='utf8'))
    if meta['feature_version'] != VERSION or meta['context'] != context or meta['donor'] != donor:
        raise ValueError('Factory schema/history/donor mismatch; use matching settings or train from scratch')
    target = destination/'bases'/entry['id']
    target.mkdir(parents=True, exist_ok=True)
    for name, expected in [('model.json', entry['metadata_sha256']), ('weights.safetensors', entry['weights_sha256'])]:
        out = target/name
        if out.exists():
            if sha(out) != expected:
                raise ValueError('Pinned factory snapshot was modified')
        else:
            pending = out.with_suffix(out.suffix+'.pending')
            shutil.copyfile(source/name, pending)
            if sha(pending) != expected:
                raise ValueError('Factory changed while pinning')
            pending.replace(out)
    return dict(id=entry['id'], weights_sha256=entry['weights_sha256'], metadata_sha256=entry['metadata_sha256'],
                profile=profile, feature_version=VERSION, context=context, donor=donor,
                maps=meta['maps'], scope='offline_observation_only')


def base_weights(store, binding):
    from safetensors.torch import load_file
    key = binding['id']
    if not re.fullmatch(r'[a-z0-9-]{1,80}', key):
        raise ValueError('Invalid pinned factory ID')
    folder = Path(store)/'bases'/key
    for name, field in [('model.json','metadata_sha256'), ('weights.safetensors','weights_sha256')]:
        if not inside(folder/name, store) or (folder/name).is_symlink() or sha(folder/name) != binding[field]:
            raise ValueError('Pinned factory integrity failure')
    return load_file(str(folder/'weights.safetensors'))


def save_overlay(store, folder, model, binding):
    """Keep the immutable base and the user delta as separately verifiable files."""
    from safetensors.torch import save_file
    base = base_weights(store, binding)
    weights = {key: value.detach().cpu().contiguous() for key,value in model.state_dict().items()}
    if set(base) != set(weights):
        raise ValueError('Factory/user tensor schema mismatch')
    delta = {key:(weights[key]-base[key]).contiguous() for key in base}
    save_file(delta, str(Path(folder)/'user-delta.safetensors'))
    return sha(Path(folder)/'user-delta.safetensors')


def load_effective(store, folder, meta):
    from safetensors.torch import load_file
    if not meta.get('factory_base'):
        return load_file(str(Path(folder)/'weights.safetensors'))
    base = base_weights(store, meta['factory_base'])
    delta_path = Path(folder)/'user-delta.safetensors'
    if sha(delta_path) != meta['files'].get('user-delta.safetensors'):
        raise ValueError('User overlay integrity failure')
    delta = load_file(str(delta_path))
    if set(base) != set(delta) or any(base[key].shape != delta[key].shape for key in base):
        raise ValueError('Factory/user tensor schema mismatch')
    return {key:base[key]+delta[key] for key in base}
