"""python -m opentdm_x_trainer (from the repository worker directory)."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="OpenTDM-X offline learning research worker")
    sub = parser.add_subparsers(dest="command", required=True)
    doctor = sub.add_parser("doctor", help="Actual forward/backward/weight-update GPU probe")
    doctor.add_argument("--backend", choices=("cpu", "cuda", "rocm", "xpu", "directml"), required=True)
    prepare = sub.add_parser("prepare", help="Import existing analyzer observations; no demo copies")
    prepare.add_argument("--combat", type=Path, required=True)
    prepare.add_argument("--groups", type=Path, required=True)
    prepare.add_argument("--out", type=Path, required=True)
    prepare.add_argument("--limit", type=int, default=12000)
    prepare.add_argument("--donor")
    train = sub.add_parser("train", help="Fit or continually update an OFFLINE model with replay")
    train.add_argument("--dataset", type=Path, required=True)
    train.add_argument("--store", type=Path, required=True)
    train.add_argument("--backend", choices=("cpu", "cuda", "rocm", "xpu", "directml"), required=True)
    train.add_argument("--mode", choices=("fresh", "update", "cancel"), required=True)
    train.add_argument("--epochs", type=int, default=60)
    train.add_argument("--seed", type=int, default=23)
    train.add_argument("--profile", choices=("reference", "compact", "balanced", "large", "xl"), default="reference")
    status = sub.add_parser("status")
    status.add_argument("--store", type=Path, required=True)
    rollback = sub.add_parser("rollback", help="Restore a verified OFFLINE checkpoint")
    rollback.add_argument("--store", type=Path, required=True)
    rollback.add_argument("--generation", required=True)
    a = parser.parse_args()
    if a.command == "doctor":
        from .learning import doctor
        result = doctor(a.backend)
    elif a.command == "prepare":
        from .data import prepare
        result = prepare(a.combat, a.groups, a.out, a.limit, a.donor)
    elif a.command == "train":
        if a.mode == "cancel":
            result = {"cancelled": True, "files_changed": False}
        else:
            from .learning import train
            result = train(a.dataset, a.store, a.backend, a.mode, a.epochs, a.seed, profile=a.profile)
    elif a.command == "rollback":
        from .learning import rollback
        result = rollback(a.store, a.generation)
    else:
        from .learning import read_generation
        folder, manifest = read_generation(a.store)
        result = dict(path=str(folder.resolve()), backend=manifest["backend"], donor=manifest["donor"],
                      mode=manifest["mode"], parent=manifest["parent"], validation=manifest["validation"],
                      runtime_qualified=False)
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
