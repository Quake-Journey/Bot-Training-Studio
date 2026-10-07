"""Causal HUD pickup evidence, not a reconstructed inventory or item timer.

A lingering pickup label is not a new pickup. A missing view is not proof
that an item is absent. Names are resolved at the current decoder sequence.
"""
VERSION = 'hud-pickup-edges-v1'
PACKET_VERSION = 'packet-item-models-v1'
NAMES = {
    'railgun': 'weapon_railgun', 'chaingun': 'weapon_chaingun',
    'rocket launcher': 'weapon_rocketlauncher', 'super shotgun': 'weapon_supershotgun',
    'hyperblaster': 'weapon_hyperblaster', 'grenade launcher': 'weapon_grenadelauncher',
    'shotgun': 'weapon_shotgun', 'machinegun': 'weapon_machinegun', 'bfg10k': 'weapon_bfg',
    'body armor': 'item_armor_body', 'combat armor': 'item_armor_combat',
    'jacket armor': 'item_armor_jacket', 'armor shard': 'item_armor_shard',
    'adrenaline': 'item_adrenaline', 'ammo pack': 'item_pack',
    'health': 'item_health', 'mega health': 'item_health_mega',
    'bullets': 'ammo_bullets', 'shells': 'ammo_shells', 'cells': 'ammo_cells',
    'slugs': 'ammo_slugs', 'rockets': 'ammo_rockets', 'grenades': 'ammo_grenades',
}

MODELS = {f'models/weapons/g_{token}/tris.md2':'weapon_'+name for token,name in
    (('rail','railgun'),('chain','chaingun'),('rocket','rocketlauncher'),('shotg2','supershotgun'),
     ('hyperb','hyperblaster'),('launch','grenadelauncher'),('shotg','shotgun'),('machn','machinegun'),('bfg','bfg'))}
MODELS.update({f'models/items/ammo/{name}/medium/tris.md2':'ammo_'+name
               for name in ('bullets','shells','cells','slugs','rockets','grenades')})
MODELS.update({f'models/items/armor/{name}/tris.md2':'item_armor_'+name
               for name in ('body','combat','jacket','shard')})
MODELS.update({f'models/items/healing/{name}/tris.md2':'item_health'
               for name in ('stimpack','medium','large')})
MODELS.update({'models/items/mega_h/tris.md2':'item_health_mega',
               'models/items/adrenal/tris.md2':'item_adrenaline','models/items/pack/tris.md2':'item_pack'})


def packet_items(scene, configs, base):
    """Present world models only; PVS presence is neither line of sight nor ownership."""
    result=[]
    for entity in scene['entities']:
        name=MODELS.get(configs.get(base+entity['models'][0],''))
        if name:
            result.append(dict(item=name,origin=entity['origin'],entity=entity['number']))
    return result


def pickup(previous, current, configs, items_base):
    """Return only observed changes on a continuous living authoritative POV.

    First frame/gap/death/chase is censored. Health deltas do not identify a
    pickup by themselves (adrenaline, mixed damage/healing and mod rules).
    """
    if not previous or items_base is None:
        return None
    for s in (previous, current):
        if (len(s['stats']) <= 17 or s['pm_type'] != 0 or s['stats'][1] <= 0
                or s['stats'][16] or s['stats'][17]):
            return None
    if not 70 <= current['time_ms'] - previous['time_ms'] <= 130:
        return None
    cs = current['stats'][8]
    if cs <= 0 or cs == previous['stats'][8] or not items_base <= cs < items_base + 256:
        return None
    name = configs.get(cs, '').strip()
    if not name:
        return None
    item = NAMES.get(name.casefold(), 'unknown')
    # Stock Health is shared by multiple entity sizes. Keep the actual HUD
    # observation generic; a 100-point delta alone is not universal mod truth.
    return dict(name=name, item=item, evidence='hud_label_edge', seq=current['seq'],
                time_ms=current['time_ms'])
