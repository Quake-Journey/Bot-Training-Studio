"""Full-cadence movement evidence from the existing decoded cache, without LLMs.

Network snapshots are observations, not original inputs. Maneuver names require
additional contact/projectile evidence; this tool exports measurable kinematics
and jump candidates, never fabricates confirmed rocket/double/strafe jumps.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import time
import pyarrow as pa
import pyarrow.parquet as pq

SCHEMA = pa.schema([
    ('recording_id', pa.string()), ('segment', pa.int32()), ('slot', pa.int32()),
    ('time_ms', pa.int64()), ('seq', pa.int64()), ('dt_ms', pa.int32()),
    ('epoch_fingerprint', pa.string()), ('actor_slot_attributed', pa.bool_()),
    ('aliases', pa.list_(pa.string())),
    ('origin', pa.list_(pa.float32())), ('velocity', pa.list_(pa.float32())),
    ('yaw', pa.float32()), ('pitch', pa.float32()), ('speed_xy', pa.float32()),
    ('yaw_rate', pa.float32()), ('pitch_rate', pa.float32()),
    ('yaw_accel', pa.float32()), ('accel_xy', pa.float32()),
    ('grounded', pa.bool_()), ('jump_candidate', pa.bool_()),
    ('air_speed_gain', pa.float32()), ('heading_view_delta', pa.float32()),
    ('health', pa.int32()), ('armor', pa.int32()),
])

def angle(a, b):
    return (a - b + 180) % 360 - 180

def quantiles(values):
    if not values: return {'n': 0}
    s = sorted(values)
    return dict(n=len(s), **{str(q): s[round((len(s)-1)*q)]
                            for q in [0, .1, .25, .5, .75, .9, .95, .99, 1]})

def process(root, out, mapname='q2duel5'):
    start = time.monotonic()
    snapshot = json.loads((root/'current.json').read_text(encoding='utf-8'))
    out.mkdir(parents=True, exist_ok=True)
    counts, samples = Counter(), defaultdict(list)
    writer = pq.ParquetWriter(out/'motion.parquet.new', SCHEMA, compression='zstd')
    rows = []
    for record in snapshot['records']:
        if record['status'] != 'decoded': continue
        directory = root/record['path']
        facts = json.loads((directory/'facts.json').read_text(encoding='utf-8'))
        segments = {v['segment']: v for v in facts if v['kind'] == 'segment_facts'}
        tracks, epochs = defaultdict(list), defaultdict(list)
        for v in facts:
            if v['kind'] == 'track': tracks[(v['segment'],v['slot'])].append(v)
            if v['kind'] == 'epoch': epochs[v['segment']].append(v)
        previous, derivatives = {}, {}
        for batch in pq.ParquetFile(directory/'player_states.parquet').iter_batches(batch_size=8192):
            for s in batch.to_pylist():
                counts['states_read'] += 1
                seg, slot, t, st = s['segment'], s['slot'], s['time_ms'], s['stats']
                key = seg, slot
                old = previous.get(key)
                previous[key] = s
                if segments.get(seg,{}).get('map') != mapname:
                    counts['other_map'] += 1; continue
                if (s['pm_type'] != 0 or len(st) <= 17 or st[17] or st[16] or
                    not all(math.isfinite(x) for f in ['origin','velocity','view_angles'] for x in s[f])):
                    derivatives.pop(key,None); counts['non_actor_or_invalid'] += 1; continue
                live = [e for e in epochs[seg] if e['start_conf']==2 and e['time_ms']<=t<=e['end_ms']]
                if len(live) != 1:
                    derivatives.pop(key,None); counts['no_live_epoch'] += 1; continue
                dt = t-old['time_ms'] if old else 0
                if (not old or not 0 < dt <= 250 or old['pm_type'] != 0 or
                    old['stats'][17] or old['stats'][16] or s['pm_flags']&32 or
                    math.dist(s['origin'],old['origin']) > 1.5*dt or
                    old['time_ms'] < live[0]['time_ms']):
                    derivatives.pop(key,None); counts['discontinuity'] += 1; continue
                active = [v for v in tracks[key] if v['time_ms']<=t<=v['end_ms'] and
                          (not v['identity_end_ms'] or t<v['identity_end_ms']) and
                          v['active_seen'] and not v['chase_spectator']]
                track = active[0] if len(active)==1 else {}
                sec=dt/1000
                speed=math.hypot(*s['velocity'][:2]); oldspeed=math.hypot(*old['velocity'][:2])
                yr=angle(s['view_angles'][1],old['view_angles'][1])/sec
                pr=angle(s['view_angles'][0],old['view_angles'][0])/sec
                prev=derivatives.get(key)
                ya=(yr-prev[0])/((sec+prev[1])*.5) if prev else None
                derivatives[key]=yr,sec
                grounded=bool(s['pm_flags']&4)
                airborne=not grounded and not old['pm_flags']&4
                ac=math.hypot(s['velocity'][0]-old['velocity'][0],s['velocity'][1]-old['velocity'][1])/sec
                jump=bool(old['pm_flags']&4 and not grounded and s['velocity'][2]>0)
                group='ground' if grounded else 'air'
                for name,value in [('yaw_rate',abs(yr)),('pitch_rate',abs(pr)),('speed_xy',speed),('accel_xy',ac)]:
                    samples[group+'/'+name].append(value)
                if ya is not None: samples[group+'/yaw_accel'].append(abs(ya))
                counts['jump_candidates']+=jump
                counts['attributed_rows']+=bool(track)
                counts['motion_rows']+=1
                rows.append(dict(recording_id=record['recording_id'],segment=seg,slot=slot,time_ms=t,seq=s['seq'],
                    dt_ms=dt,epoch_fingerprint=live[0]['fingerprint'],actor_slot_attributed=bool(track),aliases=track.get('aliases',[]),
                    origin=s['origin'],velocity=s['velocity'],yaw=s['view_angles'][1],pitch=s['view_angles'][0],speed_xy=speed,
                    yaw_rate=yr,pitch_rate=pr,yaw_accel=ya,accel_xy=ac,grounded=grounded,jump_candidate=jump,
                    air_speed_gain=speed-oldspeed if airborne else None,
                    heading_view_delta=angle(math.degrees(math.atan2(s['velocity'][1],s['velocity'][0])),s['view_angles'][1]) if speed>20 else None,
                    health=st[1],armor=st[5]))
                if len(rows)>=8192:
                    writer.write_table(pa.Table.from_pylist(rows,schema=SCHEMA)); rows=[]
        counts['recordings']+=1
    if rows: writer.write_table(pa.Table.from_pylist(rows,schema=SCHEMA))
    writer.close()
    (out/'motion.parquet.new').replace(out/'motion.parquet')
    report=dict(schema=1,map=mapname,source_run=snapshot['run_id'],counts=dict(counts),
        seconds=round(time.monotonic()-start,3),quantiles={k:quantiles(v) for k,v in samples.items()},
        original_usercmd_available=False,confirmed_maneuvers=False,
        limitations=['Network-cadence observations include quantization; sub-frame hand inputs are unobserved.',
            'Cross-recording match deduplication and held-out splits remain required before fitting.',
            'Jump candidates are not proof of double, strafe or rocket jumps.',
            'Player aliases are evidence, not verified real-person identity.'])
    (out/'motion-summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ['source_run','counts','seconds']}))
    return report

if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('--data',required=True,type=Path); ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--map',default='q2duel5')
    args=ap.parse_args(); process(args.data,args.out,args.map)
