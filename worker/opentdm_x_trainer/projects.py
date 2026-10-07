"""Transactional local map projects with incremental content-addressed decoding."""
from collections import Counter, defaultdict
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import uuid
import zipfile

from .data import sha
from .learning import atomic_json, writer_lock
from .maps import inspect


def native_home():
    candidates = [Path(os.environ["BTS_NATIVE"])] if "BTS_NATIVE" in os.environ else []
    candidates += [Path(__file__).resolve().parents[1] / "native", Path(__file__).resolve().parents[2] / "dist" / "native"]
    for folder in candidates:
        if (folder / ("decoder.exe" if os.name == "nt" else "decoder")).is_file():
            return folder
    raise FileNotFoundError("Bundled native decoder/physics component is missing")


def inventory(paths, map_hint=None):
    """List archives without copying/expanding them. Filename map hints are only hints."""
    result = []
    for raw in paths:
        path = Path(raw).resolve(strict=True)
        candidates = sorted(path.rglob("*")) if path.is_dir() else [path]
        for candidate in candidates:
            if not candidate.is_file() or candidate.is_symlink():
                continue
            suffix = candidate.suffix.lower()
            if suffix in (".dm2", ".mvd2"):
                result.append(dict(path=str(candidate), member=None, bytes=candidate.stat().st_size))
            elif suffix in (".zip", ".rar"):
                if suffix == ".zip":
                    archive = zipfile.ZipFile(candidate)
                else:
                    import rarfile
                    archive = rarfile.RarFile(candidate)
                with archive:
                    if len(archive.infolist()) > 100000:
                        raise ValueError("Archive directory exceeds limit")
                    for item in archive.infolist():
                        if Path(item.filename).suffix.lower() in (".dm2", ".mvd2"):
                            result.append(dict(path=str(candidate), member=item.filename, bytes=item.file_size))
            if len(result) > 100000:
                raise ValueError("Input inventory exceeds limit; narrow the selection")
    if map_hint:
        for row in result:
            row["map_name_hint"] = map_hint.casefold() in (row["member"] or row["path"]).casefold()
    return result


@contextlib.contextmanager
def materialize(row, scratch):
    path = Path(row["path"])
    if not 0 < row["bytes"] <= 512 * 1024**2:
        raise ValueError("Recording exceeds 512 MiB bound")
    if not row.get("member"):
        yield path
        return
    # The archive member name is never used as a filesystem path.
    if path.suffix.lower() == ".zip":
        archive = zipfile.ZipFile(path)
    else:
        import rarfile
        archive = rarfile.RarFile(path)
    with archive, tempfile.TemporaryDirectory(prefix="member-", dir=scratch) as tmp:
        target = Path(tmp) / ("input" + Path(row["member"]).suffix.lower())
        total = 0
        with archive.open(row["member"]) as source, target.open("wb") as output:
            while data := source.read(1024**2):
                total += len(data)
                if total > row["bytes"] or total > 512 * 1024**2:
                    raise ValueError("Archive size declaration exceeded")
                output.write(data)
        if total != row["bytes"]:
            raise ValueError("Truncated archive member")
        yield target


def import_project(bsp, inputs, project, mode="update", event=None, cancelled=None, selected=None):
    from bts_analysis.analyzer import decode, TableWriter
    from bts_analysis.combat import Collision, SCHEMA, SHOT_SCHEMA, prepare_record, OBSERVATION_VERSION
    from bts_analysis.group_duels import groups
    if mode not in ("fresh", "update"):
        raise ValueError("Choose fresh or update")
    project = Path(project).resolve()
    bsp = Path(bsp).resolve(strict=True)
    tools = native_home()
    decoder = tools / ("decoder.exe" if os.name == "nt" else "decoder")
    physics = tools / ("physics.dll" if os.name == "nt" else "physics.so")
    pipeline = sha(decoder) + sha(Path(__file__).resolve().parents[1] / "bts_analysis" / "analyzer.py")
    def report(**fields):
        if cancelled and cancelled():
            raise InterruptedError("Project import cancelled; previous revision preserved")
        if event:
            event(fields)
    with writer_lock(project):
        old = json.loads((project / "project.json").read_text()) if (project / "project.json").exists() else None
        world = inspect(bsp, old['map'] if old and bsp == project / 'map.bsp' else None)
        if old and old["bsp_sha256"] != world["bsp_sha256"]:
            raise ValueError("BSP differs from this project; create another project")
        recordings = {r["recording_id"]: r for r in old.get("recordings", [])} if old and mode == "update" else {}
        revision = "revision-" + uuid.uuid4().hex
        out = project / "revisions" / revision
        out.mkdir(parents=True)
        scratch = project / "scratch"; scratch.mkdir(exist_ok=True)
        cached_bsp = project / "map.bsp"
        if not cached_bsp.exists():
            shutil.copyfile(bsp, cached_bsp)
        if sha(cached_bsp) != world["bsp_sha256"]:
            raise ValueError("Cached BSP integrity failure")
        atomic_json(out / "map.json", world)
        rows = selected if selected is not None else inventory(inputs, world["map"])
        issues = []
        for i, row in enumerate(rows):
            report(stage="decode", progress=.05 + .5 * i / max(1, len(rows)), recording=i + 1, total=len(rows))
            if shutil.disk_usage(project).free < 2 * 1024**3:
                raise OSError("Less than 2 GiB free; import paused before exhausting disk")
            with materialize(row, scratch) as path:
                before = path.stat()
                fingerprint = sha(path)
                identity = hashlib.sha256((fingerprint + pipeline).encode()).hexdigest()
                directory = project / "records" / identity
                if not (directory / "manifest.json").exists():
                    directory.mkdir(parents=True, exist_ok=True)
                    result = decode(decoder, path, directory, fingerprint, [world["map"]], 240, cancelled=cancelled)
                else:
                    result = json.loads((directory / "manifest.json").read_text())
                    for name, check in result.get("artifacts", {}).items():
                        if Path(name).name != name or sha(directory / name) != check["sha256"]:
                            raise ValueError("Decoded recording cache integrity failure")
                after = path.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise ValueError("Recording changed while decoding")
                if result["status"] != "decoded" or world["map"] not in result["maps"]:
                    issues.append(dict(source=row, reason=result.get("error") or "wrong map or unsupported recording"))
                    continue
                recordings[fingerprint] = dict(recording_id=fingerprint, path=str(directory), status="decoded", source=row,
                                               role=row.get("role", "game"), maps=result["maps"])
        snapshot = dict(schema=1, run_id=revision, records=list(recordings.values()))
        atomic_json(out / "current.json", snapshot)
        group_rows, group_counts = groups(snapshot, out)
        atomic_json(out / "groups.json", dict(schema=1, source_run=revision, groups=group_rows, counts=dict(group_counts)))
        collision = Collision(physics, cached_bsp)
        contexts = TableWriter(out / "combat.parquet", SCHEMA)
        shots = TableWriter(out / "shots.parquet", SHOT_SCHEMA)
        counts, samples = Counter(), defaultdict(list)
        try:
            for i, row in enumerate(recordings.values()):
                report(stage="geometry", progress=.55 + .4 * i / max(1, len(recordings)), recording=i + 1, total=len(recordings))
                prepare_record(Path(row["path"]), row["recording_id"], world["map"], collision, contexts, shots, counts, samples)
        finally:
            contexts.close(); shots.close(); collision.close()
        atomic_json(out / "summary.json", dict(schema=1, source_run=revision, counts=dict(counts), bsp_sha256=world["bsp_sha256"],
            observation_version=OBSERVATION_VERSION, transform_sha256=sha(Path(__file__).resolve().parents[1] / 'bts_analysis' / 'combat.py')))
        report(stage="committing", progress=.98)
        result = dict(schema=1, map=world["map"], bsp_sha256=world["bsp_sha256"], active_revision=revision,
                      recordings=list(recordings.values()), counts=dict(counts), groups=dict(group_counts), issues=issues,
                      native_decoder_sha256=sha(decoder), physics_sha256=sha(physics), runtime_qualified=False)
        atomic_json(out / "project.json", result)
        atomic_json(project / "project.json", result)
        return dict(project=str(project), revision=revision, map=world["map"], recordings=len(recordings),
                    contexts=counts["contexts"], groups=dict(group_counts), issues=len(issues), runtime_qualified=False)
