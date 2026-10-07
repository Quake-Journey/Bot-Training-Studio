"""Re-evaluate fixed weights; no training, epoch reselection or holdout tuning."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'worker'))
from safetensors.torch import load_file
from opentdm_x_trainer import decisions as ds,decision_learning as learn
from opentdm_x_trainer.data import sha
from opentdm_x_trainer.learning import atomic_json,device_for
from opentdm_x_trainer.models import create


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--dataset',type=Path,required=True)
    ap.add_argument('--store',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--backend',default='cpu');a=ap.parse_args()
    data,meta=ds.load(a.dataset)
    ref=json.loads((a.store/'latest-experiment.json').read_text());generation=ref['generation']
    import re
    if not re.fullmatch(r'generation-[a-f0-9]{32}',generation):raise ValueError('Invalid generation')
    folder=a.store/generation;model_meta=json.loads((folder/'manifest.json').read_text())
    if model_meta['dataset_sha256']!=meta['samples_sha256']:raise ValueError('Dataset does not match fixed model')
    if model_meta['feature_version']!=ds.VERSION:raise ValueError('Incompatible decision features')
    if sha(folder/'weights.safetensors')!=model_meta['files']['weights.safetensors']:raise ValueError('Model integrity failure')
    device,_=device_for(a.backend);model=create(model_meta['profile'],len(ds.INPUTS),learn.OUTPUTS).to(device)
    model.load_state_dict(load_file(str(folder/'weights.safetensors')))
    calibration=data
    if 'replay.npz' in model_meta['files']:
        _,_,calibration=learn.experiment(a.store,generation)
    validation=learn.evaluate(model,data,device,'validation',calibration=calibration)
    test=learn.evaluate(model,data,device,'test',calibration=calibration)
    result=dict(generation=generation,validation=validation,test=test,source_hashes=learn.SOURCE_HASHES,
        probability_correction='training-loss-weights-v1',fixed_weights=True,epoch_reselected=False,
        qualified_observation_heads=learn.qualified(validation,test),retention=model_meta.get('retention'),
        scope='Fixed weights on the requested dataset; saved replay supplies training priors',may_activate_in_game=False)
    a.output.parent.mkdir(parents=True,exist_ok=True);atomic_json(a.output,result)
    print(str(a.output),flush=True)


if __name__=='__main__':main()
