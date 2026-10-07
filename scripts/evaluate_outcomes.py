"""Re-evaluate fixed checkpoints and conditional style without retraining."""
import argparse
import json
from pathlib import Path
import sys

import numpy as np
from safetensors.torch import load_file
import torch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from opentdm_x_trainer import behaviour, temporal, sequences
from opentdm_x_trainer.models import create
from opentdm_x_trainer.learning import atomic_json


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--experiment',type=Path,required=True)
    ap.add_argument('--projects',type=Path,nargs='*',default=[])
    args=ap.parse_args();torch.set_num_threads(4);reports={}
    for profile in ('compact','balanced'):
        folder,meta=temporal.active(args.experiment/profile)
        with np.load(folder/'replay.npz',allow_pickle=False) as raw:data={k:raw[k] for k in raw.files}
        model=create(profile,len(sequences.INPUTS),temporal.OUTPUTS)
        model.load_state_dict(load_file(str(folder/'weights.safetensors')))
        validation=temporal.evaluate(model,data,torch.device('cpu'),'validation')
        reports[profile]=dict(generation=folder.name,validation=validation,
            test=temporal.evaluate(model,data,torch.device('cpu'),'test'),
            qualified_validation_heads=temporal.qualify_heads(validation),evaluation_source=temporal.SOURCE_HASHES)
    for project in args.projects:
        meta=json.loads((project/'project.json').read_text());source=project/'revisions'/meta['active_revision']
        report=behaviour.profile(source/'combat.parquet',source/'groups.json')
        atomic_json(args.experiment/(meta['map']+'-conditional-style.json'),report)
        print(meta['map'],report['counts'],report['supported_conditions'],flush=True)
    atomic_json(args.experiment/'evaluation.json',reports)
    for profile,r in reports.items():
        print(profile,r['qualified_validation_heads'],flush=True)


if __name__=='__main__':main()
