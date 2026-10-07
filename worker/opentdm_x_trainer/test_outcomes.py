"""Regression cases for deaths, censored demos, early spawns and masked losses."""
from collections import Counter
import contextlib
import io
from pathlib import Path
import json
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from . import sequences as seq, temporal
from .test_pipeline import sequence_fixture, bsp_fixture


def row(t, health=100, continuous=True, x=None):
    return dict(recording_id='demo',segment=0,epoch_id=1,slot=0,time_ms=t,
        health=health,armor=0,continuous=continuous,origin=[t/10 if x is None else x,0,0],
        velocity=[100,0,0],view=[0,0,0],weapon='blaster',grounded=True)


POSITIONS={name:np.empty((0,3),np.float32) for name in seq.LANDMARKS}


class OutcomeTests(unittest.TestCase):
    def test_v3_training_is_available_through_desktop_worker_protocol(self):
        from . import studio
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=sequence_fixture(root/'data')
            request=dict(protocol=1,job_id='outcome-test',action='train_sequences',dataset=str(dataset),
                store=str(root/'model'),backend='cpu',profile='compact',epochs=1,batch_size=8)
            (root/'request.json').write_text(json.dumps(request))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(studio.run(root/'request.json'),0)
            messages=[json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual(messages[-1]['type'],'completed')
            self.assertEqual(set(messages[-1]['result']['validation']['fixture']['events']),set(seq.EVENTS))

    def test_prepare_keeps_terminal_and_padded_spawn_examples(self):
        import pyarrow as pa
        import pyarrow.parquet as pq
        from bts_analysis.combat import SCHEMA,OBSERVATION_VERSION
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);bsp=bsp_fixture(root/'arena.bsp')
            rows=[];groups=[]
            for split in ('train','validation','test'):
                groups.append(dict(recording_id=split,segment=0,epoch_id=1,group=split,split=split))
                for t in range(0,1100,100):
                    rows.append(dict(row(t,100 if t<1000 else 0),recording_id=split,
                        aliases=['Donor'],opponent_aliases=['Enemy'],map='arena'))
            pq.write_table(pa.Table.from_pylist(rows,schema=SCHEMA),root/'combat.parquet')
            (root/'groups.json').write_text(json.dumps(dict(source_run='fixture',groups=groups)))
            (root/'summary.json').write_text(json.dumps(dict(source_run='fixture',observation_version=OBSERVATION_VERSION)))
            source=dict(combat=root/'combat.parquet',groups=root/'groups.json',bsp=bsp)
            seq.prepare([source],root/'dataset',context=16,limit=100)
            data,meta=seq.load(root/'dataset')
            self.assertEqual(data['train_events'][0,0],1)
            self.assertEqual(data['train_x'][0,:,-1].sum(),5)
            self.assertEqual(meta['splits']['test']['short_histories'],1)
            (root/'summary.json').write_text(json.dumps(dict(source_run='fixture')))
            with self.assertRaisesRegex(ValueError,'Re-import'):
                seq.prepare([source],root/'outdated',context=16,limit=100)
            self.assertFalse((root/'outdated').exists())

    def test_raw_decoder_death_reaches_combat_observations(self):
        from bts_analysis import combat
        from collections import defaultdict
        class Writer:
            def __init__(self):self.rows=[]
            def add(self,r):self.rows.append(r)
        facts=[dict(kind='segment_facts',segment=0,map='arena'),
               dict(kind='epoch',segment=0,epoch_id=1,start_conf=2,time_ms=0,end_ms=1000)]
        for slot in (0,1):
            facts.extend([dict(kind='track',track_id=slot,slot=slot,active_seen=True,chase_spectator=False,
                time_ms=0,end_ms=1000,identity_end_ms=None,aliases=[str(slot)]),
                dict(kind='participant',track_id=slot,epoch_id=1,active=True)])
        states=[];scenes=[];frames=[]
        for i,h in enumerate((100,0,0,100)):
            stats=[0]*32;stats[1]=h
            states.append(dict(kind='state',slot=0,segment=0,seq=i*3,time_ms=i*100,stats=stats,
                pm_type=0 if h else 2,pm_flags=0,origin=[0,0,0],velocity=[0,0,0],
                view_angles=[0,0,0],gunindex=0))
            scenes.append(dict(kind='scene',segment=0,seq=i*3+1,time_ms=i*100,entities=[],mvd=False))
            frames.append(dict(kind='frame',segment=0,seq=i*3+2,time_ms=i*100))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'facts.json').write_text(json.dumps(facts))
            contexts=Writer();counts=Counter()
            def stream(path,kind=None):return iter(states if kind=='state' else scenes if kind=='scene' else frames)
            with patch.object(combat,'rows',side_effect=stream):
                combat.prepare_record(root,'demo','arena',None,contexts,Writer(),counts,defaultdict(list))
            self.assertEqual([r['health'] for r in contexts.rows],[100,0,100])
            self.assertEqual([r['seq'] for r in contexts.rows],[2,5,11])
            self.assertEqual([r['state_seq'] for r in contexts.rows],[0,3,9])
            self.assertEqual(counts['terminal_contexts'],1)
            self.assertFalse(contexts.rows[-1]['continuous'])

    def test_death_ends_horizon_without_teaching_respawn_position(self):
        rows=[row(t) for t in range(0,1000,100)]+[row(1000,0),row(1100,100,False,x=999)]
        counts=Counter(); windows=list(seq.observation_windows(rows,16,counts))
        self.assertEqual(counts['terminal_rows'],1)
        self.assertEqual(len(windows),1)
        past,future=windows[0]; result=seq.targets(past,future,POSITIONS)
        self.assertEqual(result['events'][0],1)
        self.assertTrue(result['event_mask'].all())
        self.assertTrue(result['y_mask'][:3].all())
        self.assertFalse(result['y_mask'][3:].any())
        self.assertEqual(result['landmark'],-1)
        self.assertEqual(future[-1]['health'],0)
        self.assertLess(len(past),17)  # spawn history retained, not survivor-only

    def test_gap_is_unknown_not_survival_or_teleport_destination(self):
        rows=[row(t) for t in range(0,900,100)]+[row(1800,continuous=False,x=10000)]
        past,future=list(seq.observation_windows(rows,4,Counter()))[0]
        result=seq.targets(past,future,POSITIONS)
        self.assertFalse(result['event_mask'].any())
        self.assertFalse(result['y_mask'][3:].any())
        self.assertTrue(result['y_mask'][:3].all())

    def test_terminal_before_weapon_horizon_masks_weapon_and_velocity(self):
        rows=[row(t) for t in range(0,600,100)]+[row(600,0)]
        past,future=list(seq.observation_windows(rows,4,Counter()))[0]
        result=seq.targets(past,future,POSITIONS)
        self.assertEqual(result['weapon'],-1)
        self.assertFalse(result['y_mask'].any())
        self.assertEqual(result['events'][0],1)

    def test_complete_window_and_positive_censored_event(self):
        rows=[row(t,health=80 if t>=700 else 100) for t in range(0,3600,100)]
        past,future=list(seq.observation_windows(rows,4,Counter()))[0]
        result=seq.targets(past,future,POSITIONS)
        self.assertTrue(result['y_mask'].all())
        np.testing.assert_array_equal(result['events'],[0,1,0])
        np.testing.assert_array_equal(result['event_mask'],[1,1,1])
        short=seq.targets(past,future[:3],POSITIONS)
        np.testing.assert_array_equal(short['event_mask'],[0,1,0])

    def test_mega_decay_does_not_count_as_substantial_loss(self):
        past=[row(400,150),row(500,150)]
        future=[row(t,150-(t-500)//1000) for t in range(600,3600,100)]
        result=seq.targets(past,future,POSITIONS)
        self.assertEqual(result['events'][1],0)
        self.assertEqual(result['event_mask'][1],1)
        future[-1]['armor']=0;past[-1]['armor']=25
        self.assertEqual(seq.targets(past,future,POSITIONS)['events'][1],1)

    def test_ammo_zero_is_known_and_never_fabricates_inventory(self):
        a=row(0);b=dict(row(100),ammo_observed=0,ammo_known=True,last_shot_age_ms=100,
            last_shot_known=True,packet_enemy_velocity=[0,100,0])
        encoded=seq.encode([a,b],POSITIONS)[0]
        self.assertEqual(encoded[seq.INPUTS.index('ammo_current')],0)
        self.assertEqual(encoded[seq.INPUTS.index('ammo_known')],1)
        unknown=seq.encode([a,dict(b,ammo_known=False,packet_enemy_velocity=None)],POSITIONS)[0]
        self.assertEqual(unknown[seq.INPUTS.index('ammo_known')],0)
        self.assertEqual(unknown[seq.INPUTS.index('enemy_velocity_known')],0)

    def test_masked_targets_do_not_change_loss_or_metrics(self):
        import torch
        with tempfile.TemporaryDirectory() as tmp:
            dataset=sequence_fixture(Path(tmp)/'data');data,_=seq.load(dataset)
            data['train_y_mask'][:]=0;data['train_event_mask'][:]=0
            data['train_weapon'][:]=-1;data['train_landmark'][:]=-1
            pred=torch.zeros((24,temporal.OUTPUTS),requires_grad=True)
            loss=temporal.batch_loss(pred,data,np.arange(24),torch.device('cpu'),np.ones(24))
            self.assertEqual(loss.item(),0)
            loss.backward();self.assertEqual(pred.grad.abs().sum().item(),0)
            data['test_y_mask'][:]=0;data['test_event_mask'][:]=0
            metrics=temporal.score_predictions(np.zeros((24,temporal.OUTPUTS)),data,'test',np.ones(24,bool),np.ones(24,bool))
            self.assertIsNone(metrics['motion_rmse'])
            self.assertIsNone(metrics['events'][seq.EVENTS[0]]['brier'])

    def test_switch_weighting_uses_training_only_and_is_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            dataset=sequence_fixture(Path(tmp)/'data');data,_=seq.load(dataset)
            data['train_weapon'][0]=9
            weights=temporal.weapon_sample_weights(data)
            self.assertEqual(weights[0],8)
            self.assertTrue((weights[1:]==1).all())
            data['test_weapon'][:]=9;data['validation_weapon'][:]=9
            np.testing.assert_array_equal(weights,temporal.weapon_sample_weights(data))

    def test_outcome_retention_cannot_borrow_motion_success(self):
        before={'arena':dict(motion_rmse=10,destination_rmse=20,
            events={'death_within_3s':dict(positive=20,negative=100,brier=.02,prior_brier=.04)})}
        after={'arena':dict(motion_rmse=9,destination_rmse=19,
            events={'death_within_3s':dict(brier=.04)})}
        self.assertIn('old_map_death_within_3s_regression:arena',temporal.retention_failures(before,after))

    def test_style_uses_known_geometry_and_excludes_holdout_preferences(self):
        import pyarrow as pa
        import pyarrow.parquet as pq
        from bts_analysis.combat import SCHEMA
        from .behaviour import classify,profile
        a=dict(row(500),map='arena',aliases=['Donor'],opponent_aliases=['Opponent'],
            packet_enemy_origin=[500,0,0],center_ray_clear=True,collision_complete=False,weapon='railgun')
        self.assertEqual(classify(a)[0]['visibility'],'unknown')
        self.assertEqual(classify(a)[1],'approaching')
        a['velocity']=[-100,0,0]
        self.assertEqual(classify(a)[1],'separating')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            b=dict(a,recording_id='test',weapon='chaingun')
            pq.write_table(pa.Table.from_pylist([a,b],schema=SCHEMA),root/'combat.parquet')
            (root/'summary.json').write_text(json.dumps(dict(source_run='fixture')))
            (root/'groups.json').write_text(json.dumps(dict(source_run='fixture',groups=[
                dict(recording_id=name,segment=0,epoch_id=1,split=split)
                for name,split in [('demo','train'),('test','test')]])))
            result=profile(root/'combat.parquet',root/'groups.json','Donor')
            self.assertEqual(result['conditions'][0]['weapon'],{'railgun':1.})
            self.assertFalse(result['conditions'][0]['supported'])
            self.assertEqual(result['counts']['heldout_or_quarantine'],1)


if __name__=='__main__':unittest.main()
