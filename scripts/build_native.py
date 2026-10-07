"""Build bundled offline tools; no game module or external checkout is used."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(out, cc):
    source = ROOT / "native" / "engine"
    manifest = json.loads((ROOT / "native" / "source-manifest.json").read_text())
    out.mkdir(parents=True, exist_ok=True)
    for name, files in manifest["targets"].items():
        target = out / (("decoder.exe" if os.name == "nt" else "decoder") if name == "decoder"
                        else ("physics.dll" if os.name == "nt" else "physics.so"))
        command = [cc, "-std=c17", "-O2", "-flto", "-ffunction-sections", "-fdata-sections",
                   "-Wl,--gc-sections", "-I" + str(source / "inc"), "-DUSE_LITTLE_ENDIAN=1",
                   "-DUSE_SERVER=0"]
        if name == "decoder":
            command += ["-DUSE_CLIENT=1", "-DUSE_NEW_GAME_API=1", "-DUSE_MVD_CLIENT=1"]
        else:
            command += ["-shared", "-fvisibility=hidden", "-Wl,--no-undefined", "-DUSE_CLIENT=0", "-DUSE_NEW_GAME_API=0"]
            if os.name != "nt":
                command.append("-fPIC")
        if os.name == "nt":
            command.append("-Wl,--no-insert-timestamp")
        command += [str(source / f) for f in files] + ["-o", str(target), "-lm"]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf8", errors="replace")
        (out / (name + "-build.log")).write_text(result.stdout + result.stderr, encoding="utf8")
        if result.returncode:
            raise RuntimeError(result.stderr[-6000:])
        print(json.dumps(dict(target=name, bytes=target.stat().st_size,
                             sha256=hashlib.sha256(target.read_bytes()).hexdigest())))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=shutil.which("gcc"))
    ap.add_argument("--out", type=Path, default=ROOT / "dist" / "native")
    args = ap.parse_args()
    if not args.cc:
        ap.error("GCC required only for building the developer package")
    build(args.out.resolve(), args.cc)
