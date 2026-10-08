"""Reproducible offline curriculum, using imported projects and bounded stores.

No embedded developer paths, demo copies, game process or network downloads.
Existing sequence stores update with replay and retention gates. Mechanism
experts use a separate schema/store; fresh or completed-epoch resume only.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))

from opentdm_x_trainer.learning import atomic_json
from opentdm_x_trainer import sequences, temporal, mechanics_sequences, mechanics_learning


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--projects',nargs='+',type=Path,required=True)
    parser.add_argument('--teaching-projects',nargs='*',type=Path,default=[])
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--profile',choices=('compact','balanced','large','xl'),default='compact')
    parser.add_argument('--context',type=int,default=16)
    parser.add_argument('--backend',choices=('cpu','cuda','rocm','xpu','directml'),default='cpu')
    parser.add_argument('--epochs',type=int,default=30)
    parser.add_argument('--batch-size',type=int,default=128)
    parser.add_argument('--limit',type=int,default=12000)
    parser.add_argument('--sequence-store',type=Path)
    parser.add_argument('--sequence-mode',choices=('fresh','update','resume'),default='fresh')
    parser.add_argument('--mechanism-mode',choices=('fresh','resume'),default='fresh')
    parser.add_argument('--family',choices=('all','sequences','mechanisms'),default='all')
    parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    emit=lambda row:print(json.dumps(row,allow_nan=False),flush=True)
    sources=[];revisions={}
    for project in args.projects+args.teaching_projects:
        meta=json.loads((project/'project.json').read_text(encoding='utf8'))
        revisions[str(project.resolve())]=meta['active_revision']
        if project in args.projects:
            revision=project/'revisions'/meta['active_revision']
            sources.append(dict(map=meta['map'],combat=revision/'combat.parquet',groups=revision/'groups.json',bsp=project/'map.bsp'))
    contract=dict(project_revisions=revisions,context=args.context,limit=args.limit)
    pin=args.out/'inputs.json'
    if pin.exists() and json.loads(pin.read_text(encoding='utf8'))!=contract:
        raise ValueError('Projects or dataset parameters changed; use a new output generation')
    atomic_json(pin,contract);result=dict(runtime_qualified=False,may_activate_in_game=False)
    for family in (('sequences','mechanisms') if args.family=='all' else (args.family,)):
        dataset=args.out/family
        module=sequences if family=='sequences' else mechanics_sequences
        if dataset.exists():
            _,manifest=module.load(dataset)
        else:
            inputs=sources if family=='sequences' else args.projects+args.teaching_projects
            manifest=module.prepare(inputs,dataset,context=args.context,limit=args.limit,event=emit)
        emit(dict(stage=family+'_prepared',splits=manifest['splits']))
        if args.prepare_only:continue
        options=dict(profile=args.profile,backend=args.backend,epochs=args.epochs,batch_size=args.batch_size,event=emit)
        if family=='sequences':
            result[family]=temporal.train(dataset,args.sequence_store or args.out/'models/sequences',mode=args.sequence_mode,**options)
        else:
            result[family]=mechanics_learning.train(dataset,args.out/'models/mechanisms',mode=args.mechanism_mode,**options)
        atomic_json(args.out/'curriculum.json',result)
    emit(dict(stage='completed',result=result))


if __name__=='__main__':main()
