"""Desktop worker protocol and real optimizer/cancellation regression checks."""
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

from . import learning, studio
from .data import VERSION, sha
from .models import create, choose_profile
from .test_contracts import rows


def fixture(folder):
    folder.mkdir()
    data = rows("synthetic-", "synthetic", 32)
    for split in ("train", "validation", "test"):
        data[split + "_x"][:, :3] = 1
    np.savez_compressed(folder / "samples.npz", **data)
    learning.atomic_json(folder / "manifest.json", dict(feature_version=VERSION,
        samples_sha256=sha(folder / "samples.npz"), donor=None))
    return folder


class StudioTests(unittest.TestCase):
    def test_temporal_context_and_gradient(self):
        import torch
        torch.set_num_threads(4)
        model = create("compact", 26)
        x = torch.randn(2, 16, 26, requires_grad=True)
        y = model(x)
        self.assertEqual(tuple(y.shape), (2, 3))
        y.square().mean().backward()
        self.assertGreater(x.grad[:, :8].abs().sum().item(), 0)
        with self.assertRaisesRegex(ValueError, "context"):
            model(torch.randn(2, 17, 26))
        self.assertEqual(choose_profile(29, "cuda"), "xl")
        self.assertEqual(choose_profile(21, "cuda"), "large")
        self.assertEqual(choose_profile(32, "cpu"), "compact")

    def test_unknown_backend_does_not_silently_use_cpu(self):
        with self.assertRaisesRegex(ValueError, "Unknown backend"):
            learning.device_for("pretend-gpu")

    def test_protocol_failure_and_prestart_cancellation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            request = root / "request.json"
            learning.atomic_json(request, dict(protocol=1, job_id="fixture", action="unknown"))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(studio.run(request), 1)
            events = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual([e["type"] for e in events], ["started", "failed"])
            self.assertEqual([e["seq"] for e in events], [1, 2])
            self.assertTrue(all(e["job_id"] == "fixture" for e in events))
            (root / "cancel.request").touch()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(studio.run(request), 2)
            self.assertFalse((root / "result.json").exists())

    def test_train_checkpoint_profile_guard_and_cancel(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = fixture(root / "data")
            store = root / "store"
            result = learning.train(dataset, store, backend="cpu", epochs=2)
            self.assertTrue(result["accepted"])
            active = (store / "active.json").read_bytes()
            folder, doc = learning.read_generation(store)
            self.assertTrue(doc["weights_changed"])
            self.assertFalse(doc["may_activate_in_game"])
            with self.assertRaisesRegex(ValueError, "family differs"):
                learning.train(dataset, store, backend="cpu", mode="update", epochs=1, profile="compact")
            cancelled = [False]
            def progress(event):
                cancelled[0] = True
            with self.assertRaises(InterruptedError):
                learning.train(dataset, store, backend="cpu", mode="update", epochs=3,
                               event=progress, cancelled=lambda: cancelled[0])
            self.assertEqual(active, (store / "active.json").read_bytes())
            self.assertFalse((store / ".writer.lock").exists())

    def test_cancellation_during_export_never_activates_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dataset = fixture(root / "data")
            store = root / "store"
            learning.train(dataset, store, backend="cpu", epochs=2)
            active = (store / "active.json").read_bytes()
            cancelled = [False]
            export = learning.export_tree
            def slow_export(*args):
                result = export(*args)
                cancelled[0] = True
                return result
            with mock.patch.object(learning, "export_tree", side_effect=slow_export):
                with self.assertRaisesRegex(InterruptedError, "after export"):
                    learning.train(dataset, store, backend="cpu", epochs=2, cancelled=lambda: cancelled[0])
            self.assertEqual(active, (store / "active.json").read_bytes())


if __name__ == "__main__":
    unittest.main()
