"""Conditional donor evidence, not a hand-written imitation or optimal policy.

Only training matches enter the profile. Validation/test observations remain
available for later comparison, never for constructing exported preferences.
"""
from collections import Counter, defaultdict
import json
import math
from pathlib import Path

import pyarrow.parquet as pq

from .data import donor_match


def classify(row):
    """Describe recorded motion relative to a known enemy, without guessing intent."""
    enemy=row.get('packet_enemy_origin')
    if enemy is None:
        return None
    delta=[b-a for a,b in zip(row['origin'],enemy)]
    distance=math.hypot(*delta[:2]); velocity=row['velocity']
    if distance<1 or not all(math.isfinite(v) for v in delta+velocity):
        return None
    radial=(velocity[0]*delta[0]+velocity[1]*delta[1])/distance
    lateral=(-velocity[0]*delta[1]+velocity[1]*delta[0])/distance
    speed=math.hypot(*velocity[:2])
    movement=('stationary' if speed<40 else 'approaching' if radial>60 and radial>abs(lateral)
              else 'separating' if radial < -60 and -radial>abs(lateral) else 'lateral')
    visibility=('unknown' if not row.get('collision_complete') else
                'clear_center_ray' if row.get('center_ray_clear') else 'world_occluded')
    condition=dict(range='near' if distance<256 else 'medium' if distance<640 else 'far',
        health='critical' if row['health']<=40 else 'normal',
        armor='present' if row['armor']>0 else 'absent',visibility=visibility,
        enemy_height='above' if delta[2]>64 else 'below' if delta[2]<-64 else 'same_level')
    return condition,movement,speed


def profile(combat, groups_path, donor=None, cancelled=None):
    groups_doc=json.loads(Path(groups_path).read_text(encoding='utf8'))
    summary=json.loads((Path(combat).parent/'summary.json').read_text(encoding='utf8'))
    if groups_doc['source_run']!=summary['source_run']:raise ValueError('Style group provenance mismatch')
    groups={(r['recording_id'],r['segment'],r['epoch_id']):r for r in groups_doc['groups']}
    # Cap dimensions by fixed categorical vocabularies; no raw positions or
    # chat text become unbounded keys. Individual records cannot dominate just
    # because a single demo is much longer than the others.
    buckets=defaultdict(lambda:defaultdict(lambda:dict(movement=Counter(),weapons=Counter(),speeds=[])))
    counts=Counter(); aliases=Counter()
    columns=['recording_id','segment','epoch_id','time_ms','aliases','opponent_aliases','health','armor',
        'origin','velocity','weapon','packet_enemy_origin','collision_complete','center_ray_clear','map']
    for batch in pq.ParquetFile(combat).iter_batches(batch_size=4096,columns=columns):
        if cancelled and cancelled():raise InterruptedError('Style analysis cancelled')
        for row in batch.to_pylist():
            group=groups.get((row['recording_id'],row['segment'],row['epoch_id']))
            if not group or group['split']!='train':
                counts['heldout_or_quarantine']+=1;continue
            if row['health'] is None or row['health']<=0 or row['time_ms']%500:continue
            if donor and (not donor_match(row['aliases'],donor) or donor_match(row['opponent_aliases'],donor)):
                counts['not_unambiguous_donor']+=1;continue
            result=classify(row)
            if result is None:counts['enemy_unknown']+=1;continue
            condition,movement,speed=result
            key=(row['map'],)+tuple(condition.values())
            per_record=buckets[key][row['recording_id']]
            per_record['movement'][movement]+=1;per_record['weapons'][row['weapon']]+=1
            if len(per_record['speeds'])<2000:per_record['speeds'].append(speed)
            counts['samples']+=1;aliases.update(row['aliases'])
    conditions=[]
    for key,records in sorted(buckets.items()):
        samples=sum(sum(r['movement'].values()) for r in records.values())
        def probabilities(field):
            labels=set().union(*(r[field] for r in records.values()))
            return {label:sum(r[field][label]/sum(r[field].values()) for r in records.values())/len(records)
                    for label in sorted(labels)}
        conditions.append(dict(map=key[0],condition=dict(zip(('range','health','armor','visibility','enemy_height'),key[1:])),
            samples=samples,recordings=len(records),supported=samples>=30 and len(records)>=3,
            movement=probabilities('movement'),weapon=probabilities('weapons'),
            mean_speed=sum(sum(r['speeds'])/len(r['speeds']) for r in records.values())/len(records)))
    return dict(schema=1,donor=donor,source_run=groups_doc['source_run'],partition='train',
        aliases=dict(aliases),counts=dict(counts),conditions=conditions,
        supported_conditions=sum(r['supported'] for r in conditions),runtime_qualified=False,
        scope='Observed conditional preferences, equal recording weight; not causal actions, skill or a complete player clone',
        unknown=['opponent inventory and stack in ordinary POV demos','live item availability','player intention'])
