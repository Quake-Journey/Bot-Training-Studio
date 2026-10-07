"""Factory/user isolation, real gradient updates, retention lineage and failures."""
import json
from pathlib import Path
import tempfile
import unittest

import torch
from safetensors.torch import load_file,save_file

from . import model_layers as layers,temporal
from .data import sha
from .learning import atomic_json
from .models import create
from .sequences import INPUTS,VERSION
from .test_pipeline import sequence_fixture,bsp_fixture


def fixture(root):
    root.mkdir()
    key='compact-fixture';folder=root/key;folder.mkdir()
    torch.manual_seed(23)
    model=create('compact',len(INPUTS),temporal.OUTPUTS)
    save_file(model.state_dict(),str(folder/'weights.safetensors'))
    atomic_json(folder/'model.json',dict(id=key,profile='compact',context=4,donor=None,
        maps=['fixture'],feature_version=VERSION,may_activate_in_game=False))
    atomic_json(root/'catalog.json',dict(schema=layers.SCHEMA,game_installable=False,models=[dict(
        id=key,profile='compact',metadata_sha256=sha(folder/'model.json'),weights_sha256=sha(folder/'weights.safetensors'))]))
    return root


class LayerTests(unittest.TestCase):
    def test_learning_keeps_factory_and_exports_real_user_delta_after_update(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);factory=fixture(root/'Models');dataset=sequence_fixture(root/'data');store=root/'user'
            original={p.relative_to(factory):sha(p) for p in factory.rglob('*') if p.is_file()}
            result=temporal.train(dataset,store,mode='factory',factory_catalog=factory,epochs=3,batch_size=8)
            folder=store/result['generation'];meta=json.loads((folder/'manifest.json').read_text())
            weights=load_file(str(folder/'weights.safetensors'))
            effective=layers.load_effective(store,folder,meta)
            self.assertTrue(all(torch.allclose(weights[k],effective[k],atol=1e-7,rtol=1e-6) for k in weights))
            delta=load_file(str(folder/'user-delta.safetensors'))
            self.assertTrue(any(torch.count_nonzero(v).item() for v in delta.values()))
            self.assertTrue(result['accepted']);self.assertFalse(meta['factory_retention_qualified'])
            from . import packages
            project=root/'project';project.mkdir();bsp=bsp_fixture(project/'map.bsp')
            atomic_json(project/'project.json',dict(map='fixture',bsp_sha256=sha(bsp)))
            knowledge=root/'knowledge.json'
            atomic_json(knowledge,dict(bsp_sha256=sha(bsp),style=dict(donor=None,phrases=[]),
                nodes=[dict(id=0,origin=[0,0,0])],links=[],items=[],control_candidates=[],limits=['fixture']))
            package=packages.compile(project,knowledge,root/'map.btsknowledge',store)
            _,docs=packages.validate(package['package'])
            self.assertEqual(docs['validation.json']['factory_base'],meta['factory_base'])
            self.assertEqual(docs['validation.json']['user_overlay_sha256'],meta['files']['user-delta.safetensors'])
            self.assertIsNotNone(docs['policy.json'])
            # Simulate factory catalog replacement/unavailability after a program update.
            (factory/'catalog.json').write_text('{}')
            updated=temporal.train(dataset,store,mode='update',factory_catalog=factory,epochs=1,batch_size=8)
            child=json.loads((store/updated['generation']/'manifest.json').read_text())
            self.assertEqual(meta['factory_base'],child['factory_base'])
            for relative,digest in original.items():
                if str(relative)!='catalog.json':self.assertEqual(sha(factory/relative),digest)
            # A damaged delta cannot be silently replaced by the full checkpoint.
            del delta,effective,weights
            import gc
            gc.collect() # safetensors maps files read-only on Windows until tensors are released.
            (folder/'user-delta.safetensors').write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError,'integrity'):layers.load_effective(store,folder,meta)

    def test_factory_resume_keeps_binding_and_original_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);factory=fixture(root/'Models');dataset=sequence_fixture(root/'data');stop=[False]
            with self.assertRaises(InterruptedError):
                temporal.train(dataset,root/'user',mode='factory',factory_catalog=factory,epochs=2,batch_size=8,
                    event=lambda _:stop.__setitem__(0,True),cancelled=lambda:stop[0])
            result=temporal.train(dataset,root/'user',mode='resume',factory_catalog=factory,epochs=2,batch_size=8)
            self.assertEqual(result['factory_base']['id'],'compact-fixture')
            self.assertTrue((root/'user'/result['generation']/'user-delta.safetensors').is_file())

    def test_reject_schema_scope_corruption_and_writing_factory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);factory=fixture(root/'Models');dataset=sequence_fixture(root/'data')
            for profile,context,donor in [('balanced',4,None),('compact',16,None),('compact',4,'someone')]:
                with self.assertRaises(ValueError):layers.bind(factory,root/'user',profile,context,donor)
            with self.assertRaisesRegex(ValueError,'cannot write'):
                temporal.train(dataset,factory/'child',mode='factory',factory_catalog=factory,epochs=1,batch_size=8)
            self.assertFalse((factory/'child').exists())
            (factory/'compact-fixture'/'weights.safetensors').write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError,'integrity'):layers.catalog(factory)


if __name__=='__main__':unittest.main()
