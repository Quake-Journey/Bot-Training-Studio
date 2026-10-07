"""Past-only map-conditioned sequences; targets are observations, not optimal actions."""
from collections import Counter, deque
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

from .data import FEATURES, WEAPONS, Reservoir, donor_match, features, local, sha, valid_transition
from .learning import atomic_json
from .maps import inspect

VERSION = "observed-game-sequence-v3"
LANDMARKS = ("weapon_railgun", "weapon_chaingun", "weapon_rocketlauncher", "weapon_supershotgun",
             "weapon_hyperblaster", "weapon_grenadelauncher", "item_armor_body", "item_armor_combat",
             "item_armor_jacket", "item_health_mega", "item_adrenaline", "item_health", "item_pack", "ammo")
EXTRA = ["ray_clear", "ray_known", "enemy_in_horizontal_fov", "eye_known", "rockets_observed", "grenades_observed"]
COMBAT_INPUTS = ['ammo_current', 'ammo_known', 'last_shot_age', 'last_shot_known',
                 'enemy_velocity_forward','enemy_velocity_left','enemy_velocity_up','enemy_velocity_known']
INPUTS = FEATURES + EXTRA + [f"{item}_{axis}" for item in LANDMARKS for axis in ("forward", "left", "up", "exists")] + COMBAT_INPUTS + ['history_known']
CONTINUOUS = ["velocity_forward_300ms", "velocity_left_300ms", "velocity_up_300ms",
              "displacement_forward_3s", "displacement_left_3s", "displacement_up_3s",
              "health_change_3s", "armor_change_3s"]
EVENTS = ['death_within_3s', 'stack_loss_ge20_within_3s', 'resource_gain_within_3s']
FIELDS = ('x', 'y', 'y_mask', 'weapon', 'landmark', 'events', 'event_mask',
          'identity', 'group', 'map', 'recording')


def landmark_positions(world):
    result = {}
    for name in LANDMARKS:
        result[name] = np.asarray([item["origin"] for item in world["items"] if
            (item["classname"].startswith("ammo_") if name == "ammo" else item["classname"] == name)
            and not item["spawnflags"] & 2048], dtype=np.float32).reshape(-1, 3)
    return result


def encode(history, positions):
    base = np.asarray([features(previous, row) + [float(bool(row.get("center_ray_clear"))),
        float(bool(row.get("collision_complete"))), float(bool(row.get("horizontal_fov"))),
        float(bool(row.get("eye_recorded"))), min(20, row.get("packet_rockets") or 0) / 20,
        min(20, row.get("packet_grenades") or 0) / 20] for previous, row in zip(history, history[1:])], dtype=np.float32)
    origins = np.asarray([r["origin"] for r in history[1:]], dtype=np.float32)
    yaw = np.radians([r["view"][1] for r in history[1:]])
    chunks = [base]
    for name in LANDMARKS:
        points = positions[name]
        encoded = np.zeros((len(origins), 4), dtype=np.float32)
        if len(points):
            offsets = points[None] - origins[:, None]
            nearest = np.argmin((offsets * offsets).sum(axis=2), axis=1)
            delta = offsets[np.arange(len(origins)), nearest] / 1280
            encoded[:, 0] = delta[:, 0] * np.cos(yaw) + delta[:, 1] * np.sin(yaw)
            encoded[:, 1] = -delta[:, 0] * np.sin(yaw) + delta[:, 1] * np.cos(yaw)
            encoded[:, 2] = delta[:, 2]; encoded[:, 3] = 1
        chunks.append(encoded)
    combat=[]
    for row in history[1:]:
        ammo_known=bool(row.get('ammo_known')) and row.get('ammo_observed') is not None
        shot_known=bool(row.get('last_shot_known')) and row.get('last_shot_age_ms') is not None
        enemy_velocity=row.get('packet_enemy_velocity')
        combat.append([min(1000,max(0,row['ammo_observed']))/200 if ammo_known else 0,float(ammo_known),
            min(10000,max(0,row['last_shot_age_ms']))/10000 if shot_known else 0,float(shot_known)]
            +(local(enemy_velocity,row['view'][1]) if enemy_velocity is not None else [0,0,0])
            +[float(enemy_velocity is not None)])
    chunks.append(np.asarray(combat,np.float32))
    chunks.append(np.ones((len(origins), 1), dtype=np.float32))
    return np.concatenate(chunks, axis=1)


def observation_windows(rows, context, counts):
    """Emit living anchors with observed, censored or terminal futures.

    A death ends all pending horizons and the life. Missing frames/teleports
    censor them: an unobserved future is never labeled survival. Early-life
    histories are short and explicitly left padded, not removed from training.
    """
    tracks = {}
    recording = None
    def flush(state):
        for history, future in state['pending']:
            if future:
                yield history, future
        state['pending'].clear()
    for row in rows:
        if row['recording_id'] != recording:
            for state in tracks.values():
                yield from flush(state)
            tracks.clear(); recording = row['recording_id']
        key = (row['recording_id'], row['segment'], row['epoch_id'], row['slot'])
        state = tracks.setdefault(key, dict(history=deque(maxlen=context+1), pending=[]))
        history = state['history']
        valid = (row['health'] is not None and row['armor'] is not None and
                 all(v is not None and math.isfinite(v) for f in ('origin','view','velocity') for v in row[f]))
        if not valid:
            yield from flush(state); history.clear(); counts['invalid_state'] += 1
            continue
        previous = history[-1] if history else None
        terminal = bool(previous and previous['health']>0 and row['health']<=0
                        and 70<=row['time_ms']-previous['time_ms']<=130)
        continuous = previous and valid_transition(previous, row)
        if previous and not continuous and not terminal:
            yield from flush(state); history.clear(); counts['discontinuity'] += 1
        if continuous or terminal:
            pending = []
            for past, future in state['pending']:
                future.append(row)
                if terminal or row['time_ms']-past[-1]['time_ms']>=2950:
                    yield past, future
                else:
                    pending.append((past, future))
            state['pending'] = pending
        if row['health']<=0:
            history.clear(); counts['terminal_rows'] += 1
            continue
        history.append(row)
        if len(history)>=2 and row['time_ms'] % 500 == 0:
            state['pending'].append((list(history), []))
    for state in tracks.values():
        yield from flush(state)


def targets(history, future, positions):
    current = history[-1]
    terminal = next((r for r in future if r['health']<=0), None)
    soon = next((r for r in future if 270<=r['time_ms']-current['time_ms']<=330 and r['health']>0), None)
    end = next((r for r in future if 2950<=r['time_ms']-current['time_ms']<=3050 and r['health']>0), None)
    y = np.zeros(len(CONTINUOUS), np.float32); mask = np.zeros_like(y)
    weapon = landmark = -1
    if soon:
        y[:3] = local(soon['velocity'], current['view'][1]); mask[:3] = 1
        weapon = WEAPONS.index(soon['weapon']) if soon['weapon'] in WEAPONS else -1
    if end:
        delta = [b-a for a,b in zip(current['origin'], end['origin'])]
        y[3:6] = [v/4 for v in local(delta, current['view'][1])]; mask[3:6] = 1
        y[6:] = [(end['health']-current['health'])/100, (end['armor']-current['armor'])/200]; mask[6:] = 1
        near = [(float(np.linalg.norm(p-np.asarray(end['origin']),axis=1).min()),i)
                for i,p in enumerate(positions.values()) if len(p)]
        nearest = min(near) if near else (math.inf, len(LANDMARKS))
        landmark = nearest[1] if nearest[0]<=128 else len(LANDMARKS)
    # Death is a separate endpoint: do not teach corpse/respawn positions or
    # misrepresent terminal health as a measured state exactly three seconds on.
    chain = [current] + future
    # A one-point megahealth decay is not a useful combat-risk target. Sum
    # observed health/armor losses, ignoring later healing, and require 20.
    # This still includes self-damage; do not label it "enemy hit".
    lost = sum(max(0,a['health']-max(0,b['health']))+max(0,a['armor']-b['armor'])
               for a,b in zip(chain,chain[1:]))>=20
    gained = any(b['health']>a['health'] or b['armor']>a['armor'] for a,b in zip(chain,chain[1:]))
    events = np.asarray([bool(terminal), lost, gained], np.float32)
    complete = bool(terminal or end)
    event_mask = np.maximum(events, float(complete))
    return dict(y=y, y_mask=mask, weapon=weapon, landmark=landmark, events=events, event_mask=event_mask)


def prepare(sources, out, context=16, limit=2000, donor=None, event=None, cancelled=None):
    """sources contain combat/groups/bsp; whole opponent pair assignments survive updates."""
    if context not in (4, 8, 16, 32, 64, 128) or not 100 <= limit <= 24000:
        raise ValueError("Unsupported history or sample cap")
    out = Path(out)
    if out.exists():
        raise ValueError("Use a new dataset generation")
    pools = {s: Reservoir(limit) for s in ("train", "validation", "test")}
    counts, provenance, assignments = Counter(), [], {}
    columns = ["recording_id", "segment", "epoch_id", "slot", "time_ms", "seq", "map", "aliases",
        "opponent_aliases", "velocity", "origin", "view", "health", "armor", "weapon", "grounded",
        "packet_enemy_origin", "yaw_rate", "continuous", "center_ray_clear", "collision_complete",
        "horizontal_fov", "eye_recorded", "packet_rockets", "packet_grenades",
        "ammo_observed", "ammo_known", "last_shot_age_ms", "last_shot_known", "packet_enemy_velocity"]
    for source_index, source in enumerate(sources):
        combat, groupfile, bsp = (Path(source[k]) for k in ("combat", "groups", "bsp"))
        world = inspect(bsp, source.get("map")); positions = landmark_positions(world)
        document = json.loads(groupfile.read_text(encoding="utf8"))
        groups = {(r["recording_id"], r["segment"], r["epoch_id"]): r for r in document["groups"]}
        source_summary = json.loads((combat.parent / "summary.json").read_text(encoding="utf8"))
        from bts_analysis.combat import OBSERVATION_VERSION
        if source_summary.get('observation_version') != OBSERVATION_VERSION:
            raise ValueError('Re-import project to retain terminal/ammo observations before preparing v3 sequences')
        if source_summary["source_run"] != document["source_run"]:
            raise ValueError("Recording/group provenance mismatch")
        for group in groups.values():
            if group["split"] == "quarantine":
                continue
            key = group["group"]
            if key in assignments and assignments[key] != group["split"]:
                raise ValueError("Whole match group leaks across partitions")
            assignments[key] = group["split"]
        before = combat.stat()
        provenance.append(dict(map=world["map"], combat_sha256=sha(combat), groups_sha256=sha(groupfile),
                               bsp_sha256=world["bsp_sha256"], source_run=document["source_run"]))
        def eligible_rows():
            for batch in pq.ParquetFile(combat).iter_batches(batch_size=4096, columns=columns):
                if cancelled and cancelled():
                    raise InterruptedError('Sequence preparation cancelled')
                if event:
                    event(dict(stage='sequences', source=source_index+1, sources=len(sources), rows=counts['rows']))
                for r in batch.to_pylist():
                    counts['rows'] += 1
                    if r['map'] != world['map']:
                        counts['other_map'] += 1; continue
                    group = groups.get((r['recording_id'],r['segment'],r['epoch_id']))
                    if not group or group['split'] not in pools:
                        counts['quarantined'] += 1; continue
                    if donor and (not donor_match(r['aliases'],donor) or donor_match(r['opponent_aliases'],donor)):
                        continue
                    yield r
        for history, future in observation_windows(eligible_rows(), context, counts):
            current = history[-1]
            key = (current['recording_id'],current['segment'],current['epoch_id'])
            group = groups[key]
            identity = ':'.join(map(str, key+(current['slot'],current['time_ms'])))
            pool = pools[group['split']]
            rank = int(hashlib.sha256(identity.encode()).hexdigest(),16)
            if len(pool.heap)>=limit and rank>-pool.heap[0][0]:
                counts['reservoir_skipped'] += 1; continue
            result = targets(history,future,positions)
            if not result['y_mask'].any() and not result['event_mask'].any():
                counts['unobserved_future'] += 1; continue
            encoded = encode(history,positions)
            x = np.zeros((context,len(INPUTS)),np.float32); x[-len(encoded):] = encoded
            if not np.isfinite(x).all() or not np.isfinite(result['y']).all() or abs(x).max()>30:
                counts['outlier'] += 1; continue
            pool.add(identity,dict(result,x=x,identity=identity,group=group['group'],map=world['map'],recording=current['recording_id']))
        after = combat.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("Input changed during preparation")
    arrays, splits = {}, {}
    for split, pool in pools.items():
        rows = pool.rows()
        if not rows:
            raise ValueError("No eligible " + split + " matches; add independent recordings")
        for field in FIELDS:
            dtype = np.float32 if field in ("x", "y", "y_mask", "events", "event_mask") else np.int64 if field in ("weapon", "landmark") else str
            arrays[split + "_" + field] = np.asarray([r[field] for r in rows], dtype=dtype)
        splits[split] = dict(samples=len(rows), groups=len({r["group"] for r in rows}), maps=dict(Counter(r["map"] for r in rows)))
        splits[split]['events'] = {name:dict(positive=int(arrays[split+'_events'][:,i].sum()),
            known=int(arrays[split+'_event_mask'][:,i].sum())) for i,name in enumerate(EVENTS)}
        splits[split]['short_histories'] = sum(int((r['x'][:,-1]==0).any()) for r in rows)
    out.mkdir(parents=True)
    np.savez_compressed(out / "samples.npz", **arrays)
    manifest = dict(schema=2, feature_version=VERSION, inputs=INPUTS, continuous_targets=CONTINUOUS, event_targets=EVENTS,
        weapon_labels=WEAPONS, landmark_labels=LANDMARKS + ("none_nearby",), context=context, donor=donor,
        sources=provenance, splits=splits, counts=dict(counts), samples_sha256=sha(out / "samples.npz"),
        runtime_qualified=False, semantics="Observed masked futures and terminal events; not optimal actions or confirmed pickups",
        availability_known=False, inventory_known=False, sampling='uniform deterministic anchors; holdouts never event-balanced')
    atomic_json(out / "manifest.json", manifest)
    return manifest


def load(folder):
    folder = Path(folder)
    meta = json.loads((folder / "manifest.json").read_text(encoding="utf8"))
    if meta["feature_version"] != VERSION:
        raise ValueError('Re-import project and prepare a new v3 dataset; original dataset remains intact')
    if sha(folder / "samples.npz") != meta["samples_sha256"]:
        raise ValueError("Sequence dataset integrity mismatch")
    with np.load(folder / "samples.npz", allow_pickle=False) as raw:
        data = {key: raw[key] for key in raw.files}
    seen = {}
    for split in ("train", "validation", "test"):
        x, y = data[split + "_x"], data[split + "_y"]
        if x.shape[1:] != (meta["context"], len(INPUTS)) or y.shape != (len(x), len(CONTINUOUS)) or not np.isfinite(x).all() or not np.isfinite(y).all():
            raise ValueError("Invalid sequence tensors")
        for field,width in (('y_mask',len(CONTINUOUS)),('events',len(EVENTS)),('event_mask',len(EVENTS))):
            a=data[split+'_'+field]
            if a.shape!=(len(x),width) or not np.isin(a,[0,1]).all():raise ValueError('Invalid target masks/events')
        if np.any(data[split+'_events']>data[split+'_event_mask']):raise ValueError('Unknown positive event')
        for field,count in (('weapon',len(WEAPONS)),('landmark',len(LANDMARKS)+1)):
            a=data[split+'_'+field]
            if a.shape!=(len(x),) or not np.isin(a,range(-1,count)).all():raise ValueError('Invalid categorical targets')
        for group in set(data[split + "_group"]):
            if group in seen and seen[group] != split:
                raise ValueError("Sequence split leakage")
            seen[group] = split
    return data, meta
