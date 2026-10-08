"""Mechanism semantics, future censorship and independent model compatibility."""
from collections import Counter
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from . import transitions as tr, mechanics_sequences as ds, mechanics_learning as ml
from .data import sha


def row(t,origin=None,velocity=None,flags=4,health=100):
    stats=[0]*32;stats[1]=health;stats[5]=50
    return dict(recording_id='demo',segment=0,epoch_id=0,slot=0,time_ms=t,
        pm_type=0,stats=stats,pm_flags=flags,gravity=800,origin=origin or [0.,0.,24.],
        velocity=velocity or [0.,0.,0.],view_angles=[0.,0.,0.])


def portal():
    return tr.inventory([dict(classname='misc_teleporter',origin='0 0 0',target='exit'),
        dict(classname='misc_teleporter_dest',origin='0 1000 14',targetname='exit')],[])


def fixture(root):
    root.mkdir();rng=np.random.default_rng(7);arrays={}
    for split in ('train','validation','test'):
        n=24
        fields=dict(x=rng.normal(0,.01,(n,4,len(ds.INPUTS))).astype(np.float32),y=np.zeros((n,6),np.float32),
            y_mask=np.ones((n,6),np.float32),events=np.tile([0,1],(n,1)).astype(np.float32),
            event_mask=np.ones((n,2),np.float32),identity=np.asarray([split+str(i) for i in range(n)]),
            group=np.asarray([split+str(i%2) for i in range(n)]),map=np.asarray(['fixture']*n),recording=np.asarray([split]*n))
        arrays.update({split+'_'+key:value for key,value in fields.items()})
    np.savez_compressed(root/'samples.npz',**arrays)
    (root/'manifest.json').write_text(json.dumps(dict(feature_version=ds.VERSION,context=4,samples_sha256=sha(root/'samples.npz'))))
    return root


class MechanismTests(unittest.TestCase):
    def test_teleporter_disc_is_not_trigger_and_exit_is_player_origin(self):
        p=portal()[0]
        self.assertEqual(p['bounds'],[[-8,-8,8],[8,8,24]])
        self.assertEqual(p['destination'],[0,1000,24])
        self.assertTrue(p['resolved'])

    def test_full_pitch_and_speed_multiplier_for_q3_pad(self):
        p=tr.inventory([dict(classname='trigger_push',model='*1',angles='-90 0 0',speed='92')],
            [([0]*3,[0]*3),([-50,-50,-128],[50,50,-121])])[0]
        np.testing.assert_allclose(p['velocity'],[0,0,920],atol=1e-10)

    def test_missing_ambiguous_or_disabled_destinations_are_not_invented(self):
        entities=[dict(classname='misc_teleporter',target='x'),dict(classname='misc_teleporter_dest',targetname='x'),
                  dict(classname='misc_teleporter_dest',targetname='x')]
        self.assertEqual(tr.inventory(entities,[])[0]['unresolved_reason'],'ambiguous_target')
        entities[2]['spawnflags']='2048'
        self.assertTrue(tr.inventory(entities,[])[0]['resolved'])
        entities[0]['spawnflags']='2048';self.assertEqual(tr.inventory(entities,[]),[])

    def test_geometry_and_flag_teleport_is_not_missing_frame_or_respawn(self):
        a=row(0);b=row(100,[0,1000,24],flags=32)
        self.assertEqual(tr.classify(a,b,portal())['kind'],'teleport')
        self.assertEqual(tr.classify(a,dict(b,time_ms=500),portal())['kind'],'gap')
        self.assertEqual(tr.classify(row(0,health=0),b,portal())['kind'],'life_boundary')
        self.assertEqual(tr.classify(row(0,[500,0,24]),b,portal())['kind'],'discontinuity')

    def test_hold_after_teleport_remains_observed(self):
        a=row(100,[0,1000,24],flags=32);b=row(200,[0,1000,24],flags=32)
        self.assertTrue(tr.classify(a,b,portal())['known'])

    def test_pad_needs_matching_impulse_not_just_nearby_jump(self):
        p=tr.inventory([dict(classname='trigger_push',model='*1',angles='-90 0 0',speed='92')],
            [([0]*3,[0]*3),([-50,-50,-128],[50,50,-121])])
        a=row(0,[0,0,-100]);b=row(100,[0,0,-40],[0,0,840],flags=0)
        self.assertEqual(tr.classify(a,b,p)['kind'],'push')
        self.assertEqual(tr.classify(a,row(100,[0,0,-80],[0,0,270],flags=0),p)['kind'],'ordinary')

    def test_window_continues_through_tp_and_counts_observed_exit(self):
        rows=[row(0)]+[row(t,[0,1000,24],flags=32 if t<300 else 4) for t in range(100,1200,100)]
        windows=list(ds.observation_windows(rows,portal(),4,Counter()))
        past,future=next((p,f) for p,f in windows if p[-1]['time_ms']==0)
        target=ds.targets(past,future)
        self.assertEqual(target['events'].tolist(),[1,0]);self.assertTrue(target['y_mask'].all())
        self.assertAlmostEqual(float(target['y'][1]),1000/1280)

    def test_pad_entry_can_lie_between_samples_before_velocity_reversal(self):
        p=tr.inventory([dict(classname='trigger_push',model='*1',angles='-90 0 0',speed='92')],
            [([0]*3,[0]*3),([-109,786,-128],[-18,877,-121])])
        a=row(2700,[-62.25,911.625,-103.875],[-21.75,-299.125,0])
        b=row(2800,[-63,893.125,-66.625],[-.5,-11.5,888],flags=0)
        self.assertEqual(tr.classify(a,b,p)['kind'],'push')

    def test_gap_and_early_recording_end_are_unknown_not_negative(self):
        windows=list(ds.observation_windows([row(0),row(100),row(600)],portal(),4,Counter()))
        past,future=windows[0];target=ds.targets(past,future)
        self.assertFalse(target['y_mask'].any());self.assertFalse(target['event_mask'].any())

    def test_encoding_contains_only_past_and_explicit_missing_geometry(self):
        past=[row(0),row(100)]
        x=ds.encode(past,[],4)
        self.assertEqual(x.shape,(4,len(ds.INPUTS)));self.assertFalse(x[:2].any())
        self.assertEqual(x[-1,17],0);self.assertEqual(x[-1,24],0)
        self.assertTrue(np.isfinite(x).all())

    def test_model_checkpoint_resume_and_candidate_does_not_certify_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=fixture(root/'data');store=root/'model'
            first=ml.train(dataset,store,epochs=1,batch_size=8)
            self.assertFalse(first['runtime_qualified']);self.assertFalse((store/'active.json').exists())
            second=ml.train(dataset,store,mode='resume',epochs=2,batch_size=8)
            self.assertEqual(second['epochs'],2)
            uninterrupted=ml.train(dataset,root/'uninterrupted',epochs=2,batch_size=8)
            self.assertEqual(sha(store/second['generation']/'weights.safetensors'),
                             sha(root/'uninterrupted'/uninterrupted['generation']/'weights.safetensors'))
            with self.assertRaisesRegex(ValueError,'new store'):
                ml.train(dataset,store,epochs=1)

    def test_worker_protocol_and_factory_protection(self):
        from . import studio
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=fixture(root/'data')
            request=dict(protocol=1,job_id='mechanisms',action='train_mechanisms',dataset=str(dataset),
                store=str(root/'model'),backend='cpu',profile='compact',epochs=1,batch_size=8)
            path=root/'request.json';path.write_text(json.dumps(request))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(studio.run(path),0)
            messages=[json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual(messages[-1]['type'],'completed')
            self.assertFalse(messages[-1]['result']['runtime_qualified'])
            request.update(store=str(root/'Models'/'overwrite'),factory_catalog=str(root/'Models'))
            path.write_text(json.dumps(request))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertNotEqual(studio.run(path),0)
            self.assertIn('factory',output.getvalue());self.assertFalse((root/'Models').exists())

    def test_cancel_preserves_completed_checkpoint_without_activation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=fixture(root/'data');store=root/'model';cancelled=[False]
            def progress(event):cancelled[0]=True
            with self.assertRaises(InterruptedError):
                ml.train(dataset,store,epochs=3,batch_size=8,event=progress,cancelled=lambda:cancelled[0])
            self.assertTrue((store/'work/resume.json').exists());self.assertFalse((store/'active.json').exists())
            result=ml.train(dataset,store,mode='resume',epochs=2,batch_size=8)
            self.assertEqual(result['epochs'],2)

    def test_low_probability_ranking_alone_does_not_qualify_an_inert_head(self):
        event=dict(positive=20,negative=100,positive_groups=2,brier=.01,prior_brier=.02,
                   average_precision=.8,prevalence=.1,recall=0.,precision=None)
        metrics=dict(arena=dict(events={name:dict(event) for name in ds.EVENTS}))
        self.assertFalse(any(ml.quality(metrics).values()))

    def test_dataset_rejects_cross_partition_match(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=fixture(Path(tmp)/'data');data,meta=ds.load(root)
            data['test_group']=np.full(len(data['test_group']),'train0');np.savez_compressed(root/'samples.npz',**data)
            meta['samples_sha256']=sha(root/'samples.npz');(root/'manifest.json').write_text(json.dumps(meta))
            with self.assertRaisesRegex(ValueError,'leakage'):ds.load(root)

    def test_package_keeps_witnesses_separate_from_executable_links(self):
        from . import maps,packages
        from .test_pipeline import bsp_fixture
        from .learning import atomic_json
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);project=root/'project';project.mkdir();bsp=bsp_fixture(project/'map.bsp')
            world=maps.inspect(bsp,'arena');world['transitions']=portal()
            atomic_json(project/'project.json',dict(map='arena',bsp_sha256=sha(bsp)))
            witness=dict(mechanism=dict(kind='teleport',id=0),runtime_qualified=False,
                         origin=[0,0,24],destination=[0,1000,24],velocity=[0,0,0])
            report=dict(bsp_sha256=sha(bsp),style=dict(donor=None,phrases=[]),nodes=[],links=[],items=[],
                        control_candidates=[],observed_mechanisms=dict(witnesses=[witness]),limits=[])
            atomic_json(root/'knowledge.json',report)
            with patch.object(maps,'inspect',return_value=world):
                result=packages.compile(project,root/'knowledge.json',root/'knowledge.zip')
            manifest,docs=packages.validate(result['package'])
            self.assertFalse(manifest['server_installable']);self.assertEqual(docs['routes.json']['links'],[])
            self.assertEqual(len(docs['routes.json']['observed_mechanisms']['witnesses']),1)


if __name__=='__main__':unittest.main()
