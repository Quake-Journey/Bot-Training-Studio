"""Observed shot/pickup decisions, independently versioned from motion models.

Weapon labels come from muzzle flashes, item labels from HUD edges. These
are human/bot observations, not optimal actions, complete inventory or timers.
"""
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

from bts_analysis.items import NAMES, VERSION as PICKUP_VERSION, PACKET_VERSION
from . import sequences
from .data import WEAPONS, Reservoir, donor_match, sha
from .learning import atomic_json
from .maps import inspect

VERSION = 'observed-shot-pickup-v3'
ITEMS = tuple(dict.fromkeys(NAMES.values()))
NONE = len(ITEMS)
MEMORY = [f'equipped_{w}_{f}' for w in WEAPONS for f in ('age', 'known')]
MEMORY += [f'pickup_{w}_{f}' for w in ITEMS for f in ('age', 'known')]
PLACEMENTS = [f'item_{name}_{axis}' for name in ITEMS for axis in ('forward','left','up','exists')]
PACKET_INPUTS = [f'packet_{name}_{axis}' for name in ITEMS for axis in ('forward','left','up','observed')]
INPUTS = sequences.INPUTS + MEMORY + PLACEMENTS + PACKET_INPUTS + ['gunframe','gunframe_known']
FIELDS = ('x', 'weapon', 'pickup', 'current_weapon', 'nearest_item', 'identity', 'group', 'map', 'recording')


def add_memory(rows):
    """Past evidence only. Equipped earlier != currently owned or usable.

    Reset on recording/epoch/slot changes, discontinuities and death. A zero
    known bit means unobserved, not absent. Never fill from a future pickup.
    """
    tracks = {}; recording = None
    for raw in rows:
        row = dict(raw)
        if row['recording_id'] != recording:
            tracks.clear(); recording = row['recording_id']
        key = (row['segment'], row['epoch_id'], row['slot'])
        if not row.get('continuous') or row['health'] <= 0:
            tracks.pop(key, None)
        weapons, pickups = tracks.setdefault(key, ({}, {}))
        t = row['time_ms']
        if row['health'] > 0 and row['weapon'] in WEAPONS:
            weapons[row['weapon']] = t
        if row.get('pickup_item') in ITEMS:
            pickups[row['pickup_item']] = t
        row['decision_memory'] = [v for names, observed in ((WEAPONS, weapons), (ITEMS, pickups))
            for name in names for v in ((min(30, max(0, (t-observed[name])/1000))/30, 1)
                                       if name in observed else (0, 0))]
        yield row


def item_positions(world):
    # Several stock health entities share the HUD label Health. Features for
    # this target must include all those placements, not just medium health.
    result = {}
    for name in ITEMS:
        result[name] = np.asarray([item['origin'] for item in world['items']
            if not item['spawnflags'] & 2048 and (item['classname']==name or
                name=='item_health' and item['classname'].startswith('item_health_'))],np.float32).reshape(-1,3)
    return result


def encode_items(history, positions):
    origins=np.asarray([r['origin'] for r in history[1:]],np.float32)
    yaw=np.radians([r['view'][1] for r in history[1:]])
    chunks=[]
    for name in ITEMS:
        points=positions[name];encoded=np.zeros((len(origins),4),np.float32)
        if len(points):
            offsets=points[None]-origins[:,None]
            nearest=np.argmin((offsets*offsets).sum(axis=2),axis=1)
            delta=offsets[np.arange(len(origins)),nearest]/1280
            encoded[:,0]=delta[:,0]*np.cos(yaw)+delta[:,1]*np.sin(yaw)
            encoded[:,1]=-delta[:,0]*np.sin(yaw)+delta[:,1]*np.cos(yaw)
            encoded[:,2]=delta[:,2];encoded[:,3]=1
        chunks.append(encoded)
    return np.concatenate(chunks,axis=1)


def encode_packet_items(history):
    result=[]
    for row in history[1:]:
        world=dict(items=[dict(classname=e['item'],origin=e['origin'],spawnflags=0)
                          for e in row.get('packet_items') or []])
        encoded=encode_items([row,row],item_positions(world))[0]
        frame=row.get('gunframe')
        result.append(np.concatenate((encoded,[min(255,max(0,frame))/255 if frame is not None else 0,float(frame is not None)])))
    return np.asarray(result,np.float32)


def shot_index(path):
    tracks = {}
    for batch in pq.ParquetFile(path).iter_batches():
        for row in batch.to_pylist():
            if row['epoch_id'] is None:
                raise ValueError('Re-import project for epoch-attributed shots')
            key = (row['recording_id'], row['segment'], row['epoch_id'], row['slot'])
            tracks.setdefault(key, []).append(row)
    return {k: (sorted(v, key=lambda r:r['seq'])) for k,v in tracks.items()}


def labels(current, future, shots, seqs):
    """Labels only, never called while constructing past input features."""
    weapon = -1
    if future:
        for shot in shots[bisect_right(seqs, current['seq']):]:
            if shot['time_ms']-current['time_ms'] > 1500 or shot['seq'] > future[-1]['seq']:
                break
            if (shot['time_ms'] >= current['time_ms'] and shot['context_seq'] >= current['seq']
                    and shot['weapon'] in WEAPONS):
                weapon = WEAPONS.index(shot['weapon']); break
    picked = next((r for r in future if r.get('pickup_item')), None)
    complete = bool(future and (future[-1]['health'] <= 0 or
                               future[-1]['time_ms']-current['time_ms'] >= 2950))
    if picked:
        pickup = ITEMS.index(picked['pickup_item']) if picked['pickup_item'] in ITEMS else -1
    else:
        pickup = NONE if complete and all(r.get('pickup_observation_known') or r['health'] <= 0 for r in future) else -1
    return weapon, pickup


def prepare(sources, out, context=16, limit=6000, donor=None, event=None, cancelled=None):
    if context not in (4,8,16,32,64,128) or not 100 <= limit <= 24000:
        raise ValueError('Unsupported context/sample cap')
    out = Path(out)
    if out.exists(): raise ValueError('Use a new decision dataset generation')
    pools = {s:Reservoir(limit) for s in ('train','validation','test')}
    assignments = {}; provenance = []; counts = Counter()
    for source in sources:
        combat, groups_path, bsp = (Path(source[k]) for k in ('combat','groups','bsp'))
        shots_path = combat.parent/'shots.parquet'
        summary = json.loads((combat.parent/'summary.json').read_text())
        groups_doc = json.loads(groups_path.read_text())
        if summary.get('pickup_version') != PICKUP_VERSION:
            raise ValueError('Re-import project for timed HUD pickup evidence')
        if summary.get('packet_item_version') != PACKET_VERSION:
            raise ValueError('Re-import project for frame-ordered item observations')
        if summary['source_run'] != groups_doc['source_run']:
            raise ValueError('Group/recording provenance mismatch')
        world = inspect(bsp, source.get('map')); positions = sequences.landmark_positions(world)
        placements = item_positions(world)
        groups = {(r['recording_id'],r['segment'],r['epoch_id']):r for r in groups_doc['groups']}
        for g in groups.values():
            if g['split'] not in pools: continue
            if g['group'] in assignments and assignments[g['group']] != g['split']:
                raise ValueError('Whole match group leaks across partitions')
            assignments[g['group']] = g['split']
        fingerprints = {str(p):sha(p) for p in (combat,shots_path,groups_path)}
        provenance.append(dict(map=world['map'],bsp_sha256=world['bsp_sha256'],files=fingerprints))
        shots = shot_index(shots_path); shot_seqs = {k:[r['seq'] for r in v] for k,v in shots.items()}
        def rows():
            for batch in pq.ParquetFile(combat).iter_batches(batch_size=4096):
                if cancelled and cancelled(): raise InterruptedError('Decision preparation cancelled')
                if event: event(dict(stage='decision_sequences',map=world['map'],rows=counts['rows']))
                for row in batch.to_pylist():
                    counts['rows'] += 1
                    g = groups.get((row['recording_id'],row['segment'],row['epoch_id']))
                    if row['map'] != world['map'] or not g or g['split'] not in pools: continue
                    if donor and (not donor_match(row['aliases'],donor) or donor_match(row['opponent_aliases'],donor)): continue
                    yield row
        for history, future in sequences.observation_windows(add_memory(rows()),context,counts):
            current = history[-1]; key = (current['recording_id'],current['segment'],current['epoch_id'])
            track = key+(current['slot'],); g = groups[key]
            identity = ':'.join(map(str,track+(current['time_ms'],)))
            pool = pools[g['split']]
            if len(pool.heap)>=limit and int(hashlib.sha256(identity.encode()).hexdigest(),16)>-pool.heap[0][0]:
                continue
            weapon, pickup = labels(current,future,shots.get(track,[]),shot_seqs.get(track,[]))
            if weapon<0 and pickup<0: continue
            encoded = sequences.encode(history,positions)
            memory = np.asarray([r['decision_memory'] for r in history[1:]],np.float32)
            x = np.zeros((context,len(INPUTS)),np.float32)
            x[-len(encoded):] = np.concatenate((encoded,memory,encode_items(history,placements),encode_packet_items(history)),axis=1)
            if not np.isfinite(x).all() or abs(x).max()>30: continue
            nearest = sorted((float(np.linalg.norm(points-np.asarray(current['origin']),axis=1).min()),i)
                for i,points in enumerate(placements.values()) if len(points))
            pool.add(identity,dict(x=x,weapon=weapon,pickup=pickup,
                current_weapon=WEAPONS.index(current['weapon']) if current['weapon'] in WEAPONS else -1,
                nearest_item=nearest[0][1] if nearest else NONE,identity=identity,group=g['group'],
                map=world['map'],recording=current['recording_id']))
        for p,h in fingerprints.items():
            if sha(p)!=h: raise ValueError('Decision source changed during preparation')
    arrays = {}; splits = {}
    for split,pool in pools.items():
        selected = pool.rows()
        if not selected: raise ValueError('No eligible '+split+' decision samples')
        for field in FIELDS:
            dtype = np.float32 if field=='x' else np.int64 if field in ('weapon','pickup','current_weapon','nearest_item') else str
            arrays[split+'_'+field] = np.asarray([r[field] for r in selected],dtype=dtype)
        splits[split] = dict(samples=len(selected),groups=len({r['group'] for r in selected}),
            maps=dict(Counter(r['map'] for r in selected)),shots=sum(r['weapon']>=0 for r in selected),
            switched_shots=sum(r['weapon']>=0 and r['weapon']!=r['current_weapon'] for r in selected),
            pickup_edges=sum(0<=r['pickup']<NONE for r in selected))
    out.mkdir(parents=True); np.savez_compressed(out/'samples.npz',**arrays)
    meta = dict(feature_version=VERSION,inputs=INPUTS,context=context,splits=splits,counts=dict(counts),
        sources=provenance,donor=donor,weapon_labels=WEAPONS,pickup_labels=ITEMS+('no_observed_edge',),
        samples_sha256=sha(out/'samples.npz'),runtime_qualified=False,inventory_known=False,
        semantics='Next observed shot <=1.5s; next HUD pickup edge <=3s. Missing flash is masked. '
        'No observed edge does not prove no pickup; repeated same-name pickups can be hidden by lingering HUD.')
    atomic_json(out/'manifest.json',meta); return meta


def load(folder):
    folder = Path(folder); meta = json.loads((folder/'manifest.json').read_text())
    if meta.get('feature_version')!=VERSION or meta.get('inputs')!=INPUTS:
        raise ValueError('Incompatible decision feature schema')
    if sha(folder/'samples.npz')!=meta['samples_sha256']: raise ValueError('Decision dataset integrity mismatch')
    with np.load(folder/'samples.npz',allow_pickle=False) as raw: data = {k:raw[k] for k in raw.files}
    validate(data,meta['context'])
    return data,meta


def validate(data,context):
    seen = {}
    for split in ('train','validation','test'):
        x = data[split+'_x']; n = len(x)
        if x.shape!=(n,context,len(INPUTS)) or not np.isfinite(x).all(): raise ValueError('Invalid decision inputs')
        for field, high in (('weapon',len(WEAPONS)),('current_weapon',len(WEAPONS)),('pickup',NONE+1),('nearest_item',NONE+1)):
            a = data[split+'_'+field]
            if a.shape!=(n,) or not np.isin(a,range(-1,high)).all(): raise ValueError('Invalid decision label')
        for field in ('identity','group','map','recording'):
            if data[split+'_'+field].shape!=(n,): raise ValueError('Invalid decision identity')
        for group in data[split+'_group']:
            if group in seen and seen[group]!=split: raise ValueError('Decision split leakage')
            seen[group] = split


def replay(old,new,cap=24000):
    """Retain old map evidence; never move a held-out match into training."""
    context=new['train_x'].shape[1]
    assignments={};result={}
    for data in (old,new):
        validate(data,context)
        for split in ('train','validation','test'):
            for group in data[split+'_group']:
                if group in assignments and assignments[group]!=split:
                    raise ValueError('Decision replay group crossed partition')
                assignments[group]=split
    for split in ('train','validation','test'):
        rows={}
        for data in (old,new):
            for i,key in enumerate(data[split+'_identity']):
                if key in rows:
                    prior,j=rows[key]
                    if any(not np.array_equal(prior[split+'_'+f][j],data[split+'_'+f][i]) for f in FIELDS):
                        raise ValueError('Conflicting duplicate decision')
                rows[key]=(data,i)
        chosen=[]
        maps=sorted({str(d[split+'_map'][i]) for d,i in rows.values()})
        for name in maps:
            keys=[k for k,(d,i) in rows.items() if d[split+'_map'][i]==name]
            keys=sorted(keys,key=lambda k:hashlib.sha256(k.encode()).digest())
            # Bound training replay per map. All holdout examples survive.
            chosen+=keys[:max(1,cap//len(maps))] if split=='train' else keys
        for field in FIELDS:
            result[split+'_'+field]=np.asarray([rows[k][0][split+'_'+field][rows[k][1]] for k in chosen])
    validate(result,context)
    return result
