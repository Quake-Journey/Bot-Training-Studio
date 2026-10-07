"""Offline demo preparation. No AI API calls, fixed corpus list or game process.

Read a changing input tree on each pass; stream ZIP/RAR members, reuse results
by content + decoder/transform identity, publish SQLite/Parquet and HTML results.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from contextlib import contextmanager
import hashlib
import html
import inspect
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import zipfile

import pyarrow as pa
import pyarrow.parquet as pq
import rarfile

SCHEMA_VERSION = 1
DEMO_EXTENSIONS = {'.dm2', '.mvd2'}
ARCHIVE_EXTENSIONS = {'.zip', '.rar'}
CHUNK_ROWS = 4096
MAX_RECORD_BYTES = 8 * 1024 * 1024  # bounded full entity snapshot, including extended protocols
BASE = [('recording_id', pa.string()), ('seq', pa.int64()),
        ('segment', pa.int32()), ('time_ms', pa.int64())]
STATE_SCHEMA = pa.schema(BASE + [
    ('slot', pa.int32()), ('pov', pa.bool_()), ('pm_type', pa.int32()),
    ('pm_flags', pa.int32()), ('pm_time', pa.int32()), ('gravity', pa.int32()), ('gunindex',pa.int32()), ('gunframe',pa.int32()),
    ('origin', pa.list_(pa.float32())), ('velocity', pa.list_(pa.float32())),
    ('view_angles', pa.list_(pa.float32())), ('delta_angles', pa.list_(pa.float32())),
    ('view_offset',pa.list_(pa.float32())),('kick_angles',pa.list_(pa.float32())),('fov',pa.float32()),
    ('stats', pa.list_(pa.int16()))])
ENTITY_SCHEMA = pa.schema(BASE + [('frame_number',pa.int32()),('delta_frame',pa.int32()),('mvd',pa.bool_()),
    ('entities',pa.list_(pa.struct([
        ('number',pa.int32()),('origin',pa.list_(pa.float32())),('old_origin',pa.list_(pa.float32())),
        ('angles',pa.list_(pa.float32())),('models',pa.list_(pa.int32())),
        *[(k,pa.int32()) for k in ('frame','skin','sound','event','solid')],
        *[(k,pa.uint32()) for k in ('effects','renderfx','morefx')],
        ('loop_volume',pa.float32()),('loop_attenuation',pa.float32())
    ])))])
EVENT_SCHEMA = pa.schema(BASE + [('kind', pa.string()), ('payload', pa.string())])
FEATURE_SCHEMA = pa.schema(BASE + [
    ('slot', pa.int32()), ('track_id', pa.int32()), ('epoch_id', pa.int32()),
    ('map', pa.string()), ('mode', pa.int32()), ('mode_conf', pa.int32()),
    ('live_evidence', pa.bool_()), ('actor_slot_attributed', pa.bool_()),
    ('x', pa.float32()), ('y', pa.float32()), ('z', pa.float32()),
    ('vx', pa.float32()), ('vy', pa.float32()), ('vz', pa.float32()),
    ('speed_xy', pa.float32()), ('yaw_sin', pa.float32()), ('yaw_cos', pa.float32()),
    ('health_observed', pa.int32()), ('armor_observed', pa.int32()),
    ('ammo_observed', pa.int32()), ('frags_observed', pa.int32()),
    ('spectator_flag', pa.int32()), ('chase_stat', pa.int32()),
    ('delta_ms', pa.int32()), ('continuous_motion', pa.bool_()),
    ('enemy_state_available', pa.bool_()), ('original_usercmd_available', pa.bool_()),
    ('training_ready', pa.bool_())])


def canonical(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':'), allow_nan=False)


def io_path(path):
    """Long-path Windows I/O without changing machine registry/user settings."""
    value=os.path.abspath(os.fspath(path))
    if os.name=='nt' and not value.startswith('\\\\?\\'):
        value='\\\\?\\UNC\\'+value[2:] if value.startswith('\\\\') else '\\\\?\\'+value
    return Path(value)


def display_path(path):
    value=os.fspath(path)
    if value.startswith('\\\\?\\UNC\\'):value='\\\\'+value[8:]
    elif value.startswith('\\\\?\\'):value=value[4:]
    return Path(value)


def digest_file(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def atomic_json(path, value):
    # Our publication scratch only; never an input path or PO evidence.
    part = path.with_name(path.name + '.new')
    with part.open('w', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(value, ensure_ascii=True, indent=2, allow_nan=False) + '\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(part, path)


@contextmanager
def single_writer(path):
    with path.open('a+b') as f:
        f.seek(0)
        if not f.read(1):
            f.write(b'0'); f.flush()
        f.seek(0)
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            f.seek(0)
            if os.name == 'nt':
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(f, fcntl.LOCK_UN)


class TableWriter:
    def __init__(self, path, schema, chunk_rows=CHUNK_ROWS):
        self.path, self.schema = path, schema
        self.chunk_rows=chunk_rows
        self.writer = pq.ParquetWriter(path, schema, compression='zstd')
        self.rows, self.buffer = 0, []

    def add(self, row):
        self.buffer.append(row)
        if len(self.buffer) >= self.chunk_rows:
            self.flush()

    def flush(self):
        if self.buffer:
            self.writer.write_table(pa.Table.from_pylist(self.buffer, schema=self.schema))
            self.rows += len(self.buffer)
            self.buffer.clear()

    def close(self):
        self.flush()
        self.writer.close()


def prepare_features(states_path, destination, recording_id, facts, maps):
    """Actor observations only. No action labels, future outcome or enemy truth.

    These are inspectable candidate features, deliberately not a validated train
    split. Unknown/chase actor attribution remains visible and cannot be silently
    treated as the named recorder's play.
    """
    segments = {r['segment']: r for r in facts if r['kind'] == 'segment_facts'}
    tracks = defaultdict(list)
    epochs = defaultdict(list)
    for r in facts:
        if r['kind'] == 'track': tracks[(r['segment'], r['slot'])].append(r)
        if r['kind'] == 'epoch': epochs[r['segment']].append(r)
    writer = TableWriter(destination, FEATURE_SCHEMA)
    previous = {}
    try:
        for batch in pq.ParquetFile(states_path).iter_batches(batch_size=CHUNK_ROWS):
            for s in batch.to_pylist():
                seg = segments.get(s['segment'], {})
                if maps and seg.get('map') not in maps: continue
                t, slot = s['time_ms'], s['slot']
                stats = s['stats']
                def stat(i): return stats[i] if len(stats) > i else None
                possible = [r for r in tracks[(s['segment'], slot)]
                            if r['time_ms'] <= t <= r['end_ms']
                            and (not r['identity_end_ms'] or t < r['identity_end_ms'])]
                track = possible[0] if len(possible) == 1 else {}
                live = [r for r in epochs[s['segment']]
                        if r['time_ms'] <= t <= r['end_ms'] and r['start_conf'] == 2]
                epoch = live[0] if len(live) == 1 else {}
                key = (s['segment'], slot)
                old = previous.get(key)
                dt = t - old['time_ms'] if old else None
                valid_motion = (s['pm_type'] == 0 and stat(17) == 0)
                continuous = bool(old and valid_motion and old['pm_type'] == 0
                                  and len(old['stats']) > 17 and old['stats'][17] == 0 and 0 < dt <= 250
                                  and not s['pm_flags'] & 32
                                  and math.dist(s['origin'], old['origin']) <= 1.5 * dt)
                previous[key] = s
                if not valid_motion: continue
                if not all(v is not None and math.isfinite(v)
                           for field in ['origin', 'velocity', 'view_angles'] for v in s[field]):
                    continue
                # Downsample contexts to >= 500 ms apart; preserve every raw state.
                if old and t // 500 == old['time_ms'] // 500: continue
                actor = bool(track and track['active_seen'] and not track['chase_spectator']
                             and stat(17) == 0 and stat(16) == 0)
                yaw = math.radians(s['view_angles'][1])
                row = {k: s[k] for k, _ in BASE}
                row.update(slot=slot, track_id=track.get('track_id'), epoch_id=epoch.get('epoch_id'),
                           map=seg.get('map'), mode=seg.get('mode'), mode_conf=seg.get('mode_conf'),
                           live_evidence=bool(epoch), actor_slot_attributed=actor,
                           x=s['origin'][0], y=s['origin'][1], z=s['origin'][2],
                           vx=s['velocity'][0], vy=s['velocity'][1], vz=s['velocity'][2],
                           speed_xy=math.hypot(*s['velocity'][:2]), yaw_sin=math.sin(yaw), yaw_cos=math.cos(yaw),
                           health_observed=stat(1), armor_observed=stat(5), ammo_observed=stat(3),
                           frags_observed=stat(14), spectator_flag=stat(17), chase_stat=stat(16),
                           delta_ms=dt, continuous_motion=continuous, enemy_state_available=False,
                           original_usercmd_available=False, training_ready=False)
                writer.add(row)
    finally:
        writer.close()
    return writer.rows


def decode(decoder, demo_path, directory, recording_id, maps, timeout, cancelled=None):
    states = TableWriter(directory / 'player_states.parquet', STATE_SCHEMA)
    events = TableWriter(directory / 'events.parquet', EVENT_SCHEMA)
    scene = TableWriter(directory / 'entity_frames.parquet', ENTITY_SCHEMA,chunk_rows=16)
    facts = []
    results = {}
    counts = Counter()
    last_sequence = -1
    error = None
    stderr_path = directory / 'decoder.log'
    with stderr_path.open('wb') as stderr:
        proc = subprocess.Popen([str(decoder), str(demo_path)], stdout=subprocess.PIPE, stderr=stderr)
        timer = threading.Timer(timeout, proc.kill)
        timer.daemon = True
        timer.start()
        stopped = threading.Event()
        def watch_cancel():
            while not stopped.wait(.1):
                if cancelled and cancelled():
                    if proc.poll() is None: proc.kill()
                    return
        watcher = threading.Thread(target=watch_cancel, daemon=True)
        watcher.start()
        try:
            while True:
                if cancelled and cancelled():
                    raise InterruptedError('Cancelled while decoding')
                raw = proc.stdout.readline(MAX_RECORD_BYTES + 1)
                if not raw: break
                if len(raw) > MAX_RECORD_BYTES: raise ValueError('decoder record exceeds bound')
                r = json.loads(raw)
                if r['seq'] <= last_sequence: raise ValueError('decoder sequence is not increasing')
                last_sequence = r['seq']
                kind = r['kind']; counts[kind] += 1
                r['recording_id'] = recording_id
                if kind == 'player_state':
                    r['pov'] = bool(r['pov']); states.add(r)
                elif kind == 'entity_frame':
                    r['mvd']=bool(r['mvd']);scene.add(r)
                else:
                    base = {k: r[k] for k, _ in BASE}
                    events.add(dict(base, kind=kind, payload=canonical(r)))
                    if kind in {'segment_start', 'segment_facts', 'track', 'epoch', 'participant',
                                'weapon_fact', 'pickup_fact'}:
                        facts.append(r)
                    if kind.endswith('_result'): results[kind] = r
            code = proc.wait()
            if code: raise RuntimeError(f'decoder exited {code}; see decoder.log')
            if 'decode_result' not in results: raise ValueError('decoder produced no final result')
        except Exception as exc:
            error = str(exc)
        finally:
            stopped.set()
            watcher.join(timeout=1)
            timer.cancel()
            if proc.poll() is None: proc.kill()
            proc.wait(); proc.stdout.close()
            states.close(); events.close();scene.close()
    result = results.get('decode_result', {})
    if cancelled and cancelled():
        raise InterruptedError('Cancelled while decoding; previous project revision preserved')
    clean = not error and result.get('return_code') == 0 and result.get('quality') == 0
    actual_maps = sorted({r['map'] for r in facts if r['kind'] == 'segment_facts'})
    status = ('decoded' if clean and states.rows else 'rejected')
    manifest = {'schema': SCHEMA_VERSION, 'recording_id': recording_id,
                'status': status, 'error': error, 'maps': actual_maps,
                'states': states.rows, 'events': events.rows, 'scene_frames':scene.rows, 'contexts': 0,
                'counts': dict(counts), 'results': results,
                'limitations': ['packet_presence_is_not_line_of_sight',
                                'mvd_world_is_not_actor_visibility', 'opaque_mod_multicast_payloads_not_interpreted', 'sound_audibility_requires_pov_review',
                                'original_usercmds_not_available', 'actor_identity_requires_review',
                                'match_grouping_and_train_splits_not_assigned',
                                'contexts_are_observations_not_expert_action_labels']}
    atomic_json(directory / 'facts.json', facts)
    manifest['artifacts'] = {p.name: {'bytes': p.stat().st_size, 'sha256': digest_file(p)}
                             for p in sorted(directory.iterdir()) if p.is_file()}
    atomic_json(directory / 'manifest.json', manifest)
    return manifest


class Analyzer:
    def __init__(self, args):
        self.args = args
        self.read_root = io_path(args.input).resolve(strict=True)
        self.root = display_path(self.read_root)
        self.output = io_path(args.output).resolve()
        if self.read_root == self.output or self.read_root in self.output.parents:
            raise ValueError('Output must be outside the input tree')
        args.decoder=io_path(args.decoder).resolve(strict=True)
        self.output.mkdir(parents=True, exist_ok=True)
        self.scratch = self.output / 'scratch'; self.scratch.mkdir(exist_ok=True)
        self.store = self.output / 'records'; self.store.mkdir(exist_ok=True)
        self.views = self.output / 'views'; self.views.mkdir(exist_ok=True)
        self.revisions = self.output / 'revisions'; self.revisions.mkdir(exist_ok=True)
        self.version = hashlib.sha256(canonical({
            'decoder': digest_file(args.decoder), 'transport_schema': SCHEMA_VERSION,
            'transport': hashlib.sha256(inspect.getsource(decode).encode()).hexdigest()}).encode()).hexdigest()
        self.feature_version = hashlib.sha256(canonical({
            'transform': inspect.getsource(prepare_features), 'schema': str(FEATURE_SCHEMA),
            'maps': sorted(args.map or [])}).encode()).hexdigest()
        self.run_id = uuid.uuid4().hex
        self.db = sqlite3.connect(self.output / 'catalog.sqlite3')
        self.db.execute('PRAGMA foreign_keys=ON')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS runs(id TEXT PRIMARY KEY, root TEXT, started REAL, finished REAL, status TEXT);
          CREATE TABLE IF NOT EXISTS records(id TEXT PRIMARY KEY, content_sha256 TEXT, version TEXT, path TEXT, summary TEXT);
          CREATE TABLE IF NOT EXISTS locations(root TEXT, address TEXT, run_id TEXT, record_id TEXT,
            container_sha256 TEXT, PRIMARY KEY(root,address), FOREIGN KEY(record_id) REFERENCES records(id));
          CREATE TABLE IF NOT EXISTS issues(run_id TEXT, address TEXT, error TEXT);
          CREATE TABLE IF NOT EXISTS map_scans(id TEXT PRIMARY KEY, summary TEXT);
        ''')
        old = self.db.execute("SELECT value FROM metadata WHERE key='schema'").fetchone()
        if old and old[0] != str(SCHEMA_VERSION): raise ValueError('Unsupported catalog schema')
        self.db.execute("INSERT OR IGNORE INTO metadata VALUES('schema',?)", (str(SCHEMA_VERSION),))
        self.db.execute('INSERT INTO runs VALUES(?,?,?,NULL,?)',
                        (self.run_id, str(self.root), time.time(), 'running'))
        self.db.commit()
        self.counts = Counter()
        self.issues = []
        self.expanded = 0
        self.members = 0
        self.map_scanner = getattr(args, 'map_scanner', None)
        self.scan_version = digest_file(self.map_scanner) if self.map_scanner else ''
        unrar = args.unrar or (r'C:\Program Files\WinRAR\UnRAR.exe' if os.name == 'nt' else 'unrar')
        rarfile.UNRAR_TOOL = unrar
        # Avoid rarfile's extraction optimization making temporary archive copies.
        rarfile.USE_EXTRACT_HACK = False

    def issue(self, address, error):
        message = str(error)[:2000]
        self.issues.append({'source': address, 'error': message})
        self.db.execute('INSERT INTO issues VALUES(?,?,?)', (self.run_id, address, message))
        self.db.commit()

    def copy_bounded(self, stream, target):
        sha = hashlib.sha256(); size = 0
        while block := stream.read(1024 * 1024):
            size += len(block); self.expanded += len(block)
            if size > self.args.max_member_mib * 1024**2:
                raise ValueError('member exceeds configured size bound')
            if self.expanded > self.args.max_expanded_gib * 1024**3:
                raise ValueError('pass exceeds configured expanded-byte bound')
            sha.update(block); target.write(block)
        return sha.hexdigest(), size

    def visit(self, path, address, suffix, container_hash='', depth=0):
        if depth > self.args.max_depth: raise ValueError('nested archive depth exceeded')
        if suffix in DEMO_EXTENSIONS:
            self.process_demo(path, address, container_hash)
            return
        if suffix not in ARCHIVE_EXTENSIONS: return
        archive_hash = digest_file(path)
        archive_type = zipfile.ZipFile if suffix == '.zip' else rarfile.RarFile
        self.counts['archives_opened'] += 1
        with archive_type(path) as archive:
            entries = archive.infolist()
            if len(entries) > self.args.max_members: raise ValueError('archive member count exceeded')
            for index, entry in enumerate(entries):
                if entry.is_dir(): continue
                name = entry.filename
                child_suffix = Path(name).suffix.lower()
                if child_suffix not in DEMO_EXTENSIONS | ARCHIVE_EXTENSIONS: continue
                self.members += 1
                if self.members > self.args.max_members: raise ValueError('pass member count exceeded')
                child_address = address + '!' + canonical({'index': index, 'name': name})
                try:
                    if entry.file_size > self.args.max_member_mib * 1024**2:
                        raise ValueError('declared archive member size exceeds bound')
                    # Member paths never become filesystem paths; traversal and
                    # identical basenames cannot escape or overwrite another file.
                    with tempfile.TemporaryDirectory(prefix='member-', dir=self.scratch) as work:
                        member_path = Path(work) / ('payload' + child_suffix)
                        with archive.open(entry) as src, member_path.open('wb') as dst:
                            self.copy_bounded(src, dst)
                        self.visit(member_path, child_address, child_suffix, archive_hash, depth + 1)
                except Exception as exc:
                    self.issue(child_address, exc)

    def process_demo(self, path, address, container_hash):
        content_hash = digest_file(path)
        if self.map_scanner:
            identity = content_hash + ':' + self.scan_version
            cached = self.db.execute('SELECT summary FROM map_scans WHERE id=?', (identity,)).fetchone()
            if cached:
                scan = json.loads(cached[0]); self.counts['map_scan_cache_hits'] += 1
            else:
                process = subprocess.run([str(self.map_scanner), str(path), '--maps-only'],
                                         capture_output=True, timeout=self.args.timeout)
                if process.returncode:
                    raise ValueError('recorded-map scanner failed: ' + str(process.returncode))
                scan = json.loads(process.stdout)
                self.db.execute('INSERT INTO map_scans VALUES(?,?)', (identity, canonical(scan)))
                self.db.commit(); self.counts['map_scans'] += 1
            recorded = {m.lower() for m in scan['maps'] if m}
            # Incomplete/unknown scans go through normal decoding and its
            # quality reporting. Folder/member names never establish a map.
            if scan['return_code'] == 0 and not scan['quality'] and recorded and not recorded.intersection(self.args.map):
                self.counts['other_map_recordings_skipped'] += 1
                return
        record_id = hashlib.sha256((content_hash + ':' + self.version).encode()).hexdigest()
        existing = self.db.execute('SELECT path,summary FROM records WHERE id=?', (record_id,)).fetchone()
        if existing:
            directory = self.output / existing[0]
            manifest = json.loads(existing[1])
            for name, info in manifest['artifacts'].items():
                artifact = directory / name
                if not artifact.is_file() or digest_file(artifact) != info['sha256']:
                    raise ValueError('cached artifact missing/corrupt; retained record requires repair')
            self.counts['cache_hits'] += 1
        else:
            directory = self.store / record_id
            # A prior crash may have left a fully published orphan. Never
            # overwrite it with partial output or point SQLite at staging files.
            if (directory / 'manifest.json').exists():
                manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
                for name, info in manifest['artifacts'].items():
                    if digest_file(directory / name) != info['sha256']:
                        raise ValueError('unpublished artifact failed integrity check')
            else:
                with tempfile.TemporaryDirectory(prefix='decode-', dir=self.scratch) as tmp:
                    stage = Path(tmp) / 'result'; stage.mkdir()
                    manifest = decode(self.args.decoder, path, stage, record_id,
                                      set(self.args.map or []), self.args.timeout)
                    os.replace(stage, directory)
                self.counts['new_decodes'] += 1
            self.db.execute('INSERT INTO records VALUES(?,?,?,?,?)',
                            (record_id, content_hash, self.version,
                             directory.relative_to(self.output).as_posix(), canonical(manifest)))
        self.db.execute('INSERT OR REPLACE INTO locations VALUES(?,?,?,?,?)',
                        (str(self.root), address, self.run_id, record_id, container_hash))
        self.db.commit()
        self.counts['demo_locations'] += 1

    def feature_view(self, record_id, directory, summary):
        destination = self.views / (record_id + '-' + self.feature_version)
        manifest_path = destination / 'manifest.json'
        if manifest_path.is_file():
            view = json.loads(manifest_path.read_text(encoding='utf-8'))
            if digest_file(destination / 'contexts.parquet') != view['sha256']:
                raise ValueError('cached context view failed integrity check')
            self.counts['feature_cache_hits'] += 1
            return view
        with tempfile.TemporaryDirectory(prefix='features-', dir=self.scratch) as tmp:
            stage = Path(tmp) / 'view'; stage.mkdir()
            target = stage / 'contexts.parquet'
            if summary['status'] == 'decoded':
                facts = json.loads((directory / 'facts.json').read_text(encoding='utf-8'))
                count = prepare_features(directory / 'player_states.parquet', target,
                                         record_id, facts, set(self.args.map or []))
            else:
                writer = TableWriter(target, FEATURE_SCHEMA); writer.close(); count = 0
            view = {'recording_id': record_id, 'feature_version': self.feature_version,
                    'path': destination.relative_to(self.output).as_posix(),
                    'contexts': count, 'sha256': digest_file(target), 'training_ready': False}
            atomic_json(stage / 'manifest.json', view)
            os.replace(stage, destination)
        self.counts['new_feature_views'] += 1
        return view

    def run(self):
        # Enumerate the live directory on every pass. A changing file is retried
        # on the next pass and is not accepted under an unstable source identity.
        for path in sorted(self.read_root.rglob('*')):
            if not path.is_file() or path.is_symlink(): continue
            suffix = path.suffix.lower()
            if suffix not in DEMO_EXTENSIONS | ARCHIVE_EXTENSIONS: continue
            address = path.relative_to(self.read_root).as_posix()
            try:
                before = path.stat()
                if before.st_size > self.args.max_member_mib * 1024**2:
                    raise ValueError('input exceeds configured file bound')
                with tempfile.TemporaryDirectory(prefix='source-', dir=self.scratch) as tmp:
                    snapshot = Path(tmp) / ('source' + suffix)
                    with path.open('rb') as src, snapshot.open('wb') as dst:
                        self.copy_bounded(src, dst)
                    after = path.stat()
                    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                        raise ValueError('source changed while being read; retry next pass')
                    self.visit(snapshot, address, suffix)
                self.counts['source_files'] += 1
            except Exception as exc:
                self.issue(address, exc)
        rows = self.db.execute('''SELECT DISTINCT r.id,r.path,r.summary FROM records r JOIN locations l
                                ON l.record_id=r.id WHERE l.root=? AND l.run_id=?''',
                               (str(self.root), self.run_id)).fetchall()
        status = Counter(); totals = Counter(); records = []; aliases = Counter(); map_coverage = Counter()
        for identity, path, raw in rows:
            summary = json.loads(raw); status[summary['status']] += 1
            map_coverage.update(summary['maps'])
            view = self.feature_view(identity, self.output / path, summary)
            summary['contexts'] = view['contexts']
            for key in ['states', 'events', 'contexts','scene_frames']: totals[key] += summary.get(key,0)
            records.append({'recording_id': identity, 'path': path, 'status': summary['status'],
                            'context_view': view['path'], 'contexts': view['contexts']})
            for fact in json.loads((self.output / path / 'facts.json').read_text(encoding='utf-8')):
                if fact['kind'] == 'track':
                    for alias in fact['aliases']: aliases[alias] += 1
        report = {'schema': SCHEMA_VERSION, 'run_id': self.run_id, 'input': str(self.root),
                  'pipeline_id': self.version, 'feature_version': self.feature_version,
                  'finished_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                  'counts': dict(self.counts), 'unique_recordings': len(rows), 'decode_status': dict(status),
                  'rows': dict(totals), 'issues': self.issues,
                  'records': sorted(records, key=lambda r: r['recording_id']),
                  'observed_aliases': dict(aliases.most_common()),
                  'recorded_map_coverage': dict(map_coverage),
                  'training_ready': False,
                  'note': 'Prepared observations and candidate contexts; no trained bot, verified expert identity or train split is claimed.'}
        atomic_json(self.revisions / (self.run_id + '.json'), report)
        self.db.execute('UPDATE runs SET finished=?,status=? WHERE id=?',
                        (time.time(), 'complete_with_issues' if self.issues else 'complete', self.run_id))
        self.db.commit()
        atomic_json(self.output / 'current.json', report)
        self.write_html(report, rows)
        print(canonical({k: report[k] for k in ['run_id', 'counts', 'unique_recordings', 'decode_status', 'rows']}), flush=True)
        print(f"Report: {display_path(self.output / 'report.html')}; source errors: {len(self.issues)}", flush=True)
        return report

    def write_html(self, report, rows):
        esc = html.escape
        contexts = {r['recording_id']: r['contexts'] for r in report['records']}
        body = ['<!doctype html><html lang="ru"><meta charset="utf-8"><title>OpenTDM-X Demo Analyzer</title>',
                '<style>body{font:16px system-ui;max-width:1200px;margin:32px auto;padding:0 20px;background:#161b22;color:#e6edf3}',
                'table{border-collapse:collapse;width:100%}td,th{padding:8px;border-bottom:1px solid #38404a;text-align:left}',
                'a{color:#79c0ff}code{overflow-wrap:anywhere}</style><h1>OpenTDM-X Demo Analyzer</h1>',
                '<p>Подготовлены наблюдения из демок. Идентичность игроков, группировка матчей и обучающие выборки требуют проверки.',
                ' Полное состояние противника и исходные нажатия клавиш не восстановлены.</p>',
                '<p>Источник: <code>' + esc(str(self.root)) + '</code></p>',
                '<p>' + esc(canonical(report['rows'])) + '</p>',
                '<table><tr><th>Источник</th><th>Карта</th><th>Статус</th><th>Кадры игрока</th><th>Контексты</th><th>Данные</th></tr>']
        for identity, path, raw in sorted(rows):
            summary = json.loads(raw)
            addresses = [r[0] for r in self.db.execute('SELECT address FROM locations WHERE record_id=? AND root=? AND run_id=?',
                         (identity, str(self.root), self.run_id))]
            body.append('<tr><td>' + '<br>'.join(esc(a) for a in addresses) + '</td><td>' + esc(', '.join(summary['maps'])) +
                        '</td><td>' + esc(summary['status']) + f'</td><td>{summary["states"]}</td><td>{contexts[identity]}</td>' +
                        '<td><a href="' + esc(path + '/facts.json', quote=True) + '">Факты</a> · <a href="' +
                        esc(path + '/manifest.json', quote=True) + '">Метаданные</a></td></tr>')
        body.append('</table><h2>Ошибки чтения</h2><ul>')
        body.extend('<li>' + esc(i['source'] + ': ' + i['error']) + '</li>' for i in self.issues)
        body.append('</ul></html>')
        part = self.output / 'report.html.new'
        part.write_text('\n'.join(body), encoding='utf-8')
        os.replace(part, self.output / 'report.html')


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--decoder', required=True, type=Path)
    ap.add_argument('--map', action='append', help='Filter context export by recorded map; repeatable')
    ap.add_argument('--map-scanner', type=Path, help='Optional native --maps-only decoder: omit complete recordings of other maps before full export')
    ap.add_argument('--unrar', help='UnRAR executable; defaults to installed WinRAR on Windows')
    ap.add_argument('--watch', type=float, default=0, metavar='SECONDS', help='Repeat passes; Ctrl+C stops')
    ap.add_argument('--timeout', type=float, default=240, help='Hard decoder time limit per recording')
    ap.add_argument('--max-member-mib', type=int, default=512)
    ap.add_argument('--max-expanded-gib', type=float, default=2)
    ap.add_argument('--max-members', type=int, default=10000)
    ap.add_argument('--max-depth', type=int, default=4)
    return ap


def main():
    args = parser().parse_args()
    if args.watch < 0 or args.timeout <= 0 or args.max_member_mib <= 0 or args.max_expanded_gib <= 0:
        raise ValueError('Resource/time bounds must be positive; watch may be zero')
    input_root = io_path(args.input).resolve(strict=True)
    output_root = io_path(args.output).resolve()
    if not input_root.is_dir(): raise ValueError('Input must be a directory')
    if input_root == output_root or input_root in output_root.parents:
        raise ValueError('Output must be outside the input tree')
    args.output=output_root
    args.output.mkdir(parents=True, exist_ok=True)
    args.decoder = io_path(args.decoder).resolve(strict=True)
    if args.map_scanner:
        if not args.map: raise ValueError('--map-scanner requires at least one --map')
        args.map_scanner = io_path(args.map_scanner).resolve(strict=True)
        args.map = [m.lower() for m in args.map]
    with single_writer(args.output / '.writer.lock'):
        while True:
            analyzer = Analyzer(args)
            try: analyzer.run()
            finally: analyzer.db.close()
            if not args.watch: break
            time.sleep(max(args.watch, 5))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print('Stopped; committed data retained. Run again to resume.', file=sys.stderr)
        raise SystemExit(130)
