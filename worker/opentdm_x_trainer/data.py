"""Bounded, provenance-preserving observational movement dataset.

Targets are future recorded velocities, NOT recovered usercmds or optimal actions.
Unknown opponent position stays unknown; no future state enters model features.
"""
from __future__ import annotations

import hashlib
import heapq
import json
import math
import re
import unicodedata
from collections import Counter, deque
from pathlib import Path

import numpy as np

VERSION = "observed-motion-v1"
WEAPONS = ("blaster", "shotgun", "supershotgun", "machinegun", "chaingun",
           "handgrenade", "grenadelauncher", "rocketlauncher", "hyperblaster", "railgun", "bfg")
FEATURES = ["forward_velocity", "left_velocity", "up_velocity",
            "previous_forward_velocity", "previous_left_velocity", "previous_up_velocity",
            "pitch", "yaw_rate", "health", "armor", "grounded",
            "enemy_known", "enemy_forward", "enemy_left", "enemy_up"] + ["weapon_" + w for w in WEAPONS]
TARGETS = ["future_forward_velocity", "future_left_velocity", "future_up_velocity"]


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def nickname(s):
    s = "".join(chr(ord(c) & 127) if ord(c) < 256 else c for c in s)
    s = re.sub(r"\^[0-9]", "", s)
    return "".join(c for c in unicodedata.normalize("NFKD", s).casefold() if c.isalnum())


def donor_match(aliases, requested):
    """Decorations and one typo in a long name; unrelated track aliases reject.

    This is candidate attribution, not proof of real-person identity. Never use
    it to merge train/test groups; those come from whole-match metadata.
    """
    wanted = nickname(requested)
    if len(wanted) < 3 or not aliases:
        return False

    def close(actual):
        if actual == wanted:
            return True
        if len(wanted) >= 5 and wanted in actual and len(actual) <= len(wanted) + 8:
            return True
        if len(wanted) < 6 or abs(len(actual) - len(wanted)) > 1:
            return False
        previous = list(range(len(wanted) + 1))
        for i, ch in enumerate(actual, 1):
            current = [i]
            for j, target in enumerate(wanted, 1):
                current.append(min(current[-1] + 1, previous[j] + 1, previous[j - 1] + (ch != target)))
            previous = current
        return previous[-1] <= 1
    return all(close(nickname(n)) for n in aliases)


def local(v, yaw):
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    return [(v[0] * c + v[1] * s) / 320., (-v[0] * s + v[1] * c) / 320., v[2] / 320.]


def features(previous, current):
    yaw = current["view"][1]
    enemy = current.get("packet_enemy_origin")
    relative = local([e - o for e, o in zip(enemy, current["origin"])], yaw) if enemy else [0., 0., 0.]
    x = local(current["velocity"], yaw) + local(previous["velocity"], yaw)
    x += [current["view"][0] / 90., max(-4., min(4., (current.get("yaw_rate") or 0.) / 180.)),
          current["health"] / 200., current["armor"] / 200., float(current["grounded"]),
          float(enemy is not None)] + [v / 4. for v in relative]
    x += [float(current["weapon"] == w) for w in WEAPONS]
    return x


def valid_transition(a, b):
    dt = b["time_ms"] - a["time_ms"]
    return (b.get("continuous") and 70 <= dt <= 130 and a["health"] > 0 and b["health"] > 0
            and math.dist(a["origin"], b["origin"]) < 160.)


class Reservoir:
    """Deterministic hash sampling; a bounded amount per split across the stream."""
    def __init__(self, limit):
        self.limit, self.heap, self.seen = limit, [], 0

    def add(self, identity, sample):
        self.seen += 1
        key = int(hashlib.sha256(identity.encode()).hexdigest(), 16)
        value = (-key, identity, sample)
        if len(self.heap) < self.limit:
            heapq.heappush(self.heap, value)
        elif value[:2] > self.heap[0][:2]:
            heapq.heapreplace(self.heap, value)

    def rows(self):
        return [v[2] for v in sorted(self.heap, key=lambda v: v[1])]


def prepare(combat, groups_path, out, limit=12000, donor=None):
    import pyarrow.parquet as pq
    combat, groups_path, out = map(Path, (combat, groups_path, out))
    if out.exists():
        raise ValueError("Dataset destination already exists; use a new path")
    if not 100 <= limit <= 100000:
        raise ValueError("limit must be 100..100000 per split")
    groupdoc = json.loads(groups_path.read_text(encoding="utf-8"))
    summary = json.loads((combat.parent / "summary.json").read_text(encoding="utf-8"))
    if summary["source_run"] != groupdoc["source_run"]:
        raise ValueError("Combat/group source_run mismatch")
    groups = {}
    for r in groupdoc["groups"]:
        key = (r["recording_id"], r["segment"], r["epoch_id"])
        if key in groups and groups[key] != r:
            raise ValueError("Conflicting whole-match metadata")
        groups[key] = r
    assignments = {}
    for r in groups.values():
        if r["split"] != "quarantine":
            if r["group"] in assignments and assignments[r["group"]] != r["split"]:
                raise ValueError("A whole-match group leaks across splits")
            assignments[r["group"]] = r["split"]
    pools = {s: Reservoir(limit) for s in ("train", "validation", "test")}
    counts, buffers = Counter(), {}
    donor_cache = {}
    aliases_seen = set()
    last_recording = None
    columns = ["recording_id", "segment", "epoch_id", "slot", "time_ms", "seq", "map",
               "aliases", "opponent_aliases", "velocity", "origin", "view", "health", "armor",
               "weapon", "grounded", "packet_enemy_origin", "yaw_rate", "continuous"]
    fingerprint = sha(combat)
    before = combat.stat()
    for batch in pq.ParquetFile(combat).iter_batches(batch_size=4096, columns=columns):
        for r in batch.to_pylist():
            counts["input_rows"] += 1
            key = (r["recording_id"], r["segment"], r["epoch_id"])
            group = groups.get(key)
            if not group or group["split"] not in pools:
                counts["quarantined"] += 1
                continue
            if donor:
                aliases_key = (tuple(r["aliases"]), tuple(r["opponent_aliases"]))
                if aliases_key not in donor_cache:
                    if len(donor_cache) >= 4096:
                        donor_cache.clear()
                    donor_cache[aliases_key] = (donor_match(r["aliases"], donor)
                                                and not donor_match(r["opponent_aliases"], donor))
                if not donor_cache[aliases_key]:
                    counts["not_unambiguous_donor"] += 1
                    continue
            values = r["origin"] + r["view"] + r["velocity"]
            if (not all(v is not None and math.isfinite(v) for v in values)
                    or r["health"] is None or r["armor"] is None or r["health"] <= 0):
                counts["invalid_state"] += 1
                buffers.pop(key + (r["slot"],), None)
                continue
            if r["recording_id"] != last_recording:
                buffers.clear()
                last_recording = r["recording_id"]
            q = buffers.setdefault(key + (r["slot"],), deque(maxlen=5))
            if q and not valid_transition(q[-1], r):
                q.clear()
                counts["discontinuity"] += 1
            q.append(r)
            if len(q) != 5:
                continue
            old, current, future = q[0], q[1], q[-1]
            horizon = future["time_ms"] - current["time_ms"]
            if not 270 <= horizon <= 330:
                counts["wrong_horizon"] += 1
                continue
            x = features(old, current)
            y = local(future["velocity"], current["view"][1])
            if not all(math.isfinite(v) and abs(v) <= 20. for v in x + y):
                counts["outlier"] += 1
                continue
            # Stable ID survives re-export and file renames; paired POVs share split.
            identity = ":".join(map(str, key + (r["slot"], current["time_ms"])))
            sample = dict(x=x, y=y, identity=identity, group=group["group"], map=r["map"],
                          aliases=r["aliases"], recording=r["recording_id"])
            pools[group["split"]].add(identity, sample)
            aliases_seen.update(r["aliases"])
    after = combat.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Source changed during ingestion")
    arrays, manifests = {}, {}
    for split, pool in pools.items():
        rows = pool.rows()
        if not rows:
            raise ValueError("No eligible samples in " + split)
        arrays[split + "_x"] = np.array([r["x"] for r in rows], dtype=np.float32)
        arrays[split + "_y"] = np.array([r["y"] for r in rows], dtype=np.float32)
        for field in ("identity", "group", "map", "recording"):
            arrays[split + "_" + field] = np.array([r[field] for r in rows])
        manifests[split] = dict(eligible=pool.seen, retained=len(rows),
                                groups=len({r["group"] for r in rows}),
                                maps=dict(Counter(r["map"] for r in rows)))
    out.mkdir(parents=True)
    np.savez_compressed(out / "samples.npz", **arrays)
    manifest = dict(schema=1, feature_version=VERSION, features=FEATURES, targets=TARGETS,
                    horizon_ms=300, donor=donor, donor_aliases=sorted(aliases_seen),
                    combat_sha256=fingerprint, groups_sha256=sha(groups_path),
                    samples_sha256=sha(out / "samples.npz"), source_run=summary["source_run"],
                    combat=str(combat.resolve()), groups=str(groups_path.resolve()),
                    counts=dict(counts), splits=manifests,
                    transform_sha256=sha(Path(__file__)),
                    runtime_qualified=False, split_identity_certified=False,
                    target_semantics="recorded velocity after 300ms in CURRENT view frame; not commands or tactics")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def load_dataset(path):
    path = Path(path)
    manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
    if manifest["feature_version"] != VERSION or sha(path / "samples.npz") != manifest["samples_sha256"]:
        raise ValueError("Incompatible or modified dataset")
    with np.load(path / "samples.npz", allow_pickle=False) as f:
        arrays = {k: f[k] for k in f.files}
    return arrays, manifest
