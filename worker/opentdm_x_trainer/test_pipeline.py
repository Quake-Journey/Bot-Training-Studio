"""Standalone import, native boundary, sequence and checkpoint regressions."""
import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import numpy as np

from . import maps, packages, projects, sequences, studio, temporal
from .data import sha
from .learning import atomic_json, writer_lock


def bsp_fixture(path):
    entities=b'{"classname" "worldspawn"}\n{"classname" "weapon_railgun" "origin" "1 2 3"}\0'
    vertices=struct.pack('<6f',0,0,0,100,100,100)
    lumps=[b'']*19;lumps[0]=entities;lumps[2]=vertices
    header=b'IBSP'+struct.pack('<i',38);offset=160
    for lump in lumps:header+=struct.pack('<ii',offset,len(lump));offset+=len(lump)
    path.write_bytes(header+b''.join(lumps))
    return path


def sequence_fixture(root):
    root.mkdir(); data={}
    rng=np.random.default_rng(19)
    for split in ('train','validation','test'):
        n=24
        x=rng.normal(0,.01,(n,4,len(sequences.INPUTS))).astype(np.float32)
        x[:,:,:3]=1.;x[:,:,15:26]=0;x[:,:,15]=1
        fields=dict(x=x,y=np.zeros((n,8),np.float32),weapon=np.zeros(n,np.int64),
            y_mask=np.ones((n,8),np.float32),events=np.zeros((n,len(sequences.EVENTS)),np.float32),
            event_mask=np.ones((n,len(sequences.EVENTS)),np.float32),
            landmark=np.zeros(n,np.int64),identity=np.asarray([split+str(i) for i in range(n)]),
            group=np.asarray([split]*n),map=np.asarray(['fixture']*n),recording=np.asarray([split]*n))
        data.update({split+'_'+key:value for key,value in fields.items()})
    np.savez_compressed(root/'samples.npz',**data)
    atomic_json(root/'manifest.json',dict(feature_version=sequences.VERSION,context=4,donor=None,
        samples_sha256=sha(root/'samples.npz')))
    return root


class PipelineTests(unittest.TestCase):
    def test_cached_map_keeps_logical_identity_and_rejects_bad_lump(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=bsp_fixture(Path(tmp)/'map.bsp')
            info=maps.inspect(path,'arena')
            self.assertEqual(info['map'],'arena')
            self.assertEqual(info['items'][0]['category'],'weapon')
            raw=bytearray(path.read_bytes());struct.pack_into('<i',raw,12,len(raw)+1);path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError,'lump'):maps.inspect(path)

    def test_archive_selection_never_uses_member_as_destination(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);archive=root/'references.zip'
            with zipfile.ZipFile(archive,'w') as z:
                z.writestr('../../outside.dm2',b'fixture')
                z.writestr('notes.txt',b'ignored')
            rows=projects.inventory([archive]);self.assertEqual(len(rows),1)
            with projects.materialize(rows[0],root) as demo:
                self.assertEqual(demo.read_bytes(),b'fixture')
                self.assertEqual(demo.resolve().parents[1],root.resolve())
            self.assertFalse(demo.exists())
            with self.assertRaisesRegex(ValueError,'512 MiB'):
                with projects.materialize(dict(rows[0],bytes=513*1024**2),root):pass

    def test_live_lock_excludes_writer_and_stale_indicator_is_recoverable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'.writer.lock').write_text('dead process')
            with writer_lock(root):
                with self.assertRaises(FileExistsError):
                    with writer_lock(root):pass
            with writer_lock(root):pass

    def test_postcommit_cancellation_does_not_report_rollback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            def committed(*args,**kwargs):
                (root/'cancel.request').touch()
                return dict(project='committed')
            atomic_json(root/'request.json',dict(protocol=1,job_id='test',action='import_project',bsp='b',inputs=[],project='p'))
            with patch.object(projects,'import_project',side_effect=committed),contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(studio.run(root/'request.json'),0)
            self.assertEqual(json.loads(output.getvalue().splitlines()[-1])['type'],'completed')

    def test_sequence_leak_and_replay_conflict_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=sequence_fixture(Path(tmp)/'data');data,_=sequences.load(folder)
            changed={k:v.copy() for k,v in data.items()}
            changed['train_x'][0,0,0]+=1
            with self.assertRaisesRegex(ValueError,'Conflicting'):temporal.replay(data,changed)
            changed={k:v.copy() for k,v in data.items()};changed['test_group']=np.asarray(['train']*24)
            with self.assertRaisesRegex(ValueError,'crossed'):temporal.replay(None,changed)

    def test_auxiliary_head_cannot_borrow_movement_qualification(self):
        metrics=dict(motion_rmse=100,persistence_rmse=150,destination_rmse=200,stationary_destination_rmse=300,
            resource_mae=.2,resource_nochange_mae=.1,weapon_accuracy=.9,weapon_persistence_accuracy=.95,
            weapon_after_switch_accuracy=.1,landmark_accuracy=.6,landmark_majority_accuracy=.6)
        expected=dict(motion=True,destination=True,resources=False,weapon=False,landmark=False)
        expected.update({event:False for event in sequences.EVENTS})
        self.assertEqual(temporal.qualify_heads({'arena':metrics}),expected)

    def test_completed_epoch_resume_matches_uninterrupted_training(self):
        from safetensors.torch import load_file
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);dataset=sequence_fixture(root/'data')
            result=temporal.train(dataset,root/'whole',epochs=3,batch_size=8)
            stop=[False]
            with self.assertRaises(InterruptedError):
                temporal.train(dataset,root/'resumed',epochs=3,batch_size=8,
                    event=lambda _:stop.__setitem__(0,True),cancelled=lambda:stop[0])
            self.assertFalse((root/'resumed'/'active.json').exists())
            resumed=temporal.train(dataset,root/'resumed',epochs=3,batch_size=8,mode='resume')
            a=load_file(str(root/'whole'/result['generation']/'weights.safetensors'))
            b=load_file(str(root/'resumed'/resumed['generation']/'weights.safetensors'))
            import torch
            self.assertTrue(all(torch.equal(a[k],b[k]) for k in a))
            self.assertEqual(result['validation'],resumed['validation'])
            self.assertLessEqual(len(list((root/'resumed'/'work').glob('checkpoint-*'))),3)

    def test_export_identity_integrity_and_rollback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);project=root/'project';project.mkdir();bsp=bsp_fixture(project/'map.bsp')
            atomic_json(project/'project.json',dict(map='arena',bsp_sha256=sha(bsp)))
            knowledge=root/'knowledge.json'
            atomic_json(knowledge,dict(bsp_sha256=sha(bsp),style=dict(donor=None,phrases=[dict(text='private')]),
                nodes=[dict(id=0,origin=[0,0,0])],links=[],items=[],control_candidates=[],limits=['static']))
            first=packages.compile(project,knowledge,root/'one.btsknowledge')
            second=packages.compile(project,knowledge,root/'two.btsknowledge')
            manifest,docs=packages.validate(first['package'])
            self.assertEqual(manifest['map'],'arena');self.assertFalse(manifest['server_installable'])
            self.assertEqual(docs['style.json']['phrases'],[])
            a=packages.install_offline(first['package'],root/'library')
            packages.install_offline(second['package'],root/'library')
            self.assertEqual(packages.rollback(root/'library',sha(bsp))['generation'],a['generation'])
            stored=root/'library'/(a['generation']+'.btsknowledge');stored.write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError,'integrity'):packages.install_offline(first['package'],root/'library')


if __name__=='__main__':unittest.main()
