/*
 * MapGenMovers - see inc/common/mapgen_movers.h.
 *
 * The travel of a door and of a lift is not a guess: it is the arithmetic
 * `g_func.c` does at spawn, reproduced here from the same keys. A door moves
 * along its `angle` by the width of the model in that direction less its lip;
 * a lift's stops are its drawn position and that position dropped by its
 * height, and an untargeted lift RESTS at the bottom because that is where the
 * game puts it. Getting either of those backwards moves a floor by a hundred
 * units and invents or destroys a route.
 */

#include "common/mapgen_movers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *MapGenMovers_KindName(mapgen_mover_kind_t kind)
{
    switch (kind) {
    case MAPGEN_MOVER_DOOR:   return "door";
    case MAPGEN_MOVER_PLAT:   return "lift";
    case MAPGEN_MOVER_TRAIN:  return "train";
    case MAPGEN_MOVER_BUTTON: return "button";
    case MAPGEN_MOVER_LIQUID: return "liquid";
    case MAPGEN_MOVER_OTHER:  break;
    }
    return "mover";
}

/* ---- the entity string ------------------------------------------------------ */

#define MAX_KEYS 32

typedef struct {
    char key[MAPGEN_MOVER_NAME];
    char value[MAPGEN_MOVER_NAME * 2];
} pair_t;

typedef struct {
    pair_t   pairs[MAX_KEYS];
    uint32_t count;
} entity_t;

static const char *entity_value(const entity_t *e, const char *key)
{
    for (uint32_t i = 0; i < e->count; i++) {
        if (!strcmp(e->pairs[i].key, key))
            return e->pairs[i].value;
    }
    return NULL;
}

static float entity_float(const entity_t *e, const char *key, float fallback)
{
    const char *v = entity_value(e, key);
    return v && *v ? (float)atof(v) : fallback;
}

static void entity_vector(const entity_t *e, const char *key, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    const char *v = entity_value(e, key);
    if (!v)
        return;
    char *cursor = (char *)v;
    for (int i = 0; i < 3; i++)
        out[i] = strtof(cursor, &cursor);
}

/* Copy a value into a fixed field, always terminated. */
static void copy_name(char *dst, size_t size, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Walk the entity string, handing each block to a visitor.
 *
 * Quoted values are read as quoted: a targetname with a space in it is one
 * name, and a key/value scan that split on whitespace would silently make it
 * two and then never match the door it belongs to.
 */
typedef void (*visitor_t)(const entity_t *e, void *user);

static void for_each_entity(const char *text, uint32_t length,
                            visitor_t visit, void *user)
{
    const char *at = text;
    const char *end = text + length;

    while (at < end) {
        while (at < end && *at != '{')
            at++;
        if (at >= end)
            return;
        at++;

        entity_t e;
        e.count = 0;

        while (at < end && *at != '}') {
            while (at < end && *at != '"' && *at != '}')
                at++;
            if (at >= end || *at == '}')
                break;
            at++;
            const char *ks = at;
            while (at < end && *at != '"')
                at++;
            const size_t klen = (size_t)(at - ks);
            if (at < end)
                at++;

            while (at < end && *at != '"' && *at != '}')
                at++;
            if (at >= end || *at == '}')
                break;
            at++;
            const char *vs = at;
            while (at < end && *at != '"')
                at++;
            const size_t vlen = (size_t)(at - vs);
            if (at < end)
                at++;

            if (e.count < MAX_KEYS && klen < sizeof(e.pairs[0].key) &&
                vlen < sizeof(e.pairs[0].value)) {
                memcpy(e.pairs[e.count].key, ks, klen);
                e.pairs[e.count].key[klen] = '\0';
                memcpy(e.pairs[e.count].value, vs, vlen);
                e.pairs[e.count].value[vlen] = '\0';
                e.count++;
            }
        }
        if (at < end)
            at++;

        if (e.count)
            visit(&e, user);
    }
}

/* ---- reading one mover ------------------------------------------------------ */

typedef struct {
    const mapgen_bsp_t *bsp;
    mapgen_movers_t    *out;
} reader_t;

/* "*7" -> 7, and anything else is not a brush model. */
static bool brush_model(const entity_t *e, uint32_t *out)
{
    const char *v = entity_value(e, "model");
    if (!v || v[0] != '*')
        return false;
    char *tail = NULL;
    const unsigned long n = strtoul(v + 1, &tail, 10);
    if (tail == v + 1)
        return false;
    *out = (uint32_t)n;
    return true;
}

/*
 * `angle` into a direction, with the game's two reserved values.
 *
 * -1 is up and -2 is down; every other value is a yaw in degrees. They are not
 * angles at all, and reading them as angles sends a lift sideways.
 */
static void move_direction(float angle, float out[3])
{
    if (angle == -1.0f) {
        out[0] = 0; out[1] = 0; out[2] = 1;
    } else if (angle == -2.0f) {
        out[0] = 0; out[1] = 0; out[2] = -1;
    } else {
        const float rad = angle * (float)(3.14159265358979323846 / 180.0);
        out[0] = cosf(rad);
        out[1] = sinf(rad);
        out[2] = 0;
    }
}

static void add_operator(mapgen_movers_t *m, const char *target,
                         const float mins[3], const float maxs[3],
                         bool shootable)
{
    if (!target || !*target)
        return;
    if (m->num_operators >= MAPGEN_MOVER_OPS) {
        m->overflowed = true;
        return;
    }
    mapgen_mover_operator_t *op = &m->operators[m->num_operators++];
    memset(op, 0, sizeof(*op));
    copy_name(op->target, sizeof(op->target), target);
    for (int i = 0; i < 3; i++) {
        op->mins[i] = mins[i];
        op->maxs[i] = maxs[i];
    }
    op->shootable = shootable;
}

static void add_relay(mapgen_movers_t *m, const char *name, const char *target)
{
    if (!name || !*name || !target || !*target)
        return;
    if (m->num_relays >= MAPGEN_MOVER_RELAYS) {
        m->overflowed = true;
        return;
    }
    mapgen_mover_relay_t *rl = &m->relays[m->num_relays++];
    copy_name(rl->name, sizeof(rl->name), name);
    copy_name(rl->target, sizeof(rl->target), target);
}

static void read_entity(const entity_t *e, void *user)
{
    reader_t *r = (reader_t *)user;
    mapgen_movers_t *m = r->out;

    const char *classname = entity_value(e, "classname");
    if (!classname)
        return;

    const char *targetname = entity_value(e, "targetname");
    const char *target = entity_value(e, "target");

    /* --- the things a player can operate that are not themselves movers --- */

    if (!strncmp(classname, "trigger_", 8) && target) {
        uint32_t model;
        if (brush_model(e, &model)) {
            const mapgen_bsp_model_t *bm = MapGenBsp_Model(r->bsp, model);
            if (bm && !targetname)
                add_operator(m, target, bm->mins, bm->maxs, false);
            else if (bm && targetname)
                add_relay(m, targetname, target);
        } else if (targetname) {
            /* No brush: it is not somewhere a player can go, it is a name
               that passes a firing on. */
            add_relay(m, targetname, target);
        }
    }

    /*
     * A push volume. `speed` is in the game's own units and it multiplies by
     * ten, which is not a rounding of anything - it is what `g_trigger.c`
     * does, and a push read without it is a tenth of the launch.
     */
    if (!strcmp(classname, "trigger_push")) {
        uint32_t model;
        if (brush_model(e, &model)) {
            const mapgen_bsp_model_t *bm = MapGenBsp_Model(r->bsp, model);
            if (bm && m->num_pushes < MAPGEN_MOVER_PORTALS) {
                mapgen_mover_push_t *push = &m->pushes[m->num_pushes++];
                memset(push, 0, sizeof(*push));
                for (int i = 0; i < 3; i++) {
                    push->mins[i] = bm->mins[i];
                    push->maxs[i] = bm->maxs[i];
                }
                float dir[3];
                move_direction(entity_float(e, "angle", -1.0f), dir);
                const float speed = entity_float(e, "speed", 1000.0f) * 10.0f;
                for (int i = 0; i < 3; i++)
                    push->velocity[i] = dir[i] * speed;
            } else if (bm) {
                m->overflowed = true;
            }
        }
        return;
    }

    if (!strcmp(classname, "trigger_teleport") ||
        !strcmp(classname, "misc_teleporter")) {
        uint32_t model;
        const mapgen_bsp_model_t *bm = NULL;
        if (brush_model(e, &model))
            bm = MapGenBsp_Model(r->bsp, model);
        if (m->num_portals < MAPGEN_MOVER_PORTALS) {
            mapgen_mover_portal_t *p = &m->portals[m->num_portals++];
            memset(p, 0, sizeof(*p));
            if (bm) {
                for (int i = 0; i < 3; i++) {
                    p->mins[i] = bm->mins[i];
                    p->maxs[i] = bm->maxs[i];
                }
            } else {
                float o[3];
                entity_vector(e, "origin", o);
                for (int i = 0; i < 3; i++) {
                    p->mins[i] = o[i] - 16.0f;
                    p->maxs[i] = o[i] + 16.0f;
                }
            }
            /* Where it goes is another entity's origin, and that entity may
               not have been read yet: the name is kept and a second pass
               turns it into a place. */
            copy_name(p->target, sizeof(p->target), target);
            p->has_destination = false;
        } else {
            m->overflowed = true;
        }
        return;
    }

    /* --- the movers themselves -------------------------------------------- */

    uint32_t model;
    if (!brush_model(e, &model))
        return;
    const mapgen_bsp_model_t *bm = MapGenBsp_Model(r->bsp, model);
    if (!bm)
        return;

    mapgen_mover_kind_t kind = MAPGEN_MOVER_OTHER;
    if (!strcmp(classname, "func_door") || !strcmp(classname, "func_door_rotating"))
        kind = MAPGEN_MOVER_DOOR;
    else if (!strcmp(classname, "func_plat"))
        kind = MAPGEN_MOVER_PLAT;
    else if (!strcmp(classname, "func_train"))
        kind = MAPGEN_MOVER_TRAIN;
    else if (!strcmp(classname, "func_button"))
        kind = MAPGEN_MOVER_BUTTON;
    else if (!strcmp(classname, "func_water"))
        kind = MAPGEN_MOVER_LIQUID;
    else
        return;

    if (m->num_movers >= MAPGEN_MOVER_MAX) {
        m->overflowed = true;
        return;
    }

    mapgen_mover_t *mv = &m->movers[m->num_movers++];
    memset(mv, 0, sizeof(*mv));
    mv->kind = kind;
    mv->model = model;
    copy_name(mv->classname, sizeof(mv->classname), classname);
    copy_name(mv->targetname, sizeof(mv->targetname), targetname);
    copy_name(mv->target, sizeof(mv->target), target);
    for (int i = 0; i < 3; i++) {
        mv->mins[i] = bm->mins[i];
        mv->maxs[i] = bm->maxs[i];
    }

    const float size[3] = { bm->maxs[0] - bm->mins[0],
                            bm->maxs[1] - bm->mins[1],
                            bm->maxs[2] - bm->mins[2] };
    const float lip = entity_float(e, "lip", 8.0f);
    const int spawnflags = (int)entity_float(e, "spawnflags", 0.0f);

    /* Stop 0 is always where the model was drawn. */
    mv->num_stops = 1;

    switch (kind) {
    case MAPGEN_MOVER_DOOR:
    case MAPGEN_MOVER_LIQUID: {
        float dir[3];
        move_direction(entity_float(e, "angle", 0.0f), dir);
        const float span = fabsf(dir[0] * size[0]) + fabsf(dir[1] * size[1]) +
                           fabsf(dir[2] * size[2]);
        const float travel = span - lip;
        for (int i = 0; i < 3; i++)
            mv->stop[1][i] = dir[i] * travel;
        mv->num_stops = 2;
        /*
         * START_OPEN, which the game implements by swapping the two positions
         * at spawn. Written out rather than folded in, because a door that is
         * open at rest and closes when fired is the exact case that makes a
         * route appear and then vanish.
         */
        if (spawnflags & 1) {
            for (int i = 0; i < 3; i++) {
                const float t = mv->stop[0][i];
                mv->stop[0][i] = mv->stop[1][i];
                mv->stop[1][i] = t;
            }
        }
        /* A door with no targetname opens for anyone who walks up to it. */
        mv->player_operated = !targetname;
        break;
    }
    case MAPGEN_MOVER_PLAT: {
        float height = entity_float(e, "height", 0.0f);
        if (height == 0.0f)
            height = size[2] - lip;
        /*
         * The game spawns a lift at the top and, if nothing targets it, moves
         * it to the bottom and leaves it there waiting for a player. So its
         * RESTING place - stop zero, the one a trace uses when nothing has
         * happened - is the bottom, and its raised place is where it was
         * drawn. A lift read the other way up puts a solid floor across the
         * room it is supposed to serve.
         */
        if (targetname) {
            mv->stop[1][2] = -height;       /* rests up, called down */
        } else {
            mv->stop[0][2] = -height;       /* rests down, rises when ridden */
        }
        mv->num_stops = 2;
        mv->player_operated = !targetname;
        mv->carries = true;
        break;
    }
    case MAPGEN_MOVER_TRAIN:
        /* The stops are its path corners, filled in by the second pass: a
           train is named by the first corner it is told to go to. */
        mv->carries = true;
        mv->player_operated = !targetname;
        break;
    case MAPGEN_MOVER_BUTTON: {
        float dir[3];
        move_direction(entity_float(e, "angle", 0.0f), dir);
        const float span = fabsf(dir[0] * size[0]) + fabsf(dir[1] * size[1]) +
                           fabsf(dir[2] * size[2]);
        const float travel = span - lip;
        for (int i = 0; i < 3; i++)
            mv->stop[1][i] = dir[i] * travel;
        mv->num_stops = 2;
        mv->player_operated = true;
        /* A button IS an operator: reaching it fires what it targets. */
        add_operator(m, target, bm->mins, bm->maxs, (spawnflags & 1) != 0);
        break;
    }
    case MAPGEN_MOVER_OTHER:
        break;
    }
}

/* ---- path corners, resolved after everything is known ----------------------- */

typedef struct {
    char  name[MAPGEN_MOVER_NAME];
    char  next[MAPGEN_MOVER_NAME];
    float origin[3];
} corner_t;

typedef struct {
    corner_t  corners[MAPGEN_MOVER_STOPS * 4];
    uint32_t  count;
    bool      overflowed;
} corners_t;

static void read_corner(const entity_t *e, void *user)
{
    corners_t *c = (corners_t *)user;
    const char *classname = entity_value(e, "classname");
    if (!classname || strcmp(classname, "path_corner"))
        return;
    if (c->count >= sizeof(c->corners) / sizeof(c->corners[0])) {
        c->overflowed = true;
        return;
    }
    corner_t *k = &c->corners[c->count++];
    memset(k, 0, sizeof(*k));
    copy_name(k->name, sizeof(k->name), entity_value(e, "targetname"));
    copy_name(k->next, sizeof(k->next), entity_value(e, "target"));
    entity_vector(e, "origin", k->origin);
}

static const corner_t *find_corner(const corners_t *c, const char *name)
{
    if (!name || !*name)
        return NULL;
    for (uint32_t i = 0; i < c->count; i++) {
        if (!strcmp(c->corners[i].name, name))
            return &c->corners[i];
    }
    return NULL;
}

/*
 * A train's stops are the ring of path corners it is sent around.
 *
 * The corners are absolute positions and the model is at a position of its
 * own, so the displacement to a corner is the corner less where the train
 * starts - which the game takes as the model's mins, not its centre.
 */
static void resolve_trains(mapgen_movers_t *m, const corners_t *c)
{
    for (uint32_t i = 0; i < m->num_movers; i++) {
        mapgen_mover_t *mv = &m->movers[i];
        if (mv->kind != MAPGEN_MOVER_TRAIN)
            continue;

        const corner_t *first = find_corner(c, mv->target);
        if (!first) {
            mv->inoperable = true;
            continue;
        }

        float base[3];
        for (int a = 0; a < 3; a++)
            base[a] = mv->mins[a];

        const corner_t *at = first;
        mv->num_stops = 0;
        for (uint32_t step = 0; step < MAPGEN_MOVER_STOPS && at; step++) {
            for (int a = 0; a < 3; a++)
                mv->stop[mv->num_stops][a] = at->origin[a] - base[a];
            mv->num_stops++;
            const corner_t *next = find_corner(c, at->next);
            if (!next || next == first)
                break;
            at = next;
        }
        if (!mv->num_stops) {
            mv->num_stops = 1;
            memset(mv->stop[0], 0, sizeof(mv->stop[0]));
        }
    }
}

/*
 * Which movers nothing can ever fire.
 *
 * A mover with a targetname needs something to name it, and that something has
 * to be reachable; whether it is reachable is the search's business, but
 * whether it EXISTS is knowable here. A door whose only button was never
 * placed is inoperable, and a map that needs it is broken no matter how well
 * a player moves.
 */
static void resolve_operability(mapgen_movers_t *m)
{
    /* Everything a player could ever set off, followed through the relays. */
    uint64_t operable = 0;
    for (uint32_t o = 0; o < m->num_operators; o++)
        operable |= MapGenMovers_Fired(m, m->operators[o].target);
    for (uint32_t o = 0; o < m->num_movers; o++) {
        if (m->movers[o].player_operated && m->movers[o].target[0])
            operable |= MapGenMovers_Fired(m, m->movers[o].target);
    }

    for (uint32_t i = 0; i < m->num_movers && i < 64; i++) {
        mapgen_mover_t *mv = &m->movers[i];
        if (mv->player_operated || !mv->targetname[0])
            continue;
        mv->inoperable = !((operable >> i) & 1u);
    }
}

/*
 * Where a teleporter puts a player.
 *
 * The destination is a separate entity somewhere else in the string, found by
 * name, and it may be read before or after the teleporter that names it - so
 * this is a second pass and not a lookup during the first. A teleporter whose
 * destination is missing keeps `has_destination` false, and the search treats
 * it as an exit that goes nowhere rather than as a hole to fall through.
 */
static void resolve_destination(const entity_t *e, void *user)
{
    mapgen_movers_t *m = (mapgen_movers_t *)user;
    const char *name = entity_value(e, "targetname");
    if (!name || !*name)
        return;

    float origin[3];
    entity_vector(e, "origin", origin);

    for (uint32_t i = 0; i < m->num_portals; i++) {
        mapgen_mover_portal_t *p = &m->portals[i];
        if (p->has_destination || strcmp(p->target, name))
            continue;
        /*
         * A player comes out standing, and the entity marks where his EYES
         * go. The game drops him to the floor from there; so does the search,
         * which is why this is a starting point and not a resting place.
         */
        for (int a = 0; a < 3; a++)
            p->destination[a] = origin[a];
        p->has_destination = true;
    }
}

bool MapGenMovers_Read(const mapgen_bsp_t *bsp, mapgen_movers_t *out)
{
    if (!bsp || !out)
        return false;
    memset(out, 0, sizeof(*out));

    uint32_t length = 0;
    const char *text = MapGenBsp_Entities(bsp, &length);
    if (!text || !length)
        return true;

    reader_t r = { bsp, out };
    for_each_entity(text, length, read_entity, &r);

    corners_t corners;
    memset(&corners, 0, sizeof(corners));
    for_each_entity(text, length, read_corner, &corners);
    if (corners.overflowed)
        out->overflowed = true;

    for_each_entity(text, length, resolve_destination, out);
    resolve_trains(out, &corners);
    resolve_operability(out);
    return true;
}

const float *MapGenMovers_Displacement(const mapgen_movers_t *m, uint32_t i,
                                       uint64_t opened)
{
    static const float none[3] = { 0, 0, 0 };
    if (!m || i >= m->num_movers)
        return none;
    const mapgen_mover_t *mv = &m->movers[i];
    if (!mv->num_stops)
        return none;
    const bool open = (opened >> (i & 63u)) & 1u;
    const uint32_t which = open ? mv->num_stops - 1 : 0;
    return mv->stop[which];
}

bool MapGenMovers_OperatorTouched(const mapgen_mover_operator_t *op,
                                  const float point[3])
{
    if (!op || !point)
        return false;
    /*
     * The reach of a player standing next to it, not the brush itself. A
     * button is flush with a wall and a player never occupies the same space,
     * so an exact containment test says nobody in the map can ever press
     * anything.
     */
    const float slack = 40.0f;
    for (int i = 0; i < 3; i++) {
        if (point[i] < op->mins[i] - slack || point[i] > op->maxs[i] + slack)
            return false;
    }
    return true;
}

/*
 * Every name a firing eventually reaches.
 *
 * Relays can name each other, and a map that loops them would hang a naive
 * walk, so each name is taken once: the frontier only ever grows, and it is
 * bounded by the number of relays there are.
 */
static uint32_t firing_closure(const mapgen_movers_t *m, const char *target,
                               const char **names, uint32_t limit)
{
    uint32_t count = 0;
    if (!target || !*target || !limit)
        return 0;
    names[count++] = target;

    for (uint32_t at = 0; at < count; at++) {
        for (uint32_t r = 0; r < m->num_relays && count < limit; r++) {
            if (strcmp(m->relays[r].name, names[at]))
                continue;
            bool seen = false;
            for (uint32_t k = 0; k < count && !seen; k++)
                seen = !strcmp(names[k], m->relays[r].target);
            if (!seen)
                names[count++] = m->relays[r].target;
        }
    }
    return count;
}

/*
 * Can it get from one stop to the next without running into the map?
 *
 * The model's own bounds, swept along the vector between two stops, against
 * the world. Anything solid in the way means the machine is blocked - and a
 * blocked machine is one a player can wait beside for ever, which is the same
 * outcome as a machine nothing can fire.
 *
 * The world only: two movers that pass through each other are a mapper's
 * business and the game lets them, but a lift that grinds into a wall never
 * arrives.
 */
void MapGenMovers_CheckSweeps(mapgen_movers_t *m, mapgen_trace_context_t *ctx)
{
    if (!m || !ctx)
        return;

    for (uint32_t i = 0; i < m->num_movers; i++) {
        mapgen_mover_t *mv = &m->movers[i];
        if (!mv->carries || mv->num_stops < 2)
            continue;

        /* The hull is the model itself, traced about its own centre. */
        float centre[3], half[3];
        for (int a = 0; a < 3; a++) {
            centre[a] = (mv->mins[a] + mv->maxs[a]) * 0.5f;
            half[a] = (mv->maxs[a] - mv->mins[a]) * 0.5f - 1.0f;
            if (half[a] < 1.0f)
                half[a] = 1.0f;
        }
        const float hull_mins[3] = { -half[0], -half[1], -half[2] };
        const float hull_maxs[3] = { half[0], half[1], half[2] };

        for (uint32_t s = 0; s + 1 < mv->num_stops && !mv->obstructed; s++) {
            float from[3], to[3];
            for (int a = 0; a < 3; a++) {
                from[a] = centre[a] + mv->stop[s][a];
                to[a] = centre[a] + mv->stop[s + 1][a];
            }
            mapgen_trace_result_t hit;
            MapGenTrace_Box(ctx, from, to, hull_mins, hull_maxs,
                            MAPGEN_TRACE_SOLID, &hit);
            /* Starting solid is where it was drawn and is not an obstruction;
               being stopped part way along is. */
            if (!hit.startsolid && hit.fraction < 0.999f)
                mv->obstructed = true;
        }
        if (mv->obstructed)
            mv->inoperable = true;
    }
}

uint64_t MapGenMovers_Fired(const mapgen_movers_t *m, const char *target)
{
    if (!m || !target || !*target)
        return 0;

    const char *names[MAPGEN_MOVER_RELAYS + 1];
    const uint32_t count = firing_closure(m, target, names,
                                          MAPGEN_MOVER_RELAYS + 1);

    uint64_t mask = 0;
    for (uint32_t n = 0; n < count; n++) {
        for (uint32_t i = 0; i < m->num_movers && i < 64; i++) {
            if (!strcmp(m->movers[i].targetname, names[n]))
                mask |= (uint64_t)1 << i;
        }
    }
    return mask;
}
