"""Evidence boundaries and independent decision-learning regressions."""
from pathlib import Path
import contextlib
import io
import json
import tempfile
import unittest

import numpy as np

from bts_analysis.items import pickup, packet_items
from . import decisions as ds, decision_learning as learn, studio
from .data import sha
from .test_outcomes import row


def state(t, label=0, health=100, **extra):
    stats=[0]*32;stats[1]=health;stats[8]=label
    return dict(time_ms=t,seq=t,stats=stats,pm_type=0,**extra)


def fixture(folder):
    folder.mkdir();data={};rng=np.random.default_rng(1)
    for split in ('train','validation','test'):
        n=24;x=rng.normal(0,.01,(n,4,len(ds.INPUTS))).astype(np.float32)
        data.update({split+'_'+k:v for k,v in dict(x=x,weapon=np.zeros(n,np.int64),pickup=np.full(n,ds.NONE,np.int64),
            current_weapon=np.zeros(n,np.int64),nearest_item=np.zeros(n,np.int64),
            identity=np.asarray([split+str(i) for i in range(n)]),group=np.asarray([split]*n),
            map=np.asarray(['arena']*n),recording=np.asarray([split]*n)).items()})
    np.savez_compressed(folder/'samples.npz',**data)
    (folder/'manifest.json').write_text(json.dumps(dict(feature_version=ds.VERSION,inputs=ds.INPUTS,
        context=4,samples_sha256=sha(folder/'samples.npz'))))
    return folder


class DecisionTests(unittest.TestCase):
    def test_geometry_reuse_excludes_changed_record_and_checks_integrity(self):
        from collections import Counter
        import pyarrow as pa
        import pyarrow.parquet as pq
        from bts_analysis.analyzer import TableWriter
        from .projects import reuse_observations
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);prior=root/'revisions'/'prior';prior.mkdir(parents=True)
            schema=pa.schema([('recording_id',pa.string()),('value',pa.int64())])
            table=pa.Table.from_pylist([dict(recording_id='a',value=1),dict(recording_id='b',value=2)],schema=schema)
            for name in ('combat.parquet','shots.parquet'):pq.write_table(table,prior/name)
            signature=dict(version='test',physics='p')
            old=dict(active_revision='prior',observation_signature=signature,
                observation_files={n:sha(prior/n) for n in ('combat.parquet','shots.parquet')},
                recordings=[dict(recording_id=k,path=k) for k in ('a','b')],
                record_counts={k:dict(contexts=1,recordings=1) for k in ('a','b')})
            records={k:dict(path=k if k=='a' else 'new-b') for k in ('a','b')}
            counts=Counter();per_record={}
            a=TableWriter(root/'out.parquet',schema);b=TableWriter(root/'shots.parquet',schema)
            try:
                self.assertEqual(reuse_observations(root,old,records,dict(version='changed'),a,b,counts,per_record),set())
                self.assertEqual(reuse_observations(root,old,records,signature,a,b,counts,per_record),{'a'})
                (prior/'combat.parquet').write_bytes(b'corrupt')
                with self.assertRaisesRegex(ValueError,'integrity'):
                    reuse_observations(root,old,records,signature,a,b,counts,per_record)
            finally:a.close();b.close()
            self.assertEqual(pq.read_table(root/'out.parquet').to_pylist(),[dict(recording_id='a',value=1)])
            self.assertEqual(dict(counts),dict(contexts=1,recordings=1))

    def test_decoder_change_refreshes_all_sources_without_duplicate_imports(self):
        from .projects import import_sources
        a=dict(path='example-a.dm2',member=None,bytes=10)
        b=dict(path='example-b.zip',member='b.dm2',bytes=20)
        old=dict(decode_pipeline='previous',recordings=[dict(source=a)],issues=[dict(source=b)])
        rows,refresh=import_sources(old,[a],'current','update')
        self.assertTrue(refresh);self.assertEqual(len(rows),2)
        self.assertEqual({r['path'] for r in rows},{a['path'],b['path']})
        rows,refresh=import_sources(old,[],'previous','update')
        self.assertFalse(refresh);self.assertEqual(rows,[])
        rows,refresh=import_sources(old,[a],'current','fresh')
        self.assertFalse(refresh);self.assertEqual(rows,[a])

    def test_pickup_label_edge_not_lingering_text_or_health_guess(self):
        configs={1000:'Railgun',1001:'Health'}
        self.assertEqual(pickup(state(0),state(100,1000),configs,1000)['item'],'weapon_railgun')
        self.assertIsNone(pickup(state(0,1000),state(100,1000),configs,1000))
        self.assertEqual(pickup(state(0),state(100,1001,200),configs,1000)['item'],'item_health')
        self.assertIsNone(pickup(state(0),state(100,0,200),configs,1000))
        self.assertIsNone(pickup(state(0),state(100,999),{999:'Railgun'},1000))

    def test_initial_hud_gap_death_and_spectator_are_not_pickups(self):
        configs={1000:'Railgun'}
        self.assertIsNone(pickup(None,state(100,1000),configs,1000))
        self.assertIsNone(pickup(state(0),state(1000,1000),configs,1000))
        self.assertIsNone(pickup(state(0,health=0),state(100,1000),configs,1000))
        spectator=state(0);spectator['stats'][16]=1
        self.assertIsNone(pickup(spectator,state(100,1000),configs,1000))

    def test_memory_cannot_see_future_inventory_and_resets_on_respawn(self):
        rows=[dict(row(0),weapon='railgun'),dict(row(100),weapon='blaster'),
              dict(row(200,0),weapon='blaster'),dict(row(300,continuous=False),weapon='blaster')]
        observed=list(ds.add_memory(rows));at=ds.MEMORY.index('equipped_railgun_known')
        self.assertEqual(observed[1]['decision_memory'][at],1)
        self.assertEqual(observed[3]['decision_memory'][at],0)
        unknown=ds.MEMORY.index('equipped_chaingun_known')
        self.assertEqual(observed[0]['decision_memory'][unknown],0)
        rows[-1]['weapon']='chaingun'
        self.assertEqual(list(ds.add_memory(rows))[0]['decision_memory'][unknown],0)

    def test_next_shot_is_not_equipped_future_weapon_or_preceding_flash(self):
        current=dict(row(500),seq=50);future=[dict(row(600),seq=60,weapon='railgun')]
        shot=dict(seq=55,time_ms=500,context_seq=50,weapon='chaingun')
        weapon,_=ds.labels(current,future,[dict(shot,seq=49),shot],[49,55])
        self.assertEqual(ds.WEAPONS[weapon],'chaingun')
        self.assertEqual(ds.labels(current,future,[],[])[0],-1)
        self.assertEqual(ds.labels(current,future,[dict(shot,seq=61)],[61])[0],-1)
        self.assertEqual(ds.labels(current,future,[dict(shot,context_seq=49)],[55])[0],-1)

    def test_pickup_target_censor_and_unknown_item(self):
        current=dict(row(500),seq=50)
        future=[dict(row(600),seq=60,pickup_observation_known=True)]
        self.assertEqual(ds.labels(current,future,[],[])[1],-1)
        future[-1].update(time_ms=3500)
        self.assertEqual(ds.labels(current,future,[],[])[1],ds.NONE)
        future[-1].update(pickup_item='weapon_railgun')
        self.assertEqual(ds.ITEMS[ds.labels(current,future,[],[])[1]],'weapon_railgun')
        future[-1].update(pickup_item='unknown')
        self.assertEqual(ds.labels(current,future,[],[])[1],-1)

    def test_training_weights_ignore_holdouts(self):
        data=dict(train_weapon=np.array([0,0,0,1]),test_weapon=np.array([2]))
        a=learn.weights(data,'weapon',11);data['test_weapon'][:]=0
        np.testing.assert_array_equal(a,learn.weights(data,'weapon',11))
        self.assertTrue((a<=4).all())

    def test_probability_correction_undoes_training_emphasis_without_test_labels(self):
        data=dict(train_weapon=np.asarray([0]*9+[1]),train_current_weapon=np.zeros(10,np.int64),
            train_pickup=np.array([ds.NONE]*10),test_current_weapon=np.array([0]))
        truth=np.array([.6,.4]+[1e-7]*(len(ds.WEAPONS)-2));truth/=truth.sum()
        weighted=truth*learn.weights(data,'weapon',len(ds.WEAPONS))
        weighted[1:]*=learn.switch_factor(data)
        pred=np.zeros((1,learn.OUTPUTS),np.float32);pred[0,:learn.WIDTH]=np.log(weighted)
        self.assertEqual(pred[0,:learn.WIDTH].argmax(),1)
        corrected=learn.corrected_logits(pred,data,'test')[0,:learn.WIDTH]
        probability=np.exp(corrected-corrected.max());probability/=probability.sum()
        np.testing.assert_allclose(probability,truth,rtol=1e-6,atol=1e-7)
        self.assertEqual(probability.argmax(),0)

    def test_item_features_distinguish_ammo_and_include_small_health(self):
        world=dict(items=[dict(classname='ammo_bullets',origin=[100,0,0],spawnflags=0),
            dict(classname='ammo_rockets',origin=[0,200,0],spawnflags=0),
            dict(classname='item_health_small',origin=[20,0,0],spawnflags=0),
            dict(classname='item_health',origin=[1000,0,0],spawnflags=0)])
        positions=ds.item_positions(world)
        encoded=ds.encode_items([dict(row(0),origin=[0,0,0]),dict(row(100),origin=[0,0,0])],positions)[0]
        self.assertAlmostEqual(encoded[ds.PLACEMENTS.index('item_ammo_bullets_forward')],100/1280)
        self.assertAlmostEqual(encoded[ds.PLACEMENTS.index('item_ammo_rockets_left')],200/1280)
        self.assertAlmostEqual(encoded[ds.PLACEMENTS.index('item_item_health_forward')],20/1280)
        self.assertEqual(encoded[ds.PLACEMENTS.index('item_item_armor_body_exists')],0)

    def test_packet_item_presence_is_not_inferred_from_static_map(self):
        scene=dict(entities=[dict(number=42,models=[1,0,0,0],origin=[100,0,0]),
            dict(number=1,models=[255,1,0,0],origin=[0,0,0])])
        observed=packet_items(scene,{33:'models/weapons/g_rail/tris.md2'},32)
        self.assertEqual(observed,[dict(item='weapon_railgun',origin=[100,0,0],entity=42)])
        r=dict(row(100),origin=[0,0,0],packet_items=observed,gunframe=20)
        encoded=ds.encode_packet_items([r,r])[0]
        self.assertEqual(encoded[ds.PACKET_INPUTS.index('packet_weapon_railgun_observed')],1)
        self.assertEqual(encoded[ds.PACKET_INPUTS.index('packet_item_armor_body_observed')],0)
        self.assertEqual(encoded[-1],1)
        missing=ds.encode_packet_items([r,dict(r,packet_items=[],gunframe=None)])[0]
        self.assertEqual(missing[ds.PACKET_INPUTS.index('packet_weapon_railgun_observed')],0)
        self.assertEqual(missing[-1],0)

    def test_independent_quality_gate_rejects_one_match_even_if_perfect(self):
        report={'arena':dict(weapon=dict(groups=1,changed_shots=100,comparable_accuracy=1.,
            persistence_accuracy=.8,changed_shot_accuracy=1.,same_weapon_accuracy=1.),
            pickup=dict(groups=1,observed_edges=100,accuracy=1.,majority_accuracy=.8,
                observed_edge_accuracy=1.,nearest_item_accuracy=.2,macro_f1=1.))}
        self.assertEqual(learn.qualified(report,report),dict(weapon=False,pickup=False))

    def test_worker_training_and_resume_preserve_exact_cpu_result(self):
        from safetensors.torch import load_file
        import torch
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=fixture(root/'data')
            whole=learn.train(dataset,root/'whole',epochs=2,batch_size=8)
            stop=[False]
            with self.assertRaises(InterruptedError):
                learn.train(dataset,root/'resume',epochs=2,batch_size=8,
                    event=lambda _:stop.__setitem__(0,True),cancelled=lambda:stop[0])
            self.assertFalse((root/'resume'/'latest-experiment.json').exists())
            request=dict(protocol=1,job_id='decision',action='train_decisions',dataset=str(dataset),
                store=str(root/'resume'),backend='cpu',mode='resume',epochs=2,batch_size=8)
            (root/'request.json').write_text(json.dumps(request))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(studio.run(root/'request.json'),0)
            result=json.loads(output.getvalue().splitlines()[-1])['result']
            a=load_file(str(root/'whole'/whole['generation']/'weights.safetensors'))
            b=load_file(str(root/'resume'/result['generation']/'weights.safetensors'))
            self.assertTrue(all(torch.equal(a[k],b[k]) for k in a))
            self.assertEqual(whole['test'],result['test'])
            self.assertFalse((root/'resume'/'active.json').exists())

    def test_decision_replay_deduplicates_and_rejects_leaks_and_conflicts(self):
        with tempfile.TemporaryDirectory() as tmp:
            data,_=ds.load(fixture(Path(tmp)/'data'))
            merged=ds.replay(data,data)
            self.assertEqual(len(merged['train_x']),len(data['train_x']))
            changed={k:v.copy() for k,v in data.items()}
            changed['train_weapon'][0]=1
            with self.assertRaisesRegex(ValueError,'Conflicting'):ds.replay(data,changed)
            crossed={k:v.copy() for k,v in data.items()}
            for field in ds.FIELDS:
                crossed['train_'+field]=data['test_'+field]
                crossed['test_'+field]=data['train_'+field]
            with self.assertRaisesRegex(ValueError,'crossed'):ds.replay(data,crossed)

    def test_update_keeps_parent_and_old_maps_on_cancel_and_resume(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a=fixture(root/'a');b=fixture(root/'b')
            data,meta=ds.load(b)
            for split in ('train','validation','test'):
                for field in ('identity','group','recording'):
                    data[split+'_'+field]=np.char.add('new-',data[split+'_'+field])
                data[split+'_map']=np.full(24,'new-arena')
            np.savez_compressed(b/'samples.npz',**data)
            meta['samples_sha256']=sha(b/'samples.npz');(b/'manifest.json').write_text(json.dumps(meta))
            parent=learn.train(a,root/'store',epochs=1,batch_size=8)
            previous=(root/'store'/'latest-experiment.json').read_bytes();stop=[False]
            with self.assertRaises(InterruptedError):
                learn.train(b,root/'store',mode='update',epochs=2,batch_size=8,
                    event=lambda _:stop.__setitem__(0,True),cancelled=lambda:stop[0])
            self.assertEqual((root/'store'/'latest-experiment.json').read_bytes(),previous)
            result=learn.train(b,root/'store',mode='resume',epochs=2,batch_size=8)
            folder,model,replay=learn.experiment(root/'store')
            self.assertEqual(model['parent_generation'],parent['generation'])
            self.assertEqual(set(replay['train_map']),{'arena','new-arena'})
            self.assertEqual(len(replay['test_x']),48)
            self.assertIn('arena',result['retention']['test'])
            self.assertTrue((root/'store'/parent['generation']/'weights.safetensors').is_file())
            self.assertFalse((root/'store'/'active.json').exists())

    def test_retention_cannot_hide_a_rare_weapon_regression(self):
        prior=dict(accuracy=.9,changed_shot_accuracy=.6,macro_f1=.7)
        after=dict(accuracy=.95,changed_shot_accuracy=.1,macro_f1=.8)
        before={s:dict(arena=dict(weapon=prior)) for s in ('validation','test')}
        current={s:dict(arena=dict(weapon=after)) for s in ('validation','test')}
        self.assertFalse(learn.retention(before,current)['test']['arena']['weapon']['retained'])


if __name__=='__main__':unittest.main()
