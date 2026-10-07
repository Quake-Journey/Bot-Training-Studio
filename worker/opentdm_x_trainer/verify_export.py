"""Compile a generated prior and compare its CPU result to the export interpreter."""
import argparse
import ctypes
import json
import subprocess
from pathlib import Path

import numpy as np

from .learning import read_generation, tree_predict


def verify(store, compiler, output):
    folder, doc = read_generation(store)
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-shared",
                    str(folder / "motion-prior.c"), "-o", str(output)], check=True, capture_output=True)
    lib = ctypes.CDLL(str(output))
    f = lib.otx_motion_prior
    ptr = ctypes.POINTER(ctypes.c_float)
    f.argtypes, f.restype = [ptr, ptr], ctypes.c_int
    policy = json.loads((folder / "motion-prior.json").read_text(encoding="utf-8"))
    with np.load(folder / "replay.npz", allow_pickle=False) as data:
        x = data["test_x"]
    # Include exactly-on and adjacent-float split boundaries, not only easy random rows.
    boundary = []
    for n in policy["nodes"]:
        if n["feature"] >= 0:
            for v in (np.float32(n["threshold"]), np.nextafter(np.float32(n["threshold"]), np.float32(np.inf))):
                row = x[0].copy()
                row[n["feature"]] = v
                boundary.append(row)
    if boundary:
        x = np.concatenate([x, np.array(boundary, dtype=np.float32)])
    expected = tree_predict(policy, x)
    actual = np.empty_like(expected)
    for row, out in zip(x, actual):
        if f(row.ctypes.data_as(ptr), out.ctypes.data_as(ptr)) != 1:
            raise RuntimeError("C evaluator rejected a valid row")
    np.testing.assert_array_equal(expected, actual)
    bad = x[0].copy()
    bad[0] = np.nan
    assert f(bad.ctypes.data_as(ptr), actual[0].ctypes.data_as(ptr)) == 0
    assert f(None, actual[0].ctypes.data_as(ptr)) == 0
    return dict(rows=len(x), bit_exact=True, nan_rejected=True, null_rejected=True,
                runtime_qualified=False, generation=folder.name)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--store", type=Path, required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--output", type=Path, required=True)
    a = ap.parse_args()
    print(json.dumps(verify(a.store, a.compiler, a.output), indent=2))
