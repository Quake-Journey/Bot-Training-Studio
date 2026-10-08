"""Read bounded IBSP38 geometry and game entities, never infer item availability."""
import hashlib
import math
from pathlib import Path
import re
import struct


def category(name):
    if name.startswith("weapon_"):
        return "weapon"
    if name.startswith("ammo_"):
        return "ammo"
    if "armor" in name:
        return "armor"
    if name.startswith("item_health") or name == "item_adrenaline":
        return "health"
    if name in ("item_pack", "item_bandolier"):
        return "capacity"
    return "other"


def inspect(path, map_name=None):
    path = Path(path)
    map_name = map_name or path.stem
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", map_name):
        raise ValueError("Invalid logical map name")
    if not 160 <= path.stat().st_size <= 128 * 1024**2:
        raise ValueError("BSP size outside supported bounds")
    raw = path.read_bytes()
    if raw[:4] != b"IBSP" or struct.unpack_from("<i", raw, 4)[0] != 38:
        raise ValueError("Only Quake II IBSP version 38 is supported")
    lumps = []
    for i in range(19):
        offset, length = struct.unpack_from("<ii", raw, 8 + i * 8)
        if offset < 0 or length < 0 or offset + length > len(raw):
            raise ValueError("BSP lump outside file")
        lumps.append(raw[offset:offset + length])
    if len(lumps[0]) > 2 * 1024**2 or len(lumps[2]) % 12 or len(lumps[13]) % 48:
        raise ValueError("Malformed BSP entity/vertex/model lump")
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', lumps[0].rstrip(b"\0").decode("latin1"))
    entities, current, key = [], None, None
    for value, brace in tokens:
        if brace == "{":
            if current is not None:
                raise ValueError("Nested entity")
            current = {}
        elif brace == "}":
            if current is None or key is not None:
                raise ValueError("Unbalanced entity")
            entities.append(current); current = None
        elif current is not None:
            if key is None:
                key = value
            else:
                current[key] = value; key = None
    if current is not None or not entities or entities[0].get("classname") != "worldspawn":
        raise ValueError("Incomplete BSP entities")
    items, spawns, movers, teleports = [], [], [], []
    for index, ent in enumerate(entities):
        name = ent.get("classname", "")
        origin = [float(v) for v in ent.get("origin", "0 0 0").split()]
        if len(origin) != 3 or not all(math.isfinite(v) and abs(v) < 65536 for v in origin):
            raise ValueError("Invalid entity origin")
        base = dict(id=index, classname=name, origin=origin, spawnflags=int(ent.get("spawnflags", "0")),
                    target=ent.get("target"), targetname=ent.get("targetname"))
        if name.startswith(("weapon_", "ammo_", "item_")):
            items.append(dict(base, category=category(name)))
        if name.startswith("info_player_"):
            spawns.append(base)
        if name.startswith("func_"):
            movers.append(dict(base, model=ent.get("model"), speed=ent.get("speed"), height=ent.get("height")))
        if "teleport" in name:
            teleports.append(base)
    from .transitions import inventory, VERSION
    models = []
    for offset in range(0, len(lumps[13]), 48):
        values = struct.unpack_from('<6f', lumps[13], offset)
        if not all(math.isfinite(v) and abs(v) < 65536 for v in values) or any(values[i] > values[i+3] for i in range(3)):
            raise ValueError('Invalid inline model bounds')
        models.append((list(values[:3]), list(values[3:])))
    mechanisms = inventory(entities, models)
    vertices = list(struct.iter_unpack("<3f", lumps[2]))
    if not vertices or not all(math.isfinite(v) for point in vertices for v in point):
        raise ValueError("Invalid map vertices")
    return dict(schema=1, map=map_name, bsp_sha256=hashlib.sha256(raw).hexdigest(),
                title=entities[0].get("message", path.stem), items=items, spawns=spawns,
                movers=movers, teleports=teleports, transitions=mechanisms, transition_version=VERSION,
                vertices=len(vertices),
                bounds=[[min(p[i] for p in vertices) for i in range(3)], [max(p[i] for p in vertices) for i in range(3)]],
                world=entities[0], item_availability="initial entity placements only; runtime presence not implied")
