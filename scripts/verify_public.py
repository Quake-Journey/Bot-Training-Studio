"""Export and inspect the staged SOURCE snapshot before public publication.

Run a separate secret scanner on the resulting folder as a second check.
No GitHub authentication or publication is performed here.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import zipfile
from io import BytesIO

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = {".dm2", ".mvd2", ".bsp", ".cfg", ".safetensors", ".npz", ".parquet", ".exe", ".dll", ".pdb"}
PATTERNS = [rb"[A-Za-z]:[\\/]Claude2", rb"(?i)rcon_password\s+[\"']?[^\s\"']+",
            rb"gh[pousr]_[A-Za-z0-9]{30,}", rb"github_pat_[A-Za-z0-9_]{40,}",
            rb"-----BEGIN [A-Z ]*PRIVATE KEY-----"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists():
        raise ValueError("Choose a new empty snapshot path")
    out.mkdir(parents=True)
    files = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    count, total = 0, 0
    for name in filter(None, files):
        path = Path(name)
        if path.suffix.lower() in FORBIDDEN or any(p in {"local", "data", "models", "artifacts", "dist", "bin", "obj", ".venv"} for p in path.parts):
            raise ValueError("Private/generated content staged: " + name)
        data = subprocess.check_output(["git", "show", ":" + name], cwd=ROOT)
        if len(data) > 1_000_000:
            raise ValueError("Unexpected large source file: " + name)
        if path.suffix.lower() != ".png" and any(re.search(pattern, data) for pattern in PATTERNS):
            raise ValueError("Private path or credential-like content: " + name)
        if path.suffix.lower() == ".docx":
            with zipfile.ZipFile(BytesIO(data)) as document:
                for part in document.namelist():
                    if part.endswith((".xml", ".rels")):
                        content = document.read(part)
                        if any(re.search(pattern, content) for pattern in PATTERNS):
                            raise ValueError("Private content in document: " + name)
                        # Expand text for the separate secret scanner as well. This inspection directory
                        # belongs to the exported scan snapshot, never to the published source tree.
                        inspection = out / "_document_inspection" / path.stem / part.replace("/", "_")
                        inspection.parent.mkdir(parents=True, exist_ok=True)
                        inspection.write_bytes(content)
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        count += 1
        total += len(data)
    print(json.dumps(dict(files=count, bytes=total, scoped_source_check=True)))


if __name__ == "__main__":
    main()
