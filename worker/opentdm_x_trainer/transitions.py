"""Map-bound movement mechanisms. Geometry is not a claim of route reachability.

Pure data/geometry code: no Torch, renderer, game DLL or mutable native world.
Keep uncertain jumps, deaths and missing frames distinct from observed launches.
"""
import math

VERSION = 'map-transitions-v1'
PLAYER_MIN = (-16., -16., -24.)
PLAYER_MAX = (16., 16., 32.)


def vector(value, default='0 0 0'):
    result = [float(v) for v in (value or default).split()]
    if len(result) != 3 or not all(math.isfinite(v) and abs(v) < 65536 for v in result):
        raise ValueError('Invalid transition vector')
    return result


def direction(angle):
    if angle == -1: return [0., 0., 1.]
    if angle == -2: return [0., 0., -1.]
    return [math.cos(math.radians(angle)), math.sin(math.radians(angle)), 0.]


def push_direction(entity):
    angles = vector(entity.get('angles')) if 'angles' in entity else [0., float(entity.get('angle', '0')), 0.]
    if angles == [0., -1., 0.]: return [0., 0., 1.]
    if angles == [0., -2., 0.]: return [0., 0., -1.]
    if angles == [0., 0., 0.]: return [0., 0., 0.]  # InitTrigger does not call G_SetMovedir.
    pitch, yaw = math.radians(angles[0]), math.radians(angles[1])
    return [math.cos(pitch)*math.cos(yaw), math.cos(pitch)*math.sin(yaw), -math.sin(pitch)]


def inventory(entities, models):
    """Resolve DM-active trigger volumes and named destinations in two passes.

    misc_teleporter's DISC is not its trigger: g_misc.c creates [-8,-8,8]
    ..[8,8,24]. Destination +10 z and trigger_push speed*10 follow the game.
    Duplicate/missing target names stay unresolved, never silently choose one.
    """
    named = {}
    for index, entity in enumerate(entities):
        if entity.get('targetname') and not int(entity.get('spawnflags', '0')) & 2048:
            named.setdefault(entity['targetname'], []).append((index, entity))
    result = []
    for index, entity in enumerate(entities):
        name = entity.get('classname')
        if name not in ('misc_teleporter', 'trigger_teleport', 'trigger_push'):
            continue
        if int(entity.get('spawnflags', '0')) & 2048:
            continue
        origin = vector(entity.get('origin'))
        model = entity.get('model', '')
        bounds = None
        if model.startswith('*') and model[1:].isdigit():
            number = int(model[1:])
            if not 0 < number < len(models):
                raise ValueError('Transition references an invalid inline model')
            low, high = models[number]
            bounds = [[v + origin[i] for i, v in enumerate(side)] for side in (low, high)]
        elif name == 'misc_teleporter':
            bounds = [[origin[i] + d for i, d in enumerate(side)]
                      for side in ((-8., -8., 8.), (8., 8., 24.))]
        row = dict(id=index, kind='push' if name == 'trigger_push' else 'teleport',
                   classname=name, bounds=bounds, target=entity.get('target'), resolved=False)
        if row['kind'] == 'push':
            speed = float(entity.get('speed', '0')) or 1000.
            if not math.isfinite(speed) or not 0 < speed <= 10000:
                raise ValueError('Invalid push speed/direction')
            row.update(velocity=[v * speed * 10 for v in push_direction(entity)],
                       once=bool(int(entity.get('spawnflags', '0')) & 1), resolved=bounds is not None)
        else:
            destinations = named.get(entity.get('target'), [])
            if len(destinations) == 1:
                dest_id, dest = destinations[0]
                xyz = vector(dest.get('origin')); xyz[2] += 10
                row.update(destination=xyz, destination_id=dest_id,
                           destination_angles=vector(dest.get('angles')) if 'angles' in dest
                           else [0., float(dest.get('angle', '0')), 0.], resolved=bounds is not None)
            else:
                row['unresolved_reason'] = 'ambiguous_target' if destinations else 'missing_target'
        result.append(row)
    return result


def intersects(start, end, bounds, padding=0.):
    """Swept standing player hull against a trigger AABB (conservative)."""
    if bounds is None: return False
    lo, hi = 0., 1.
    for axis in range(3):
        low = bounds[0][axis] - PLAYER_MAX[axis] - padding
        high = bounds[1][axis] - PLAYER_MIN[axis] + padding
        delta = end[axis] - start[axis]
        if abs(delta) < 1e-9:
            if not low <= start[axis] <= high: return False
        else:
            a, b = (low-start[axis])/delta, (high-start[axis])/delta
            lo, hi = max(lo, min(a,b)), min(hi, max(a,b))
            if lo > hi: return False
    return True


def living(row):
    return row['pm_type'] == 0 and row['stats'][1] > 0 and not row['stats'][16] and not row['stats'][17]


def classify(previous, current, mechanisms):
    """Classify one same-track, adjacent playerstate edge; gaps never teleport.

    Teleport requires source-volume evidence AND target evidence AND either
    teleport hold flags or a discontinuity. The no-freeze variant is explicitly
    geometry-consistent evidence, not a claim to have observed the trigger call.
    Push requires a swept entry AND its characteristic velocity impulse.
    """
    if previous is None: return dict(kind='start', known=False)
    if any(previous[k] != current[k] for k in ('recording_id','segment','slot')):
        return dict(kind='gap', known=False)
    dt = current['time_ms'] - previous['time_ms']
    if not 70 <= dt <= 130: return dict(kind='gap', known=False)
    if not living(previous) or not living(current): return dict(kind='life_boundary', known=False)
    a, b = previous['origin'], current['origin']
    velocity = previous['velocity']
    projected = [a[i] + velocity[i]*dt/1000 for i in range(3)]
    candidates = []
    for mechanism in mechanisms:
        if not mechanism['resolved']: continue
        if mechanism['kind'] == 'teleport':
            # Do not sweep through the entire world along the apparent jump.
            entry = intersects(a, projected, mechanism['bounds'], padding=8.)
            target = math.dist(b, mechanism['destination']) <= 96
            held = bool(current['pm_flags'] & 32) and not bool(previous['pm_flags'] & 32)
            if entry and target and (held or math.dist(a,b) > 160):
                candidates.append(dict(kind='teleport', known=True, id=mechanism['id'],
                    evidence='hold_and_geometry' if held else 'geometry_consistent'))
        else:
            expected = mechanism['velocity']
            impulse = math.dist(previous['velocity'], expected) > 160
            # Allow one 100ms gravity tick and air steering after activation.
            launched = math.dist(current['velocity'], expected) <= 150
            # The launch reverses vertical motion inside this network tick;
            # the straight chord between recorded endpoints can miss the pad.
            entry = (intersects(a, projected, mechanism['bounds'], padding=2.) or
                     intersects(a, b, mechanism['bounds'], padding=2.))
            if entry and impulse and launched:
                candidates.append(dict(kind='push', known=True, id=mechanism['id'], evidence='geometry_and_impulse'))
    if len(candidates) > 1 and all(r['kind'] == 'teleport' for r in candidates):
        destinations = {next(m['destination_id'] for m in mechanisms if m['id'] == r['id']) for r in candidates}
        if len(destinations) == 1:
            return dict(candidates[0], equivalent_entries=[r['id'] for r in candidates])
    if len(candidates) == 1: return candidates[0]
    if candidates: return dict(kind='ambiguous', known=False)
    if math.dist(a,b) > 160 or (bool(current['pm_flags'] & 32) and not bool(previous['pm_flags'] & 32)):
        return dict(kind='discontinuity', known=False)
    return dict(kind='ordinary', known=True)
