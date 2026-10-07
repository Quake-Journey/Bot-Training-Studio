"""Check incremental observations against a full recomputation on real recordings."""
import argparse
import json
from pathlib import Path
import sys
import time
import re
import shutil
import os

import pyarrow.parquet as pq

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer.projects import import_project,native_home
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--project',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    if a.output.exists():raise ValueError('Use a new cache experiment directory')
    original=json.loads((a.project/'project.json').read_text())
    if not re.fullmatch(r'[A-Za-z0-9_-]+',original['map']):raise ValueError('Invalid logical map name')
    a.output.mkdir(parents=True)
    bsp=a.output/(original['map']+'.bsp');shutil.copyfile(a.project/'map.bsp',bsp)
    observations=a.project/'revisions'/original['active_revision']/'combat.parquet'
    eligible=set(pq.read_table(observations,columns=['recording_id'])['recording_id'].unique().to_pylist())
    sources=[r['source'] for r in sorted(original['recordings'],key=lambda r:r['source']['bytes'])
             if r['recording_id'] in eligible][:2]
    if len(sources)!=2:raise ValueError('Two accepted recordings are required')
    reports=[];project=a.output/'incremental'
    def run(target,selected):
        maximum=[0];start=time.monotonic()
        def event(row):
            maximum[0]=max(maximum[0],row.get('reused',0))
        result=import_project(bsp,[],target,selected=selected,event=event)
        result.update(seconds=time.monotonic()-start,reused=maximum[0]);reports.append(result)
        print(json.dumps(result),flush=True)
        return result
    run(project,sources[:1]);second=run(project,sources[1:]);third=run(project,[])
    reference=run(a.output/'fresh',sources)
    assert second['reused']==1 and third['reused']==2 and reference['reused']==0
    assert third['contexts']>0 and reference['contexts']>0
    paths=[project/'revisions'/third['revision'],a.output/'fresh'/'revisions'/reference['revision']]
    for name in ('combat.parquet','shots.parquet'):
        tables=[pq.read_table(p/name) for p in paths]
        assert tables[0].num_rows>0,name
        order=[(k,'ascending') for k in ('recording_id','segment','seq','slot')]
        assert tables[0].sort_by(order).equals(tables[1].sort_by(order)),name
    meta=[json.loads((p/'project.json').read_text()) for p in paths]
    assert meta[0]['counts']==meta[1]['counts']
    # Mutate only an isolated copy of the tool, not the installed decoder.
    tools=a.output/'tool-change';tools.mkdir()
    binary='decoder.exe' if os.name=='nt' else 'decoder'
    physics='physics.dll' if os.name=='nt' else 'physics.so'
    for name in (binary,physics):shutil.copy2(native_home()/name,tools/name)
    prior_env=os.environ.get('BTS_NATIVE');os.environ['BTS_NATIVE']=str(tools.resolve())
    before=(project/'project.json').read_bytes()
    def change_tool(row):
        if row.get('stage')=='committing':
            with (tools/binary).open('ab') as f:f.write(b'changed-after-analysis')
    try:
        try:import_project(bsp,[],project,event=change_tool)
        except ValueError as error:assert 'changed during import' in str(error)
        else:raise AssertionError('Changed tool was silently certified')
    finally:
        if prior_env is None:os.environ.pop('BTS_NATIVE',None)
        else:os.environ['BTS_NATIVE']=prior_env
    assert before==(project/'project.json').read_bytes()
    atomic_json(a.output/'summary.json',dict(passed=True,full_recompute_equal=True,tool_change_preserves_revision=True,runs=reports))


if __name__=='__main__':main()
