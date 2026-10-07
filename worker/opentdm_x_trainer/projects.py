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
                    archive = rar_archive(candidate)
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


def rar_archive(path):
    """Use an installed extractor even when WinRAR is not on PATH."""
    import rarfile
    if not shutil.which(rarfile.UNRAR_TOOL):
        for key in ('ProgramFiles', 'ProgramFiles(x86)'):
            base = os.environ.get(key)
            candidate = Path(base)/'WinRAR'/'UnRAR.exe' if base else None
            if candidate and candidate.is_file():
                rarfile.UNRAR_TOOL = str(candidate); break
    return rarfile.RarFile(path)


def import_sources(old, rows, pipeline, mode):
    """A new decoder/transport cannot certify observations from an old cache."""
    refresh = bool(old and mode == 'update' and old.get('decode_pipeline') != pipeline)
    candidates = list(rows)
    if refresh:
        candidates = [r['source'] for r in old.get('recordings', [])] + \
                     [r['source'] for r in old.get('issues', [])] + candidates
    unique = {}
    for row in candidates:
        unique[(str(Path(row['path']).resolve()), row.get('member'))] = row
    return list(unique.values()), refresh


def reuse_observations(project,old,records,signature,contexts,shots,counts,per_record):
    """Copy only unchanged, hash-verified per-record observations; no geometry replay."""
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq
    if not old or old.get('observation_signature')!=signature:return set()
    folder=Path(project)/'revisions'/old['active_revision']
    prior={r['recording_id']:r for r in old['recordings']}
    reused={key for key,row in records.items() if key in prior and
            row['path']==prior[key]['path'] and key in old.get('record_counts',{})}
    if not reused:return set()
    for name,writer in (('combat.parquet',contexts),('shots.parquet',shots)):
        if sha(folder/name)!=old.get('observation_files',{}).get(name):
            raise ValueError('Observation cache integrity failure')
        writer.flush()
        for batch in pq.ParquetFile(folder/name).iter_batches(batch_size=4096):
            selected=batch.filter(pc.is_in(batch.column('recording_id'),value_set=pa.array(sorted(reused))))
            if selected.num_rows:
                writer.writer.write_batch(selected);writer.rows+=selected.num_rows
    for key in reused:
        per_record[key]=old['record_counts'][key];counts.update(per_record[key])
    return reused


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
        archive = rar_archive(path)
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
    from bts_analysis.items import VERSION as PICKUP_VERSION, PACKET_VERSION
    if mode not in ("fresh", "update"):
        raise ValueError("Choose fresh or update")
    project = Path(project).resolve()
    bsp = Path(bsp).resolve(strict=True)
    tools = native_home()
    decoder = tools / ("decoder.exe" if os.name == "nt" else "decoder")
    physics = tools / ("physics.dll" if os.name == "nt" else "physics.so")
    decoder_sha,physics_sha=sha(decoder),sha(physics)
    dependencies=Path(__file__).resolve().parents[1]/'bts_analysis'
    transform_hashes={name:sha(dependencies/name) for name in ('combat.py','items.py','analyzer.py','motion.py')}
    pipeline = decoder_sha + transform_hashes['analyzer.py']
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
        rows = selected if selected is not None else inventory(inputs, world["map"])
        rows, refresh = import_sources(old, rows, pipeline, mode)
        recordings = {r["recording_id"]: r for r in old.get("recordings", [])} if old and mode == "update" and not refresh else {}
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
        selected_keys = {(r['path'], r.get('member')) for r in rows}
        issues = [r for r in old.get('issues', []) if
                  (r['source']['path'], r['source'].get('member')) not in selected_keys] if old and mode == 'update' else []
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
                    outcome = result.get('results', {}).get('decode_result', {})
                    reason = result.get('error') or ("wrong map" if world['map'] not in result['maps'] else "decoder rejected recording")
                    issues.append(dict(source=row, reason=reason, maps=result['maps'],
                                       return_code=outcome.get('return_code'), quality=outcome.get('quality'),
                                       diagnostic_path=str(directory/'decoder.log')))
                    continue
                recordings[fingerprint] = dict(recording_id=fingerprint, path=str(directory), status="decoded", source=row,
                                               role=row.get("role", "game"), maps=result["maps"], decode_pipeline=pipeline)
        snapshot = dict(schema=1, run_id=revision, records=list(recordings.values()))
        atomic_json(out / "current.json", snapshot)
        group_rows, group_counts = groups(snapshot, out)
        atomic_json(out / "groups.json", dict(schema=1, source_run=revision, groups=group_rows, counts=dict(group_counts)))
        collision = Collision(physics, cached_bsp)
        contexts = TableWriter(out / "combat.parquet", SCHEMA)
        shots = TableWriter(out / "shots.parquet", SHOT_SCHEMA)
        counts, samples = Counter(), defaultdict(list)
        observation_signature=dict(version='record-reuse-v1',bsp=world['bsp_sha256'],physics=physics_sha,sources=transform_hashes)
        per_record={}
        try:
            reused=reuse_observations(project,old,recordings,observation_signature,contexts,shots,counts,per_record) if mode=='update' else set()
            for i, row in enumerate(recordings.values()):
                report(stage="geometry", progress=.55 + .4 * i / max(1, len(recordings)), recording=i + 1, total=len(recordings),reused=len(reused))
                if row['recording_id'] in reused:continue
                record_counts=Counter()
                prepare_record(Path(row["path"]), row["recording_id"], world["map"], collision, contexts, shots, record_counts, samples)
                per_record[row['recording_id']]=dict(record_counts);counts.update(record_counts)
        finally:
            contexts.close(); shots.close(); collision.close()
        atomic_json(out / "summary.json", dict(schema=1, source_run=revision, counts=dict(counts), bsp_sha256=world["bsp_sha256"],
            observation_version=OBSERVATION_VERSION, pickup_version=PICKUP_VERSION, packet_item_version=PACKET_VERSION,
            item_transform_sha256=sha(Path(__file__).resolve().parents[1] / 'bts_analysis' / 'items.py'),
            transform_sha256=sha(Path(__file__).resolve().parents[1] / 'bts_analysis' / 'combat.py')))
        report(stage="committing", progress=.98)
        if (sha(decoder)!=decoder_sha or sha(physics)!=physics_sha or
                any(sha(dependencies/name)!=digest for name,digest in transform_hashes.items())):
            raise ValueError('Analysis tools changed during import; previous project revision preserved')
        result = dict(schema=1, map=world["map"], bsp_sha256=world["bsp_sha256"], active_revision=revision,
                      recordings=list(recordings.values()), counts=dict(counts), groups=dict(group_counts), issues=issues,
                      observation_signature=observation_signature,record_counts=per_record,
                      observation_files={name:sha(out/name) for name in ('combat.parquet','shots.parquet')},
                      native_decoder_sha256=decoder_sha, physics_sha256=physics_sha, decode_pipeline=pipeline, runtime_qualified=False)
        atomic_json(out / "project.json", result)
        atomic_json(project / "project.json", result)
        return dict(project=str(project), revision=revision, map=world["map"], recordings=len(recordings),
                    contexts=counts["contexts"], groups=dict(group_counts), issues=len(issues), runtime_qualified=False)
