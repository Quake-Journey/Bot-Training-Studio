"""Causal movement-mechanism expert datasets from raw, attributed player states.

Separate schema keeps installed v3 factory weights and user overlays compatible.
The expert predicts observed futures, not optimal actions or playable nav links.
"""
from collections import Counter, deque
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

from .data import Reservoir, sha
from .learning import atomic_json
from .maps import inspect
from .transitions import classify, living, VERSION as TRANSITION_VERSION

VERSION = 'observed-mechanisms-v1'
EVENTS = ('teleport_within_1s', 'push_within_1s')
CONTINUOUS = ('destination_forward_1s', 'destination_left_1s', 'destination_up_1s',
              'velocity_forward_1s', 'velocity_left_1s', 'velocity_up_1s')
INPUTS = ('velocity_forward','velocity_left','velocity_up','pitch','yaw_sin','yaw_cos',
          'health','armor','grounded','teleport_hold','gravity',
          'portal_forward','portal_left','portal_up','exit_forward','exit_left','exit_up','portal_exists',
          'pad_forward','pad_left','pad_up','launch_forward','launch_left','launch_up','pad_exists','history_known')
FIELDS = ('x','y','y_mask','events','event_mask','identity','group','map','recording')


def local(vector, yaw, scale):
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    return [(vector[0]*c+vector[1]*s)/scale,(-vector[0]*s+vector[1]*c)/scale,vector[2]/scale]


def encode(history, mechanisms, context):
    rows = []
    for state in history:
        xyz, yaw = state['origin'], state['view_angles'][1]
        values = local(state['velocity'],yaw,1000) + [state['view_angles'][0]/180,
            math.sin(math.radians(yaw)),math.cos(math.radians(yaw)),state['stats'][1]/200,
            state['stats'][5]/200,float(bool(state['pm_flags']&4)),float(bool(state['pm_flags']&32)),state['gravity']/800]
        for kind in ('teleport','push'):
            candidates = [m for m in mechanisms if m['kind']==kind and m['resolved']]
            if candidates:
                nearest = min(candidates,key=lambda m:sum((xyz[i]-(m['bounds'][0][i]+m['bounds'][1][i])/2)**2 for i in range(3)))
                center = [(a+b)/2 for a,b in zip(*nearest['bounds'])]
                values += local([b-a for a,b in zip(xyz,center)],yaw,1280)
                values += (local([b-a for a,b in zip(xyz,nearest['destination'])],yaw,1280)
                           if kind=='teleport' else local(nearest['velocity'],yaw,1000)) + [1.]
            else:
                values += [0.]*7
        rows.append(values+[1.])
    result = np.zeros((context,len(INPUTS)),np.float32)
    result[-len(rows):] = rows
    return result


def observation_windows(rows, mechanisms, context, counts):
    """Continue a witnessed teleport/push, censor gaps; never bridge respawns."""
    tracks = {}
    def flush(state):
        for history, future in state['pending']:
            if future: yield history, future
        state['pending'].clear()
    for row in rows:
        key = (row['recording_id'],row['segment'],row['epoch_id'],row['slot'])
        state = tracks.setdefault(key,dict(history=deque(maxlen=context),pending=[]))
        history = state['history']; previous = history[-1] if history else None
        edge = classify(previous,row,mechanisms)
        terminal = bool(previous and living(previous) and row['stats'][1]<=0 and
                        70<=row['time_ms']-previous['time_ms']<=130)
        counts['edge/'+edge['kind']] += 1
        if previous and not edge['known'] and not terminal:
            yield from flush(state); history.clear()
        if edge['known'] or terminal:
            pending = []
            for past, future in state['pending']:
                future.append((row,edge))
                if terminal or row['time_ms']-past[-1]['time_ms']>=1000:
                    yield past,future
                else: pending.append((past,future))
            state['pending'] = pending
        if not living(row):
            history.clear(); continue
        history.append(row)
        # Uniform anchors, including early lives. No positive oversampling of holdouts.
        if row['time_ms'] % 200 == 0:
            state['pending'].append((list(history),[]))
    for state in tracks.values(): yield from flush(state)


def targets(history, future):
    current = history[-1]
    end = next((r for r,e in future if 970<=r['time_ms']-current['time_ms']<=1030 and living(r)),None)
    terminal = any(r['stats'][1]<=0 for r,e in future)
    events = np.asarray([any(e['kind']==kind for r,e in future) for kind in ('teleport','push')],np.float32)
    event_mask = np.maximum(events,float(end is not None or terminal))
    y = np.zeros(6,np.float32); mask = np.zeros(6,np.float32)
    if end:
        y[:3] = local([b-a for a,b in zip(current['origin'],end['origin'])],current['view_angles'][1],1280)
        y[3:] = local(end['velocity'],current['view_angles'][1],1000); mask[:]=1
    return dict(y=y,y_mask=mask,events=events,event_mask=event_mask)


def record_rows(record, groups, counts):
    directory = Path(record['path'])
    facts = json.loads((directory/'facts.json').read_text(encoding='utf8'))
    epochs = [f for f in facts if f['kind']=='epoch']
    players = {}
    tracks = {f['track_id']:f for f in facts if f['kind']=='track'}
    for f in facts:
        if f['kind']=='participant' and f.get('active') and f['track_id'] in tracks:
            players.setdefault(f['epoch_id'],[]).append(tracks[f['track_id']])
    for batch in pq.ParquetFile(directory/'player_states.parquet').iter_batches(batch_size=4096):
        for row in batch.to_pylist():
            if record.get('role')=='tricks':
                actor=[t for t in tracks.values() if t['segment']==row['segment'] and t['slot']==row['slot'] and
                       t.get('is_recorder_pov') and not t.get('chase_spectator') and
                       t['time_ms']<=row['time_ms']<=t['end_ms'] and
                       (not t.get('identity_end_ms') or row['time_ms']<t['identity_end_ms'])]
                if len(actor)!=1 or len(row['stats'])<18:continue
                if not all(math.isfinite(v) for k in ('origin','velocity','view_angles') for v in row[k]):
                    raise ValueError('Invalid trick observation')
                counts['trick_rows']+=1
                yield dict(row,recording_id=record['recording_id'],epoch_id=-1-actor[0]['track_id'],
                    group='trick-'+record['recording_id'],split='train')
                continue
            candidates = [e for e in epochs if e['segment']==row['segment'] and e['time_ms']<=row['time_ms']<e['end_ms']]
            if len(candidates)!=1: continue
            epoch = candidates[0]; key=(record['recording_id'],row['segment'],epoch['epoch_id'])
            group = groups.get(key)
            if not group or group['split'] not in ('train','validation','test'): continue
            actor = [t for t in players.get(epoch['epoch_id'],[]) if t['slot']==row['slot'] and
                     t['time_ms']<=row['time_ms']<=t['end_ms'] and not t.get('chase_spectator') and
                     (not t.get('identity_end_ms') or row['time_ms']<t['identity_end_ms'])]
            if len(actor)!=1 or len(row['stats'])<18: continue
            if not all(math.isfinite(v) for k in ('origin','velocity','view_angles') for v in row[k]):
                raise ValueError('Invalid raw movement observation')
            counts['rows'] += 1
            yield dict(row,recording_id=record['recording_id'],epoch_id=epoch['epoch_id'],
                       group=group['group'],split=group['split'])


def prepare(projects, out, context=16, limit=12000, event=None, cancelled=None):
    if context not in (4,8,16,32,64,128) or not 100<=limit<=24000: raise ValueError('Unsupported dataset bounds')
    out = Path(out)
    if out.exists(): raise ValueError('Use a new dataset generation')
    pools = {split:Reservoir(limit) for split in ('train','validation','test')}
    counts = Counter(); provenance=[]; assignments={}
    transform_hashes={name:sha(Path(__file__).with_name(name)) for name in ('maps.py','transitions.py','mechanics_sequences.py')}
    roles={}
    for project in map(Path,projects):
        meta=json.loads((project/'project.json').read_text(encoding='utf8'))
        world=inspect(project/'map.bsp',meta['map']);revision=project/'revisions'/meta['active_revision']
        if world['bsp_sha256']!=meta['bsp_sha256']: raise ValueError('Project BSP changed')
        doc=json.loads((revision/'groups.json').read_text(encoding='utf8'))
        if doc['source_run']!=meta['active_revision']: raise ValueError('Group revision mismatch')
        groups={(g['recording_id'],g['segment'],g['epoch_id']):g for g in doc['groups']}
        provenance.append(dict(map=world['map'],bsp_sha256=world['bsp_sha256'],source_run=meta['active_revision'],
                               groups_sha256=sha(revision/'groups.json'),recordings=[]))
        for record in meta['recordings']:
            if cancelled and cancelled(): raise InterruptedError('Mechanism dataset cancelled')
            # Teaching tricks are training-only; validation/test stay natural games.
            role=record.get('role','game')
            if role not in ('game','tricks'):continue
            if record['recording_id'] in roles:
                if roles[record['recording_id']]!=role:raise ValueError('Recording has conflicting game/trick roles')
                continue
            roles[record['recording_id']]=role
            directory=Path(record['path']);rawmeta=json.loads((directory/'manifest.json').read_text(encoding='utf8'))
            for file in ('player_states.parquet','facts.json'):
                expected=rawmeta['artifacts'][file]['sha256']
                if sha(directory/file)!=expected: raise ValueError('Raw observation integrity failure')
            provenance[-1]['recordings'].append(dict(id=record['recording_id'],role=role,
                states_sha256=sha(directory/'player_states.parquet'),facts_sha256=sha(directory/'facts.json')))
            rows=record_rows(record,groups,counts)
            for history,future in observation_windows(rows,world['transitions'],context,counts):
                current=history[-1];target=targets(history,future)
                if not target['y_mask'].any() and not target['event_mask'].any():continue
                split,group=current['split'],current['group']
                if group in assignments and assignments[group]!=split:raise ValueError('Whole-match leakage')
                assignments[group]=split
                identity=':'.join(map(str,(current['recording_id'],current['segment'],current['epoch_id'],current['slot'],current['time_ms'])))
                x=encode(history,world['transitions'],context)
                if not np.isfinite(x).all() or np.abs(x).max()>30:raise ValueError('Invalid mechanism inputs')
                pools[split].add(identity,dict(target,x=x,identity=identity,group=group,map=world['map'],recording=current['recording_id']))
            if event:event(dict(stage='mechanisms_prepare',map=world['map'],recordings=len(provenance[-1]['recordings']),counts=dict(counts)))
    arrays={};splits={}
    for split,pool in pools.items():
        rows=pool.rows()
        if not rows:raise ValueError('No eligible '+split+' matches')
        for field in FIELDS:
            arrays[split+'_'+field]=np.asarray([r[field] for r in rows],dtype=np.float32 if field in ('x','y','y_mask','events','event_mask') else str)
        splits[split]=dict(samples=len(rows),groups=len({r['group'] for r in rows}),maps=dict(Counter(r['map'] for r in rows)),
            events={e:dict(positive=int(arrays[split+'_events'][:,i].sum()),known=int(arrays[split+'_event_mask'][:,i].sum())) for i,e in enumerate(EVENTS)})
    if any(sha(Path(__file__).with_name(name))!=digest for name,digest in transform_hashes.items()):
        raise ValueError('Mechanism analysis code changed during preparation')
    out.mkdir(parents=True);np.savez_compressed(out/'samples.npz',**arrays)
    manifest=dict(feature_version=VERSION,transition_version=TRANSITION_VERSION,context=context,inputs=INPUTS,
        continuous_targets=CONTINUOUS,event_targets=EVENTS,sources=provenance,splits=splits,counts=dict(counts),
        samples_sha256=sha(out/'samples.npz'),source_sha256=transform_hashes,
        runtime_qualified=False,may_activate_in_game=False,
        semantics='Observed one-second movement/teleport/push futures; geometry-consistent inference is not native action replay')
    atomic_json(out/'manifest.json',manifest);return manifest


def load(folder):
    folder=Path(folder);meta=json.loads((folder/'manifest.json').read_text(encoding='utf8'))
    if meta['feature_version']!=VERSION or sha(folder/'samples.npz')!=meta['samples_sha256']:
        raise ValueError('Mechanism dataset schema/integrity mismatch')
    with np.load(folder/'samples.npz',allow_pickle=False) as archive:data={k:archive[k] for k in archive.files}
    seen={}
    for split in ('train','validation','test'):
        x=data[split+'_x'];n=len(x)
        if x.shape!=(n,meta['context'],len(INPUTS)) or not np.isfinite(x).all():raise ValueError('Invalid mechanism inputs')
        for key,width in (('y',6),('y_mask',6),('events',2),('event_mask',2)):
            a=data[split+'_'+key]
            if a.shape!=(n,width) or not np.isfinite(a).all():raise ValueError('Invalid mechanism targets')
            if key!='y' and not np.isin(a,[0,1]).all():raise ValueError('Invalid mechanism masks')
        if np.any(data[split+'_events']>data[split+'_event_mask']):raise ValueError('Unknown positive mechanism event')
        for group in set(data[split+'_group']):
            if group in seen and seen[group]!=split:raise ValueError('Whole-match leakage')
            seen[group]=split
    return data,meta
