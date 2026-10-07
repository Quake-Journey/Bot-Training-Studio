"""Past-only map-conditioned sequences; targets are observations, not optimal actions."""
from collections import Counter, deque
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

from .data import FEATURES, WEAPONS, Reservoir, donor_match, features, local, sha, valid_transition
from .learning import atomic_json
from .maps import inspect

VERSION = "observed-game-sequence-v1"
LANDMARKS = ("weapon_railgun", "weapon_chaingun", "weapon_rocketlauncher", "weapon_supershotgun",
             "weapon_hyperblaster", "weapon_grenadelauncher", "item_armor_body", "item_armor_combat",
             "item_armor_jacket", "item_health_mega", "item_adrenaline", "item_health", "item_pack", "ammo")
EXTRA = ["ray_clear", "ray_known", "enemy_in_horizontal_fov", "eye_known", "rockets_observed", "grenades_observed"]
INPUTS = FEATURES + EXTRA + [f"{item}_{axis}" for item in LANDMARKS for axis in ("forward", "left", "up", "exists")]
CONTINUOUS = ["velocity_forward_300ms", "velocity_left_300ms", "velocity_up_300ms",
              "displacement_forward_3s", "displacement_left_3s", "displacement_up_3s",
              "health_change_3s", "armor_change_3s"]


def landmark_positions(world):
    result = {}
    for name in LANDMARKS:
        result[name] = np.asarray([item["origin"] for item in world["items"] if
            (item["classname"].startswith("ammo_") if name == "ammo" else item["classname"] == name)
            and not item["spawnflags"] & 2048], dtype=np.float32).reshape(-1, 3)
    return result


def encode(history, positions):
    base = np.asarray([features(previous, row) + [float(bool(row.get("center_ray_clear"))),
        float(bool(row.get("collision_complete"))), float(bool(row.get("horizontal_fov"))),
        float(bool(row.get("eye_recorded"))), min(20, row.get("packet_rockets") or 0) / 20,
        min(20, row.get("packet_grenades") or 0) / 20] for previous, row in zip(history, history[1:])], dtype=np.float32)
    origins = np.asarray([r["origin"] for r in history[1:]], dtype=np.float32)
    yaw = np.radians([r["view"][1] for r in history[1:]])
    chunks = [base]
    for name in LANDMARKS:
        points = positions[name]
        encoded = np.zeros((len(origins), 4), dtype=np.float32)
        if len(points):
            offsets = points[None] - origins[:, None]
            nearest = np.argmin((offsets * offsets).sum(axis=2), axis=1)
            delta = offsets[np.arange(len(origins)), nearest] / 1280
            encoded[:, 0] = delta[:, 0] * np.cos(yaw) + delta[:, 1] * np.sin(yaw)
            encoded[:, 1] = -delta[:, 0] * np.sin(yaw) + delta[:, 1] * np.cos(yaw)
            encoded[:, 2] = delta[:, 2]; encoded[:, 3] = 1
        chunks.append(encoded)
    return np.concatenate(chunks, axis=1)


def prepare(sources, out, context=16, limit=2000, donor=None, event=None, cancelled=None):
    """sources contain combat/groups/bsp; whole opponent pair assignments survive updates."""
    if context not in (4, 8, 16, 32, 64, 128) or not 100 <= limit <= 24000:
        raise ValueError("Unsupported history or sample cap")
    out = Path(out)
    if out.exists():
        raise ValueError("Use a new dataset generation")
    pools = {s: Reservoir(limit) for s in ("train", "validation", "test")}
    counts, provenance, assignments = Counter(), [], {}
    columns = ["recording_id", "segment", "epoch_id", "slot", "time_ms", "seq", "map", "aliases",
        "opponent_aliases", "velocity", "origin", "view", "health", "armor", "weapon", "grounded",
        "packet_enemy_origin", "yaw_rate", "continuous", "center_ray_clear", "collision_complete",
        "horizontal_fov", "eye_recorded", "packet_rockets", "packet_grenades"]
    for source_index, source in enumerate(sources):
        combat, groupfile, bsp = (Path(source[k]) for k in ("combat", "groups", "bsp"))
        world = inspect(bsp, source.get("map")); positions = landmark_positions(world)
        document = json.loads(groupfile.read_text(encoding="utf8"))
        groups = {(r["recording_id"], r["segment"], r["epoch_id"]): r for r in document["groups"]}
        source_summary = json.loads((combat.parent / "summary.json").read_text(encoding="utf8"))
        if source_summary["source_run"] != document["source_run"]:
            raise ValueError("Recording/group provenance mismatch")
        for group in groups.values():
            if group["split"] == "quarantine":
                continue
            key = group["group"]
            if key in assignments and assignments[key] != group["split"]:
                raise ValueError("Whole match group leaks across partitions")
            assignments[key] = group["split"]
        before = combat.stat()
        provenance.append(dict(map=world["map"], combat_sha256=sha(combat), groups_sha256=sha(groupfile),
                               bsp_sha256=world["bsp_sha256"], source_run=document["source_run"]))
        buffers, last_recording = {}, None
        for batch in pq.ParquetFile(combat).iter_batches(batch_size=4096, columns=columns):
            if cancelled and cancelled():
                raise InterruptedError("Sequence preparation cancelled")
            if event:
                event(dict(stage="sequences", source=source_index + 1, sources=len(sources), rows=counts["rows"]))
            for r in batch.to_pylist():
                counts["rows"] += 1
                if r["map"] != world["map"]:
                    counts["other_map"] += 1; continue
                key = (r["recording_id"], r["segment"], r["epoch_id"])
                group = groups.get(key)
                if not group or group["split"] not in pools:
                    counts["quarantined"] += 1; continue
                if donor and (not donor_match(r["aliases"], donor) or donor_match(r["opponent_aliases"], donor)):
                    continue
                if last_recording != r["recording_id"]:
                    buffers.clear(); last_recording = r["recording_id"]
                track = key + (r["slot"],)
                q = buffers.setdefault(track, deque(maxlen=context + 31))
                if q and not valid_transition(q[-1], r):
                    q.clear(); counts["discontinuity"] += 1
                q.append(r)
                # Take one candidate every 0.5 seconds, never shuffled frames.
                if len(q) != q.maxlen or r["time_ms"] % 500:
                    continue
                history = list(q)
                current, soon, future = history[context], history[context + 3], history[-1]
                if not 2950 <= future["time_ms"] - current["time_ms"] <= 3050:
                    continue
                identity = ":".join(map(str, track + (current["time_ms"],)))
                pool = pools[group["split"]]
                rank = int(hashlib.sha256(identity.encode()).hexdigest(), 16)
                if len(pool.heap) >= limit and rank > -pool.heap[0][0]:
                    counts["reservoir_skipped"] += 1; continue
                x = encode(history[:context + 1], positions)
                y = local(soon["velocity"], current["view"][1])
                delta = [b-a for a,b in zip(current["origin"], future["origin"])]
                y += [v / 4 for v in local(delta, current["view"][1])]
                y += [(future["health"] - current["health"]) / 100, (future["armor"] - current["armor"]) / 200]
                near = [(float(np.linalg.norm(p - np.asarray(future["origin"]), axis=1).min()), i)
                        for i, p in enumerate(positions.values()) if len(p)]
                nearest = min(near) if near else (math.inf, len(LANDMARKS))
                landmark = nearest[1] if nearest[0] <= 128 else len(LANDMARKS)
                if not np.isfinite(x).all() or not np.isfinite(y).all() or abs(x).max() > 30:
                    counts["outlier"] += 1; continue
                pool.add(identity, dict(x=x, y=y, weapon=WEAPONS.index(soon["weapon"]) if soon["weapon"] in WEAPONS else -1,
                    landmark=landmark, identity=identity, group=group["group"], map=world["map"], recording=r["recording_id"]))
        after = combat.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("Input changed during preparation")
    arrays, splits = {}, {}
    for split, pool in pools.items():
        rows = pool.rows()
        if not rows:
            raise ValueError("No eligible " + split + " matches; add independent recordings")
        for field in ("x", "y", "weapon", "landmark", "identity", "group", "map", "recording"):
            dtype = np.float32 if field in ("x", "y") else np.int64 if field in ("weapon", "landmark") else str
            arrays[split + "_" + field] = np.asarray([r[field] for r in rows], dtype=dtype)
        splits[split] = dict(samples=len(rows), groups=len({r["group"] for r in rows}), maps=dict(Counter(r["map"] for r in rows)))
    out.mkdir(parents=True)
    np.savez_compressed(out / "samples.npz", **arrays)
    manifest = dict(schema=1, feature_version=VERSION, inputs=INPUTS, continuous_targets=CONTINUOUS,
        weapon_labels=WEAPONS, landmark_labels=LANDMARKS + ("none_nearby",), context=context, donor=donor,
        sources=provenance, splits=splits, counts=dict(counts), samples_sha256=sha(out / "samples.npz"),
        runtime_qualified=False, semantics="Observed future movement, weapon and proximity; not optimal commands or confirmed pickups")
    atomic_json(out / "manifest.json", manifest)
    return manifest


def load(folder):
    folder = Path(folder)
    meta = json.loads((folder / "manifest.json").read_text(encoding="utf8"))
    if meta["feature_version"] != VERSION or sha(folder / "samples.npz") != meta["samples_sha256"]:
        raise ValueError("Sequence dataset integrity or schema mismatch")
    with np.load(folder / "samples.npz", allow_pickle=False) as raw:
        data = {key: raw[key] for key in raw.files}
    seen = {}
    for split in ("train", "validation", "test"):
        x, y = data[split + "_x"], data[split + "_y"]
        if x.shape[1:] != (meta["context"], len(INPUTS)) or y.shape != (len(x), len(CONTINUOUS)) or not np.isfinite(x).all() or not np.isfinite(y).all():
            raise ValueError("Invalid sequence tensors")
        for group in set(data[split + "_group"]):
            if group in seen and seen[group] != split:
                raise ValueError("Sequence split leakage")
            seen[group] = split
    return data, meta
