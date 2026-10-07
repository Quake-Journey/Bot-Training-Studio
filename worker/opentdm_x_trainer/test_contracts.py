"""Data leakage, continual replay and activation failure contracts."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

from .data import FEATURES, donor_match, features, valid_transition, prepare, load_dataset
from .learning import atomic_json, merge_samples, read_generation, tree_predict, writer_lock


def rows(prefix, mapname, n=10):
    result = {}
    for split in ("train", "validation", "test"):
        result[split + "_x"] = np.zeros((n, len(FEATURES)), dtype=np.float32)
        result[split + "_y"] = np.zeros((n, 3), dtype=np.float32)
        for field in ("identity", "recording"):
            result[split + "_" + field] = np.array([prefix + split + str(i) for i in range(n)])
        result[split + "_group"] = np.array([prefix + split] * n)
        result[split + "_map"] = np.array([mapname] * n)
    return result


class Contracts(unittest.TestCase):
    def test_donor_fuzzy_and_ambiguous_track(self):
        self.assertTrue(donor_match(["^2[clan]Pacifist"], "Pacifist"))
        self.assertTrue(donor_match(["pacifst"], "Pacifist"))
        self.assertFalse(donor_match(["Pacifist", "Purri"], "Pacifist"))
        self.assertFalse(donor_match(["aid"], "ai"))

    def test_features_only_current_past_and_enemy_mask(self):
        r = dict(view=[0, 90, 0], origin=[0, 0, 0], velocity=[0, 320, 0],
                 health=100, armor=0, grounded=True, weapon="railgun")
        x = features(r, r)
        self.assertAlmostEqual(x[0], 1.)
        self.assertAlmostEqual(x[1], 0.)
        self.assertEqual(x[11], 0.)
        r["packet_enemy_origin"] = [0, 1280, 0]
        known = features(r, r)
        self.assertEqual(known[11], 1.)
        self.assertAlmostEqual(known[12], 1.)

    def test_respawn_gap_teleport_not_a_movement_target(self):
        a = dict(time_ms=0, health=100, origin=[0, 0, 0])
        b = dict(time_ms=100, health=100, origin=[10, 0, 0], continuous=True)
        self.assertTrue(valid_transition(a, b))
        self.assertFalse(valid_transition(a, dict(b, time_ms=500)))
        self.assertFalse(valid_transition(a, dict(b, origin=[1000, 0, 0])))
        self.assertFalse(valid_transition(a, dict(b, health=0)))
        self.assertFalse(valid_transition(a, dict(b, continuous=False)))

    def test_replay_dedup_and_map_retention(self):
        a, b = rows("a", "q2duel5"), rows("b", "ztn2dm3", 100)
        combined = merge_samples(a, b, cap=20)
        self.assertEqual(sum(combined["train_map"] == "q2duel5"), 10)
        self.assertEqual(sum(combined["train_map"] == "ztn2dm3"), 10)
        dedup = merge_samples(a, a)
        self.assertEqual(len(dedup["train_x"]), 10)

    def test_leakage_rejected_between_new_and_replay(self):
        a, b = rows("a", "q2duel5"), rows("b", "ztn2dm3")
        b["test_group"] = np.array([a["train_group"][0]] * 10)
        with self.assertRaisesRegex(ValueError, "Split leakage"):
            merge_samples(a, b)

    def test_duplicate_observation_conflict(self):
        a, b = rows("a", "q2duel5"), rows("a", "q2duel5")
        b["train_y"][0, 0] = 1
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            merge_samples(a, b)

    def test_failed_pointer_replace_preserves_active(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "active.json"
            atomic_json(path, {"generation": "old"})
            with mock.patch("os.replace", side_effect=OSError("power/interruption simulation")):
                with self.assertRaises(OSError):
                    atomic_json(path, {"generation": "new"})
            self.assertEqual(json.loads(path.read_text())["generation"], "old")

    def test_writer_exclusion_and_release_on_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(RuntimeError):
                with writer_lock(root):
                    with self.assertRaises(FileExistsError):
                        with writer_lock(root):
                            pass
                    raise RuntimeError("failed learner")
            self.assertFalse((root / ".writer.lock").exists())

    def test_no_generation_path_escape(self):
        with self.assertRaises(ValueError):
            read_generation(".", "../../other")

    def test_export_rejects_nonfinite(self):
        with self.assertRaises(ValueError):
            tree_predict({}, np.full((1, len(FEATURES)), np.nan))

    def test_parquet_windows_exclude_gap_and_keep_split(self):
        import pyarrow as pa
        import pyarrow.parquet as pq
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            records, groups = [], []
            for index, split in enumerate(("train", "validation", "test")):
                rid = "recording" + str(index)
                groups.append(dict(recording_id=rid, segment=0, epoch_id=0, group=rid, split=split))
                for i in range(12):
                    records.append(dict(recording_id=rid, segment=0, epoch_id=0, slot=0,
                        time_ms=i*100, seq=i, map="fixture", aliases=["donor"], opponent_aliases=["enemy"],
                        velocity=[320., 0., 0.], origin=[float(i*32), 0., 0.], view=[0., 0., 0.],
                        health=100, armor=0, weapon="blaster", grounded=True, packet_enemy_origin=None,
                        yaw_rate=0., continuous=i != 6))
            pq.write_table(pa.Table.from_pylist(records), root / "combat.parquet")
            (root / "summary.json").write_text(json.dumps({"source_run": "fixture"}))
            (root / "groups.json").write_text(json.dumps({"source_run": "fixture", "groups": groups}))
            prepare(root / "combat.parquet", root / "groups.json", root / "out", limit=100)
            arrays, manifest = load_dataset(root / "out")
            self.assertEqual(manifest["splits"]["train"]["retained"], 4)
            self.assertTrue(all("recording0" in s for s in arrays["train_identity"]))
            np.testing.assert_allclose(arrays["test_y"], [[1., 0., 0.]] * 4)
            (root / "out" / "samples.npz").write_bytes(b"corrupted")
            with self.assertRaisesRegex(ValueError, "modified dataset"):
                load_dataset(root / "out")

    def test_incomplete_generation_cannot_be_activated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            name = "generation-" + "a" * 32
            (root / name).mkdir()
            (root / name / "manifest.json").write_text(json.dumps({"files": {}}))
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                read_generation(root, name)


if __name__ == "__main__":
    unittest.main()
