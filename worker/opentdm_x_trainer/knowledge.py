"""Offline map evidence and bounded static-physics route/shot candidates."""
from collections import Counter, defaultdict
import ctypes as c
import json
import math
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

from .data import donor_match, sha
from .learning import atomic_json, writer_lock
from .maps import inspect
from .projects import native_home


class Physics:
    def __init__(self, bsp):
        from bts_analysis.combat import Collision
        self.collision=Collision(native_home()/("physics.dll" if __import__("os").name=="nt" else "physics.so"),Path(bsp))
        self.lib=self.collision.lib
        self.lib.BTS_Trace.argtypes=[c.POINTER(c.c_float),c.POINTER(c.c_float),c.c_int,c.POINTER(c.c_float)]
        self.lib.OTXF_NavSettle.argtypes=[c.POINTER(c.c_float),c.c_int,c.POINTER(c.c_float)]
        self.lib.OTXF_NavLinks.argtypes=[c.POINTER(c.c_float),c.c_int,c.POINTER(c.c_float)]
    def __enter__(self):return self
    def __exit__(self,*args):self.collision.close()
    def trace(self,start,end):
        out=(c.c_float*8)()
        if not self.lib.BTS_Trace((c.c_float*3)(*start),(c.c_float*3)(*end),3,out):raise ValueError("Invalid trace")
        return dict(fraction=out[0],solid=bool(out[1]),point=list(out[2:5]),normal=list(out[5:8]))
    def settle(self,points):
        if not points:return []
        if len(points)>32768:raise ValueError("Node limit")
        values=np.asarray(points,dtype=np.float32).reshape(-1)
        out=(c.c_float*(len(points)*4))()
        if not self.lib.OTXF_NavSettle(values.ctypes.data_as(c.POINTER(c.c_float)),len(points),out):raise ValueError("Settle rejected")
        return np.asarray(out).reshape(-1,4).tolist()
    def links(self,pairs):
        if not pairs:return []
        if len(pairs)>1024:raise ValueError("Link batch limit")
        values=np.asarray(pairs,dtype=np.float32).reshape(-1)
        out=(c.c_float*(len(pairs)*2))()
        if not self.lib.OTXF_NavLinks(values.ctypes.data_as(c.POINTER(c.c_float)),len(pairs),out):raise ValueError("Links rejected")
        return np.asarray(out).reshape(-1,2).tolist()
    def projectile(self,origin,velocity,grenade=False,seconds=2.5):
        p=np.asarray(origin,dtype=float);v=np.asarray(velocity,dtype=float);bounces=[]
        if not np.isfinite(p).all() or not np.isfinite(v).all() or not 0<seconds<=5:raise ValueError("Invalid trajectory")
        for step in range(math.ceil(seconds/.01)):
            if grenade:v[2]-=800*.01
            nextpoint=p+v*.01;hit=self.trace(p,nextpoint)
            if hit["solid"]:return dict(valid=False,reason="starts_in_solid")
            if hit["fraction"]<1:
                if not grenade:return dict(valid=True,impact=hit["point"],seconds=(step+hit["fraction"])*.01,scope="static_world_only")
                normal=np.asarray(hit["normal"]);v-=1.5*np.dot(v,normal)*normal
                p=np.asarray(hit["point"])+normal*.05;bounces.append(hit["point"])
                if normal[2]>.7 and abs(v[2])<60:v[:]=0
            else:p=nextpoint
        return dict(valid=True,impact=p.tolist(),seconds=seconds,bounces=bounces,scope="static_world_only")


def analyze(project, donor=None, event=None, cancelled=None):
    project=Path(project);meta=json.loads((project/"project.json").read_text())
    source=project/"revisions"/meta["active_revision"]
    world=inspect(project/"map.bsp", meta["map"])
    if world["bsp_sha256"]!=meta["bsp_sha256"]:raise ValueError("Project map changed")
    from .transitions import classify
    cells=defaultdict(list); edges=Counter();last={};weapons=Counter();speeds=[];chat=Counter();participants=Counter();tricks=Counter()
    mechanism_counts=Counter();mechanism_witnesses=[]
    for ri,record in enumerate(meta["recordings"]):
        if cancelled and cancelled():raise InterruptedError("Map analysis cancelled")
        if event:event(dict(stage="map_evidence",recording=ri+1,total=len(meta["recordings"])))
        folder=Path(record["path"])
        decoded=json.loads((folder/"manifest.json").read_text())
        for name, check in decoded.get("artifacts", {}).items():
            if Path(name).name != name or sha(folder/name) != check["sha256"]:
                raise ValueError("Decoded evidence integrity failure")
        facts=json.loads((folder/"facts.json").read_text())
        tracks=[r for r in facts if r["kind"]=="track" and not r.get("chase_spectator")
                and (r.get("active_seen") or record.get("role")=="tricks")
                and (not donor or donor_match(r["aliases"],donor))]
        for track in tracks:participants.update(track["aliases"])
        def current_tracks(row):
            return [track for track in tracks if track["segment"]==row["segment"]
                    and track["time_ms"]<=row["time_ms"]<=track["end_ms"]
                    and (not track.get("identity_end_ms") or row["time_ms"]<track["identity_end_ms"])]
        for batch in pq.ParquetFile(folder/"player_states.parquet").iter_batches(batch_size=4096):
            if cancelled and cancelled():raise InterruptedError("Map analysis cancelled")
            for r in batch.to_pylist():
                stats=r["stats"]
                eligible=[track for track in current_tracks(r) if track["slot"]==r["slot"]]
                if len(eligible)!=1 or r["pm_type"]!=0 or len(stats)<18 or stats[1]<=0 or stats[16] or stats[17]:continue
                pos=r["origin"]
                if not all(math.isfinite(v) for v in pos):continue
                key=tuple(math.floor(v/64) for v in pos)
                if len(cells)<8192 or key in cells:
                    if len(cells[key])<8:cells[key].append(pos)
                track=(record["recording_id"],r["segment"],eligible[0]["track_id"])
                prev=last.get(track)
                edge=classify(prev,r,world['transitions'])
                if edge['kind'] in ('teleport','push'):
                    mechanism_counts[edge['kind']]+=1
                    if len(mechanism_witnesses)<4096:
                        mechanism_witnesses.append(dict(recording_id=record['recording_id'],segment=r['segment'],
                            slot=r['slot'],time_ms=r['time_ms'],mechanism=edge,
                            origin=prev['origin'],destination=r['origin'],velocity=r['velocity'],
                            runtime_qualified=False,scope='observed mechanism; not native action replay'))
                if prev and r["time_ms"]-prev["time_ms"]==100 and math.dist(pos,prev["origin"])<150 and key!=prev["cell"]:
                    if key in cells and prev["cell"] in cells:edges[prev["cell"],key]+=1
                last[track]=dict(r,cell=key)
                speed=math.hypot(*r["velocity"][:2])
                if len(speeds)<100000 and math.isfinite(speed):speeds.append(speed)
        for batch in pq.ParquetFile(folder/"events.parquet").iter_batches(batch_size=2048):
            for entry in batch.to_pylist():
                if entry["kind"]=="movement_event" and not donor:
                    value=json.loads(entry["payload"]);tricks[value.get("event","unknown")]+=1
                if entry["kind"]!="print":continue
                value=json.loads(entry["payload"])
                if value.get("level")!=3:continue
                text=value.get("text","").strip()
                # Only an exact known alias prefix, not arbitrary nickname text
                # inside someone else's message, can attribute a phrase.
                matches=[name for track in current_tracks(entry) for name in track["aliases"] if text.startswith(name+": ")]
                if len(set(matches))!=1:continue
                name=matches[0];message=text[len(name)+2:].strip()
                if 0<len(message)<=160 and not any(ord(ch)<32 for ch in message):chat[name,message]+=1
    # Styles use only attributed live combat observations when available.
    for batch in pq.ParquetFile(source/"combat.parquet").iter_batches(batch_size=4096,columns=["aliases","opponent_aliases","weapon","health"]):
        for row in batch.to_pylist():
            if row['health'] is not None and row['health']>0 and (not donor or
                (donor_match(row['aliases'],donor) and not donor_match(row['opponent_aliases'],donor))):weapons[row["weapon"]]+=1
    from .behaviour import profile as behaviour_profile
    conditional=behaviour_profile(source/'combat.parquet',source/'groups.json',donor,cancelled)
    with Physics(project/"map.bsp") as physics:
        ordered=sorted(cells)
        points=[np.mean(cells[key],axis=0).tolist() for key in ordered]
        settled=physics.settle(points)
        nodes=[];ids={}
        for key,point in zip(ordered,settled):
            if point[3]:ids[key]=len(nodes);nodes.append(dict(id=len(nodes),origin=point[:3]))
        candidates=[(ids[a],ids[b],count) for (a,b),count in edges.items() if a in ids and b in ids]
        links=[];unverified=0
        for offset in range(0,len(candidates),256):
            if cancelled and cancelled():raise InterruptedError("Route validation cancelled")
            batch=candidates[offset:offset+256]
            results=physics.links([nodes[a]["origin"]+nodes[b]["origin"] for a,b,_ in batch])
            for (a,b,count),result in zip(batch,results):
                if result[0]:links.append(dict(source=a,target=b,observations=count,seconds=result[0],jump=bool(result[1]),validation="ordinary_pmove_static_world"))
                else:unverified+=1
        controls=[]
        keyitems=[item for item in world["items"] if item["category"] in ("armor","weapon","health")]
        # Prove a static corridor to an item, not an unobserved opponent or a
        # guarantee that it is a profitable shot in a live match.
        for item in keyitems:
            targets=sorted(nodes,key=lambda n:math.dist(n["origin"],item["origin"]))[2:18]
            for node in targets:
                eye=[*node["origin"]];eye[2]+=22
                distance=math.dist(eye,item["origin"])
                if not 128<distance<1400:continue
                ray=physics.trace(eye,item["origin"])
                if ray["fraction"]==1 and not ray["solid"]:
                    controls.append(dict(node=node["id"],item=item["id"],aim=item["origin"],line_clear=True,
                        requires="live opponent likelihood, actual item timing, muzzle and dynamic collision check"))
    report=dict(schema=1,map=world["map"],bsp_sha256=world["bsp_sha256"],source_revision=meta["active_revision"],
        nodes=nodes,links=links,unverified_observed_links=unverified,items=world["items"],control_candidates=controls,
        mechanisms=world['transitions'],observed_mechanisms=dict(counts=dict(mechanism_counts),
            witnesses=mechanism_witnesses,capacity=4096,truncated=sum(mechanism_counts.values())>len(mechanism_witnesses)),
        style=dict(donor=donor,aliases=dict(participants),weapon_observations=dict(weapons),
                   conditional_preferences=conditional,
                   speed_quantiles={str(q):float(np.quantile(speeds,q)) for q in (.25,.5,.9,.99)} if speeds else {},
                   movement_events=dict(tricks),movement_events_scope="recording aggregate; omitted for donor learning",
                   phrases=[dict(alias=a,text=t,count=n) for (a,t),n in chat.most_common(200)]),
        runtime_qualified=False,limits=["Static world and initial mover stops; observed edges not passing Pmove are not exported as traversable.",
            "Mechanism witnesses preserve teleport/push evidence but are not exported as native-qualified traversal links.",
            "Rocket-jump knockback, moving-platform timing and complete mechanism actions require separate qualification.",
            "Item visibility candidates are not a policy; no invented opponent, availability or hit probability."])
    name="knowledge-"+(__import__("uuid").uuid4().hex)+".json"
    atomic_json(source/name,report)
    return dict(path=str(source/name),map=world["map"],nodes=len(nodes),validated_links=len(links),
                control_candidates=len(controls),observed_mechanisms=dict(mechanism_counts),
                phrases=len(report["style"]["phrases"]),runtime_qualified=False)
