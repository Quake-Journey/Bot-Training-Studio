"""Exercise JSONL workers in separate processes; optional local research dataset.

Run with the selected training environment's Python. No network or demo upload.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--backend", choices=["cpu", "cuda", "rocm", "xpu"], default="cpu")
    ap.add_argument("--dataset", type=Path)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipts = []
    env = dict(os.environ, PYTHONPATH=str(ROOT / "worker"), PYTHONUTF8="1")

    def job(action, **fields):
        folder = out / uuid.uuid4().hex
        folder.mkdir()
        request = dict(protocol=1, job_id=folder.name, action=action, **fields)
        (folder / "request.json").write_text(json.dumps(request), encoding="utf8")
        run = subprocess.run([sys.executable, "-u", "-m", "opentdm_x_trainer.studio",
                              "--request", str(folder / "request.json")],
                             env=env, capture_output=True, text=True, encoding="utf8", timeout=600)
        (folder / "stderr.txt").write_text(run.stderr, encoding="utf8")
        events = [json.loads(line) for line in run.stdout.splitlines()]
        assert events and events[0]["type"] == "started"
        assert all(e["seq"] == i + 1 and e["job_id"] == folder.name for i, e in enumerate(events))
        assert run.returncode == 0 and events[-1]["type"] == "completed", events[-1]
        result = events[-1]["result"]
        receipts.append(dict(action=action, fields=fields, result=result))
        print(json.dumps(dict(action=action, profile=fields.get("profile"),
                              passed=True, result=result if action == "probe" else "saved")), flush=True)

    job("hardware")
    for profile in (["reference", "compact"] if args.backend == "cpu" else ["compact", "balanced", "large", "xl"]):
        job("probe", backend=args.backend, profile=profile)
    if args.dataset:
        for profile in ("reference", "compact"):
            job("train", backend=args.backend, profile=profile, dataset=str(args.dataset.resolve()),
                store=str(out / (profile + "-store")), mode="fresh", epochs=20)
    (out / "summary.json").write_text(json.dumps(dict(pass_checks=True, jobs=receipts), indent=2), encoding="utf8")


if __name__ == "__main__":
    main()
