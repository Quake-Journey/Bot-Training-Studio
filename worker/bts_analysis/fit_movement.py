"""Bounded offline inverse movement fitting. Computation never calls an LLM.

Produces possible control witnesses, with measured endpoint errors and source
identity. Does not label them as original usercmds or confirmed rocket jumps.
"""
import argparse
from collections import Counter
import ctypes as c
import hashlib
import json
import math
from pathlib import Path
import time

import pyarrow as pa
import pyarrow.parquet as pq

SCHEMA = pa.schema([
    ('recording_id', pa.string()), ('segment', pa.int32()), ('slot', pa.int32()),
    ('time_ms', pa.int64()), ('seq', pa.int64()), ('epoch', pa.string()),
    ('aliases', pa.list_(pa.string())), ('origin', pa.list_(pa.float32())),
    ('destination', pa.list_(pa.float32())), ('velocity', pa.list_(pa.float32())),
    ('end_velocity', pa.list_(pa.float32())), ('view', pa.list_(pa.float32())),
    ('end_view', pa.list_(pa.float32())), ('flags', pa.int32()),
    ('pm_time', pa.int32()), ('gravity', pa.int32()), ('pm_type', pa.int32()),
    ('forward', pa.int32()), ('side', pa.int32()), ('jump_pattern', pa.int32()),
    ('substep_ms', pa.int32()), ('position_error', pa.float32()),
    ('velocity_error', pa.float32()), ('ground_contacts', pa.int32()),
    ('jump_events', pa.int32()), ('accepted_witness', pa.bool_()),
])


def active(state):
    stats = state['stats']
    return (state['pm_type'] == 0 and len(stats) > 17 and stats[1] > 0 and
            not stats[16] and not stats[17] and not state['pm_flags'] & 32 and
            all(math.isfinite(v) for key in ('origin', 'velocity', 'view_angles') for v in state[key]))


def main(args, event=None, cancelled=None):
    began = time.monotonic()
    snapshot = json.loads((args.data / 'current.json').read_text(encoding='utf-8'))
    if args.stride < 1 or args.max_samples < 0:
        raise ValueError('stride must be positive and max-samples nonnegative')
    args.out.mkdir(parents=True, exist_ok=True)
    lib = c.CDLL(str(args.physics.resolve()))
    lib.OTXF_Open.argtypes = [c.c_void_p, c.c_size_t]
    lib.OTXF_Fit.argtypes = [c.POINTER(c.c_float), c.POINTER(c.c_float), c.POINTER(c.c_int), c.POINTER(c.c_float)]
    raw = args.bsp.read_bytes()
    if not lib.OTXF_Open(raw, len(raw)):
        raise RuntimeError('Cannot bind BSP and initial mover states to Pmove')
    counts = Counter(); rows = []; errors = []
    writer = pq.ParquetWriter(args.out / 'witnesses.parquet.new', SCHEMA, compression='zstd')
    try:
        for record in snapshot['records']:
            if args.max_samples and counts['fitted'] >= args.max_samples:
                break
            if record['status'] != 'decoded':
                continue
            directory = args.data / record['path']
            facts = json.loads((directory / 'facts.json').read_text(encoding='utf-8'))
            segments = {r['segment'] for r in facts if r['kind'] == 'segment_facts' and r['map'] == args.map}
            epochs = [r for r in facts if r['kind'] == 'epoch' and r['segment'] in segments and r['start_conf'] == 2]
            tracks = [r for r in facts if r['kind'] == 'track' and r['segment'] in segments and
                      r['active_seen'] and not r['chase_spectator']]
            previous = {}; pair_index = 0
            offset = int(record['recording_id'][:8], 16) % args.stride
            for batch in pq.ParquetFile(directory / 'player_states.parquet').iter_batches(batch_size=8192):
                for state in batch.to_pylist():
                    if cancelled and cancelled():raise InterruptedError('Movement fitting cancelled')
                    key = state['segment'], state['slot']; old = previous.get(key)
                    previous[key] = state; counts['states_read'] += 1
                    if (not old or state['segment'] not in segments or not active(old) or not active(state) or
                            state['time_ms'] - old['time_ms'] != 100 or
                            math.dist(state['origin'], old['origin']) > 150):
                        continue
                    counts['continuous_pairs'] += 1
                    pair_index += 1
                    # Adding/reordering another recording must not resample
                    # this unchanged recording's movement evidence.
                    if (pair_index + offset) % args.stride:
                        continue
                    live = [e for e in epochs if e['segment'] == key[0] and e['time_ms'] <= old['time_ms'] and state['time_ms'] <= e['end_ms']]
                    actor = [t for t in tracks if (t['segment'], t['slot']) == key and
                             t['time_ms'] <= old['time_ms'] and state['time_ms'] <= t['end_ms'] and
                             (not t['identity_end_ms'] or state['time_ms'] < t['identity_end_ms'])]
                    if len(actor) != 1:
                        continue
                    if args.teaching_alias:
                        from opentdm_x_trainer.data import donor_match
                        if not donor_match(actor[0]['aliases'], args.teaching_alias):
                            continue
                        epoch = 'teaching:' + record['recording_id'] + ':' + str(key[0])
                    elif record.get('role') == 'tricks' and actor[0].get('is_recorder_pov'):
                        epoch = 'teaching:' + record['recording_id'] + ':' + str(key[0])
                    elif len(live) == 1:
                        epoch = live[0]['fingerprint']
                    else:
                        continue
                    av = (c.c_float * 9)(*(old['origin'] + old['velocity'] + old['view_angles']))
                    bv = (c.c_float * 9)(*(state['origin'] + state['velocity'] + state['view_angles']))
                    meta = (c.c_int * 5)(old['pm_flags'], old['pm_time'], old['gravity'], old['pm_type'], 100)
                    fitted = (c.c_float * 12)()
                    if not lib.OTXF_Fit(av, bv, meta, fitted):
                        counts['not_fitted'] += 1; continue
                    accepted = fitted[4] <= 4 and fitted[5] <= 40
                    counts['fitted'] += 1; counts['accepted_witnesses'] += accepted
                    if event and counts['fitted'] % 20 == 0:
                        event(dict(stage='movement_fitting', fitted=counts['fitted'], accepted=counts['accepted_witnesses']))
                    counts['accepted_jumps'] += accepted and fitted[7] > 0
                    errors.append(float(fitted[4]))
                    rows.append(dict(recording_id=record['recording_id'], segment=key[0], slot=key[1],
                        time_ms=old['time_ms'], seq=old['seq'], epoch=epoch, aliases=actor[0]['aliases'],
                        origin=old['origin'], destination=state['origin'], velocity=old['velocity'], end_velocity=state['velocity'],
                        view=old['view_angles'], end_view=state['view_angles'], flags=old['pm_flags'],
                        pm_time=old['pm_time'], gravity=old['gravity'], pm_type=old['pm_type'],
                        forward=int(fitted[0]), side=int(fitted[1]), jump_pattern=int(fitted[2]), substep_ms=int(fitted[3]),
                        position_error=float(fitted[4]), velocity_error=float(fitted[5]),
                        ground_contacts=int(fitted[6]), jump_events=int(fitted[7]), accepted_witness=bool(accepted)))
                    if len(rows) >= 512:
                        writer.write_table(pa.Table.from_pylist(rows, schema=SCHEMA)); rows = []
                    if args.max_samples and counts['fitted'] >= args.max_samples:
                        break
                if args.max_samples and counts['fitted'] >= args.max_samples:
                    break
            counts['recordings_examined'] += 1
        if rows:
            writer.write_table(pa.Table.from_pylist(rows, schema=SCHEMA))
    finally:
        writer.close(); lib.OTXF_Close()
    (args.out / 'witnesses.parquet.new').replace(args.out / 'witnesses.parquet')
    errors.sort()
    report = dict(source_run=snapshot['run_id'], counts=dict(counts),
                  seconds=round(time.monotonic() - began, 3),
                  map=args.map, bsp_sha256=hashlib.sha256(raw).hexdigest(),
                  physics_sha256=hashlib.sha256(args.physics.read_bytes()).hexdigest(),
                  position_error_quantiles={str(q): errors[round((len(errors)-1)*q)] for q in (.5, .9, .95)} if errors else {},
                  original_commands=False, learned_policy=False, teaching_alias=args.teaching_alias,
                  limits=['Static BSP and movers at initial stops; projectile knockback is not modeled.',
                          'Controls are possible reconstructed witnesses, never original player input.',
                          'Acceptance only checks the recorded endpoint and velocity; no style or expert-strength claim.',
                          'Sampling is bounded; match grouping and held-out evaluation remain required.'])
    (args.out / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if event is None:print(json.dumps({k: report[k] for k in ('counts', 'seconds', 'position_error_quantiles')}))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--bsp', type=Path, required=True)
    parser.add_argument('--map', required=True)
    parser.add_argument('--physics', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--stride', type=int, default=40)
    parser.add_argument('--max-samples', type=int, default=6000)
    parser.add_argument('--teaching-alias', help='Explicitly authorized direct-POV mechanics examples; no competitive-match claim')
    main(parser.parse_args())
