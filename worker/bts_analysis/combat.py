"""Stream recorded duel scenes into inspectable combat/shot observations.

No demo inventory, original-input reconstruction, online model or training claim.
All scene/state/event joins use decoder sequence, never a nearest future row.
"""
from __future__ import annotations
import argparse,ctypes as c,hashlib,heapq,json,math,re,struct,time
from collections import Counter,defaultdict
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
from .analyzer import TableWriter,atomic_json,io_path
from .motion import angle,quantiles

WEAPONS=dict(blast='blaster',shotg='shotgun',shotg2='supershotgun',machn='machinegun',
    chain='chaingun',handgr='handgrenade',launch='grenadelauncher',rocket='rocketlauncher',
    hyperb='hyperblaster',rail='railgun',bfg='bfg')
FLASH={0:'blaster',1:'machinegun',2:'shotgun',3:'chaingun',4:'chaingun',5:'chaingun',
       6:'railgun',7:'rocketlauncher',8:'grenadelauncher',12:'bfg',13:'supershotgun',14:'hyperblaster'}
VEC=pa.list_(pa.float32())
SCHEMA=pa.schema([
    ('recording_id',pa.string()),('segment',pa.int32()),('seq',pa.int64()),('time_ms',pa.int64()),
    ('epoch_id',pa.int32()),('slot',pa.int32()),('enemy_slot',pa.int32()),
    ('aliases',pa.list_(pa.string())),('opponent_aliases',pa.list_(pa.string())),
    ('map',pa.string()),('mvd',pa.bool_()),('weapon',pa.string()),('grounded',pa.bool_()),
    ('origin',VEC),('velocity',VEC),('view',VEC),('eye',VEC),('fov',pa.float32()),
    ('speed_xy',pa.float32()),('health',pa.int32()),('armor',pa.int32()),
    ('packet_enemy_origin',VEC),('packet_enemy_velocity',VEC),('range',pa.float32()),
    ('yaw_error',pa.float32()),('pitch_error',pa.float32()),('camera_yaw_error',pa.float32()),
    ('camera_pitch_error',pa.float32()),('center_ray_clear',pa.bool_()),('shot_ray_clear',pa.bool_()),
    ('horizontal_fov',pa.bool_()),('collision_complete',pa.bool_()),('eye_recorded',pa.bool_()),
    ('context',pa.string()),('continuous',pa.bool_()),('yaw_rate',pa.float32()),
    ('pitch_rate',pa.float32()),('lateral_speed',pa.float32()),('radial_speed',pa.float32()),
    ('packet_rockets',pa.int32()),('packet_grenades',pa.int32()),
    ('original_usercmd_available',pa.bool_()),('training_ready',pa.bool_())])
SHOT_SCHEMA=pa.schema([
    ('recording_id',pa.string()),('segment',pa.int32()),('seq',pa.int64()),('time_ms',pa.int64()),
    ('slot',pa.int32()),('weapon',pa.string()),('silenced',pa.bool_()),('mvd',pa.bool_()),
    ('scope',pa.int32()),('context_seq',pa.int64()),('context_time_ms',pa.int64()),
    ('context_age_ms',pa.int32()),('context',pa.string()),('range',pa.float32()),
    ('yaw_error',pa.float32()),('pitch_error',pa.float32()),('training_ready',pa.bool_())])

def finite(v):return v is not None and len(v)==3 and all(x is not None and math.isfinite(x) for x in v)

class Collision:
    def __init__(self,library,bsp):
        self.lib=c.CDLL(str(library.resolve()));self.lib.OTXF_Open.argtypes=[c.c_void_p,c.c_size_t]
        self.lib.OTXF_SceneRays.argtypes=[c.POINTER(c.c_float),c.c_int,c.POINTER(c.c_int),c.POINTER(c.c_float),c.c_int,c.c_int,c.POINTER(c.c_float)]
        raw=bsp.read_bytes();self.sha=hashlib.sha256(raw).hexdigest()
        if not self.lib.OTXF_Open(raw,len(raw)):raise ValueError('Cannot bind requested BSP')
        self.models=self.lib.OTXF_ModelCount()
        self.lib.OTXF_ModelEnvelope.argtypes=[c.c_int,c.POINTER(c.c_float)]
        self.envelopes={}
        for n in range(1,self.models):
            out=(c.c_float*6)()
            if self.lib.OTXF_ModelEnvelope(n,out):self.envelopes[n]=list(out)
        # Task step 0 (ztn2dm3): the shared adapter bounds only func_plat. Two
        # more classes are qualified here, in the review adapter, from the
        # game's own spawn rules - no shared MAPGEN primitive changes:
        # * func_wall never moves (SP_func_wall: MOVETYPE_PUSH, no mover
        #   think); a toggle only changes presence, never position. Its model
        #   bounds are therefore its entire possible volume.
        # * trigger_* is SOLID_TRIGGER: never in MASK_OPAQUE or MASK_SHOT, so
        #   it cannot obstruct a sight or shot ray.
        # Every other unrecorded class stays unknown.
        self.transparent=set();self.static_walls=set()
        for n,classname,bounds in bsp_models(raw):
            if classname.startswith('trigger_'):self.transparent.add(n)
            elif classname=='func_wall' and n not in self.envelopes:
                self.envelopes[n]=[bounds[0]-1,bounds[1]-1,bounds[2]-1,bounds[3]+1,bounds[4]+1,bounds[5]+1]
                self.static_walls.add(n)
    def rays(self,eye,target,models,origins):
        ray=(c.c_float*6)(*eye,*target);out=(c.c_float*3)()
        result=[]
        for mask in (1|8|16,1|2): # actual MASK_OPAQUE and world part of MASK_SHOT
            if not self.lib.OTXF_SceneRays(ray,1,(c.c_int*len(models))(*models),
                (c.c_float*len(origins))(*origins),len(models),mask,out):raise ValueError('Invalid scene collision input')
            result.append(out[0]==1 and not out[1] and not out[2])
        return result
    def close(self):self.lib.OTXF_Close()

def bsp_models(raw):
    """(model number, classname, model bounds) for every entity with an
    inline model, read from the exact BSP's entity and model lumps."""
    eo,el=struct.unpack_from('<ii',raw,8);mo,ml=struct.unpack_from('<ii',raw,8+13*8)
    out=[]
    for text in re.findall(r'\{[^}]*\}',raw[eo:eo+el].decode('latin1')):
        fields=dict(re.findall(r'"([^"\n]*)"\s+"([^"\n]*)"',text))
        model=fields.get('model','')
        if not model.startswith('*') or not model[1:].isdigit():continue
        n=int(model[1:])
        if not 0<n<ml//48:continue
        out.append((n,fields.get('classname',''),list(struct.unpack_from('<6f',raw,mo+n*48))))
    return out

def intersects(start,end,bounds):
    """Conservative closed segment/slab intersection, including inside starts."""
    low,high=0.,1.
    for k in range(3):
        d=end[k]-start[k]
        if abs(d)<1e-9:
            if start[k]<bounds[k] or start[k]>bounds[k+3]:return False
        else:
            a=(bounds[k]-start[k])/d;b=(bounds[k+3]-start[k])/d
            low=max(low,min(a,b));high=min(high,max(a,b))
            if low>high:return False
    return True

def rows(path,kind=None):
    for batch in pq.ParquetFile(path).iter_batches(batch_size=16 if kind=='scene' else 2048):
        for row in batch.to_pylist():
            if kind:row['kind']=kind
            else:
                if row['kind'] not in {'segment_start','configstring','frame','muzzleflash'}:continue
                row=json.loads(row['payload'])
            yield row

def weapon(path):
    for token,name in WEAPONS.items():
        if path==f'models/weapons/v_{token}/tris.md2':return name
    return 'unknown'

def scene_models(scene,configs,base,model_count,eye=None,target=None,envelopes=None,transparent=()):
    models=[];origins=[];rotated=False
    for e in scene['entities']:
        if e['solid']!=31:continue
        name=configs.get(base+e['models'][0],'')
        if not name.startswith('*') or not name[1:].isdigit():rotated=True;continue
        n=int(name[1:])
        if not 0<n<model_count:rotated=True;continue
        if any(abs(angle(a,0))>.001 for a in e['angles']):rotated=True;continue
        models.append(n);origins.extend(e['origin'])
    # A missing lift cannot obstruct a ray outside its ENTIRE possible swept
    # volume. This certifies irrelevance without guessing its current position.
    # Unknown model classes and rays through an unseen lift remain unresolved.
    missing=set(range(1,model_count))-set(models)-set(transparent)
    complete=not rotated and all(eye is not None and target is not None and n in (envelopes or {})
        and not intersects(eye,target,envelopes[n]) for n in missing)
    return models,origins,complete

def features(s,enemy,old,old_enemy,scene,collision,configs,base):
    pos=s['origin'];vel=s['velocity'];view=s['view_angles'];dt=s['time_ms']-old['time_ms'] if old else 0
    continuous=bool(old and dt==100 and old['pm_type']==0 and not old['stats'][16] and not old['stats'][17]
        and old['stats'][1]>0 and not s['pm_flags']&32 and math.dist(pos,old['origin'])<=150)
    eye_ok=finite(s.get('view_offset')) and s.get('fov') is not None and 1<=s['fov']<180
    eye=[a+b for a,b in zip(pos,s['view_offset'])] if eye_ok else None
    row=dict(origin=pos,velocity=vel,view=view,eye=eye,fov=s.get('fov'),speed_xy=math.hypot(*vel[:2]),
        grounded=bool(s['pm_flags']&4),health=s['stats'][1],armor=s['stats'][5],
        weapon=weapon(configs.get(base+s['gunindex'],'')),continuous=continuous,
        yaw_rate=angle(view[1],old['view_angles'][1])*10 if continuous else None,
        pitch_rate=angle(view[0],old['view_angles'][0])*10 if continuous else None,
        eye_recorded=eye_ok,context='enemy_absent',original_usercmd_available=False,training_ready=False,
        packet_rockets=0,packet_grenades=0,collision_complete=False)
    for e in scene['entities']:
        path=configs.get(base+e['models'][0],'')
        row['packet_rockets']+=path=='models/objects/rocket/tris.md2'
        row['packet_grenades']+=path in {'models/objects/grenade/tris.md2','models/objects/grenade2/tris.md2'}
    if not enemy or not finite(enemy['origin']) or enemy['models'][0]!=255 or not enemy['solid']:return row
    target=enemy['origin'];relative=[b-a for a,b in zip(pos,target)];d=math.dist(pos,target)
    row.update(packet_enemy_origin=target,range=d)
    if old_enemy and continuous and old_enemy['models']==enemy['models'] and old_enemy['solid'] and math.dist(old_enemy['origin'],target)<=150:
        row['packet_enemy_velocity']=[(b-a)*10 for a,b in zip(old_enemy['origin'],target)]
    yaw=math.degrees(math.atan2(relative[1],relative[0]));bearing=math.radians(yaw)
    row.update(lateral_speed=-vel[0]*math.sin(bearing)+vel[1]*math.cos(bearing),
               radial_speed=vel[0]*math.cos(bearing)+vel[1]*math.sin(bearing))
    if not eye_ok:row['context']='camera_unknown';return row
    diff=[b-a for a,b in zip(eye,target)]
    yaw=math.degrees(math.atan2(diff[1],diff[0]));pitch=-math.degrees(math.atan2(diff[2],math.hypot(*diff[:2])))
    kick=s.get('kick_angles') if finite(s.get('kick_angles')) else [0,0,0]
    row.update(yaw_error=angle(view[1],yaw),pitch_error=angle(view[0],pitch),
        camera_yaw_error=angle(view[1]+kick[1],yaw),camera_pitch_error=angle(view[0]+kick[0],pitch))
    row['horizontal_fov']=abs(row['camera_yaw_error'])<=s['fov']*.5
    models,origins,complete=scene_models(scene,configs,base,collision.models,eye,target,getattr(collision,'envelopes',{}),
                                         getattr(collision,'transparent',()))
    row['collision_complete']=complete
    row['center_ray_clear'],row['shot_ray_clear']=collision.rays(eye,target,models,origins)
    row['context']=('world_occluded' if not row['center_ray_clear'] else 'horizontal_offscreen' if not row['horizontal_fov']
        else 'mover_unknown' if not complete else 'mvd_visibility_unknown' if scene['mvd'] else 'clear_center_candidate')
    return row

def prepare_record(directory,record_id,mapname,collision,contexts,shots,counts,samples):
    facts=json.loads((directory/'facts.json').read_text(encoding='utf-8'))
    segs={f['segment']:f for f in facts if f['kind']=='segment_facts'}
    tracks={f['track_id']:f for f in facts if f['kind']=='track'}
    parts=defaultdict(list);epochs=defaultdict(list)
    for f in facts:
        # DM2 has playerstate activity only for its POV. Opponent participation
        # comes from actual in-epoch combat facts, not the absent POV bit.
        if f['kind']=='participant' and (f['active'] or f.get('kills',0)>0 or f.get('deaths',0)>0):
            parts[f['epoch_id']].append(f)
    for f in facts:
        if f['kind']=='epoch' and f['start_conf']==2 and len(parts[f['epoch_id']])==2:epochs[f['segment']].append(f)
    streams=[rows(directory/'player_states.parquet','state'),rows(directory/'entity_frames.parquet','scene'),rows(directory/'events.parquet')]
    states={};scene=None;configs={};base=32;previous={};previous_scene=None;latest={};segment=None
    for row in heapq.merge(*streams,key=lambda r:r['seq']):
        kind=row['kind'];seg=row['segment'];t=row['time_ms']
        if seg!=segment:
            states={};scene=None;configs={};base=32;previous={};previous_scene=None;latest={};segment=seg
        if kind=='segment_start':base=62 if row['extended'] else 32
        elif kind=='configstring':configs[row['index']]=row['text']
        elif kind=='scene':scene=row
        elif kind=='state':states[row['slot']]=row
        elif kind=='muzzleflash':
            w=FLASH.get(row['weapon']&127)
            if row['monster'] or not w:continue
            prior=latest.get(row['entity']-1)
            age=t-prior['time_ms'] if prior else None
            if not prior or age not in (0,100):counts['unjoined_shots']+=1;continue
            shots.add(dict(recording_id=record_id,segment=seg,seq=row['seq'],time_ms=t,slot=row['entity']-1,
                weapon=w,silenced=bool(row['weapon']&128),mvd=bool(row['mvd']),scope=row['scope'],
                context_seq=prior['seq'],context_time_ms=prior['time_ms'],context_age_ms=age,
                context=prior['context'],range=prior.get('range'),yaw_error=prior.get('yaw_error'),
                pitch_error=prior.get('pitch_error'),training_ready=False))
            counts['shots']+=1;counts['shot/'+w]+=1
        elif kind=='frame':
            if segs.get(seg,{}).get('map')!=mapname or not scene or scene['time_ms']!=t:continue
            live=[e for e in epochs[seg] if e['time_ms']<=t<e['end_ms']]
            if len(live)!=1:latest={};previous={};previous_scene=None;continue
            epoch=live[0];players=[tracks[p['track_id']] for p in parts[epoch['epoch_id']] if p['track_id'] in tracks]
            if len(players)!=2 or players[0]['slot']==players[1]['slot']:continue
            entities={e['number']:e for e in scene['entities']}
            prior_entities={e['number']:e for e in previous_scene['entities']} if previous_scene and t-previous_scene['time_ms']==100 else {}
            latest={}
            for actor,opponent in (players,players[::-1]):
                slot=actor['slot'];s=states.get(slot)
                if not s or s['time_ms']!=t or s['pm_type']!=0 or len(s['stats'])<=17 or s['stats'][1]<=0 or s['stats'][16] or s['stats'][17]:continue
                if not actor['active_seen'] or actor['chase_spectator'] or not actor['time_ms']<=t<=actor['end_ms'] or (actor['identity_end_ms'] and t>=actor['identity_end_ms']):continue
                if not all(finite(s[k]) for k in ('origin','velocity','view_angles')):continue
                result=features(s,entities.get(opponent['slot']+1),previous.get(slot),prior_entities.get(opponent['slot']+1),scene,collision,configs,base)
                result.update(recording_id=record_id,segment=seg,seq=s['seq'],time_ms=t,epoch_id=epoch['epoch_id'],slot=slot,
                    enemy_slot=opponent['slot'],aliases=actor['aliases'],opponent_aliases=opponent['aliases'],map=mapname,mvd=scene['mvd'])
                contexts.add(result);latest[slot]=result;counts['contexts']+=1;counts['context/'+result['context']]+=1
                if result['continuous']:
                    samples[result['context']+'/yaw_rate'].append(abs(result['yaw_rate']))
                    if result.get('lateral_speed') is not None:samples[result['context']+'/lateral_speed'].append(abs(result['lateral_speed']))
            previous=dict(states);previous_scene=scene
    counts['recordings']+=1

def main(a):
    began=time.monotonic();a.data=io_path(a.data);a.out=io_path(a.out)
    sources={name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
             for name in ('combat.py','analyzer.py','motion.py')}
    if a.out.exists():raise ValueError('Use a new evidence output directory')
    snapshot=json.loads((a.data/'current.json').read_text(encoding='utf-8'))
    collision=Collision(a.physics,a.bsp);a.out.mkdir(parents=True)
    contexts=TableWriter(a.out/'combat.parquet.new',SCHEMA);shots=TableWriter(a.out/'shots.parquet.new',SHOT_SCHEMA)
    counts=Counter();samples=defaultdict(list)
    try:
        for record in snapshot['records']:
            if record['status']!='decoded':continue
            prepare_record(a.data/record['path'],record['recording_id'],a.map,collision,contexts,shots,counts,samples)
    finally:contexts.close();shots.close();collision.close()
    for name in ('combat','shots'):(a.out/(name+'.parquet.new')).replace(a.out/(name+'.parquet'))
    report=dict(schema=1,source_run=snapshot['run_id'],bsp_sha256=collision.sha,model_count=collision.models,
        physics_sha256=hashlib.sha256(a.physics.read_bytes()).hexdigest(),transform_sha256=sources['combat.py'],sources=sources,
        counts=dict(counts),quantiles={k:quantiles(v) for k,v in samples.items()},seconds=round(time.monotonic()-began,3),
        training_ready=False,limits=[
            'Packet presence is not visibility; MVD participant PVS and dynamic area state are unresolved.',
            'Center ray plus horizontal FOV is a candidate context, not complete rendered visibility; viewport aspect is unrecorded.',
            'Unknown inline or rotated movers keep clear-ray contexts unqualified; absent ordinary lifts are excluded only outside their full swept bounds.',
            'Shots join only preceding observations with explicit 0/100ms age; no exact subframe aim or hit-rate claim.',
            'Body-center angular error is not hitbox error; projectile lead/splash targets require separate action labels.',
            'Named identity, cross-POV match grouping and held-out splits remain required before training.'])
    atomic_json(a.out/'summary.json',report)
    print(json.dumps({k:report[k] for k in ('source_run','model_count','counts','seconds')}))

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--data',required=True,type=Path);ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--physics',required=True,type=Path);ap.add_argument('--bsp',required=True,type=Path);ap.add_argument('--map',required=True)
    main(ap.parse_args())
