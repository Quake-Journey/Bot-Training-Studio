"""Offline native inverse-input witnesses, kept separate from learned policies."""
import json
import os
from pathlib import Path
from types import SimpleNamespace
import uuid
import ctypes as c
from collections import deque
import numpy as np
import pyarrow.parquet as pq

from .data import sha
from .learning import atomic_json, writer_lock
from .projects import native_home


def fit(project, donor=None, limit=200, event=None, cancelled=None):
    from bts_analysis.fit_movement import main
    if not 1<=limit<=10000:raise ValueError('Movement fitting limit must be 1..10000')
    project=Path(project)
    with writer_lock(project):
        meta=json.loads((project/'project.json').read_text())
        if sha(project/'map.bsp')!=meta['bsp_sha256']:raise ValueError('Cached BSP integrity failure')
        source=project/'revisions'/meta['active_revision']
        out=source/('movement-'+uuid.uuid4().hex)
        args=SimpleNamespace(data=source,bsp=project/'map.bsp',map=meta['map'],
            physics=native_home()/('physics.dll' if os.name=='nt' else 'physics.so'),
            out=out,stride=1,max_samples=limit,teaching_alias=donor)
        result=main(args,event=event or (lambda _:None),cancelled=cancelled)
        result['continuous_sequences']=qualify_sequences(project/'map.bsp',out/'witnesses.parquet',out/'sequences.json',cancelled)
        result.update(path=str(out),runtime_qualified=False,
            scope='possible 100ms controls under static-world physics, not complete tricks')
        atomic_json(out/'summary.json',result)
        return result


def qualify_sequences(bsp,witnesses,output,cancelled=None):
    """Validate an uninterrupted 1.6s witness, never reset state at each frame."""
    from .knowledge import Physics
    verified=[];candidate_count=0;tracks={}
    with Physics(bsp) as physics:
        physics.lib.OTXF_Replay.argtypes=[c.POINTER(c.c_float),c.POINTER(c.c_int),c.POINTER(c.c_int),
            c.POINTER(c.c_float),c.c_int,c.POINTER(c.c_float)]
        physics.lib.BTS_FitSequence.argtypes=[c.POINTER(c.c_float),c.POINTER(c.c_int),c.POINTER(c.c_float),
            c.c_int,c.POINTER(c.c_int),c.POINTER(c.c_float)]
        for batch in pq.ParquetFile(witnesses).iter_batches(batch_size=1024):
            for row in batch.to_pylist():
                if cancelled and cancelled():raise InterruptedError('Movement replay cancelled')
                key=tuple(row[k] for k in ('recording_id','segment','slot','epoch','gravity'))
                if len(tracks)>1024:tracks.clear()
                window=tracks.setdefault(key,deque(maxlen=16))
                if window and row['time_ms']-window[-1]['time_ms']!=100:
                    window.clear()
                window.append(row)
                if len(window)<16 or row['time_ms']%800:continue
                candidate_count+=1;rows=list(window);first=rows[0]
                controls=[r[k] for r in rows for k in ('forward','side','jump_pattern','substep_ms')]
                targets=[v for r in rows for v in r['destination']+r['end_velocity']+r['end_view']]
                start=first['origin']+first['velocity']+first['view']
                state=[first[k] for k in ('flags','pm_time','gravity','pm_type')]
                out=(c.c_float*(len(rows)*8))()
                valid=physics.lib.OTXF_Replay((c.c_float*9)(*start),(c.c_int*4)(*state),
                    (c.c_int*len(controls))(*controls),(c.c_float*len(targets))(*targets),len(rows),out)
                values=np.asarray(out).reshape(-1,8)
                if not valid or not np.isfinite(values).all():continue
                if values[:,0].max()>8 or values[:,1].max()>60:
                    # Refit from simulated state; qualify those controls again
                    # with the independent uninterrupted replay entry point.
                    fitted=(c.c_int*len(controls))()
                    if not physics.lib.BTS_FitSequence((c.c_float*9)(*start),(c.c_int*4)(*state),
                        (c.c_float*len(targets))(*targets),len(rows),fitted,out):continue
                    controls=list(fitted)
                    if not physics.lib.OTXF_Replay((c.c_float*9)(*start),(c.c_int*4)(*state),fitted,
                        (c.c_float*len(targets))(*targets),len(rows),out):continue
                    values=np.asarray(out).reshape(-1,8)
                    if not np.isfinite(values).all() or values[:,0].max()>8 or values[:,1].max()>60:continue
                verified.append(dict(recording_id=first['recording_id'],segment=first['segment'],slot=first['slot'],
                    time_ms=first['time_ms'],aliases=first['aliases'],initial=start,state=state,controls=controls,
                    target_views=[r['end_view'] for r in rows],destination=rows[-1]['destination'],
                    frames=len(rows),max_position_error=float(values[:,0].max()),max_velocity_error=float(values[:,1].max()),
                    jump_events=int(values[:,2].sum()),preconditions='matching BSP, gravity, initial pose/velocity and static mover state'))
    atomic_json(output,dict(schema=1,bsp_sha256=sha(bsp),sequences=verified,candidates=candidate_count,
        uninterrupted_replay=True,original_commands=False,runtime_qualified=False))
    return dict(candidates=candidate_count,verified=len(verified),jump_sequences=sum(r['jump_events']>0 for r in verified),path=str(output))
