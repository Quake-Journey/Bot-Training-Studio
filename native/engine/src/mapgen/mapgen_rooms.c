/*
 * MapGenRooms - see inc/common/mapgen_rooms.h.
 *
 * Three passes over one lattice.
 *
 *   1. Which cells are empty.
 *   2. How far each empty cell is from the nearest solid one - a distance
 *      transform, done as a breadth-first sweep out from the solid, which is
 *      exact on this lattice and linear in the number of cells.
 *   3. A watershed: take the empty cells in order of decreasing clearance and
 *      give each one the room of whichever wider neighbour it touches, or a
 *      new room if it is wider than everything around it and wide enough to be
 *      a room at all.
 *
 * The third pass is where the segmentation actually happens, and the reason it
 * is done in that order is that it makes the middles of rooms into seeds
 * automatically: the widest cell in a hall is processed before anything else
 * in the hall and has no wider neighbour to inherit from.
 */

#include "common/mapgen_rooms.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *MapGenRooms_ResultName(mapgen_rooms_result_t r)
{
    switch (r) {
    case MAPGEN_ROOMS_OK:            return "OK";
    case MAPGEN_ROOMS_ERR_ARGS:      return "ERR_ARGS";
    case MAPGEN_ROOMS_ERR_MEMORY:    return "ERR_MEMORY";
    case MAPGEN_ROOMS_ERR_TOO_LARGE: return "ERR_TOO_LARGE";
    case MAPGEN_ROOMS_ERR_NO_SPACE:  return "ERR_NO_SPACE";
    }
    return "ERR_UNKNOWN";
}

#define NO_ROOM UINT32_MAX

struct mapgen_rooms_s {
    int32_t  mins[3];
    int32_t  size[3];
    uint32_t count;

    uint8_t  *solid;
    uint16_t *clearance;    /* in cells, 0 for solid                        */
    uint32_t *room;         /* NO_ROOM for solid and for cells too tight    */

    mapgen_room_t      rooms[MAPGEN_ROOMS_MAX];
    uint32_t           num_rooms;
    mapgen_room_link_t links[MAPGEN_ROOMS_MAX_LINKS];
    uint32_t           num_links;
};

static const int32_t SIDES[6][3] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
    { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};

static int32_t cell_of(float v)
{
    return (int32_t)floorf(v / MAPGEN_ROOMS_CELL);
}

static int64_t index_of(const mapgen_rooms_t *r, int32_t x, int32_t y, int32_t z)
{
    if (x < 0 || y < 0 || z < 0 ||
        x >= r->size[0] || y >= r->size[1] || z >= r->size[2])
        return -1;
    return (int64_t)x + (int64_t)y * r->size[0]
         + (int64_t)z * r->size[0] * r->size[1];
}

static void centre_of(const mapgen_rooms_t *r, int64_t at, float out[3])
{
    const int32_t x = (int32_t)(at % r->size[0]);
    const int32_t y = (int32_t)((at / r->size[0]) % r->size[1]);
    const int32_t z = (int32_t)(at / ((int64_t)r->size[0] * r->size[1]));
    out[0] = (r->mins[0] + x + 0.5f) * MAPGEN_ROOMS_CELL;
    out[1] = (r->mins[1] + y + 0.5f) * MAPGEN_ROOMS_CELL;
    out[2] = (r->mins[2] + z + 0.5f) * MAPGEN_ROOMS_CELL;
}

/* ---- the three passes -------------------------------------------------------- */

static void rasterise(const mapgen_bsp_t *bsp, mapgen_rooms_t *r)
{
    for (int32_t z = 0; z < r->size[2]; z++) {
      for (int32_t y = 0; y < r->size[1]; y++) {
        for (int32_t x = 0; x < r->size[0]; x++) {
            const float p[3] = {
                (r->mins[0] + x + 0.5f) * MAPGEN_ROOMS_CELL,
                (r->mins[1] + y + 0.5f) * MAPGEN_ROOMS_CELL,
                (r->mins[2] + z + 0.5f) * MAPGEN_ROOMS_CELL,
            };
            /* Solid stops a player; water and lava do not, and a flooded room
               is still a room. */
            if (MapGenBsp_PointContents(bsp, p) & 1)
                r->solid[index_of(r, x, y, z)] = 1;
        }
      }
    }
}

/*
 * How far every empty cell is from solid, in cells.
 *
 * A breadth-first sweep out from every solid cell at once. On a lattice with
 * six-connected neighbours this is the exact Manhattan-along-the-grid distance
 * and it costs one visit per cell, which matters: q2dm1 is two million of
 * them and an exact Euclidean transform is not worth the difference here.
 */
static bool measure_clearance(mapgen_rooms_t *r)
{
    uint32_t *queue = malloc((size_t)r->count * sizeof(*queue));
    if (!queue)
        return false;

    uint32_t tail = 0, cursor = 0;
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->solid[i]) {
            r->clearance[i] = 0;
            queue[tail++] = i;
        } else {
            r->clearance[i] = UINT16_MAX;
        }
    }

    while (cursor < tail) {
        const uint32_t at = queue[cursor++];
        const int32_t x = (int32_t)(at % (uint32_t)r->size[0]);
        const int32_t y = (int32_t)((at / (uint32_t)r->size[0]) % (uint32_t)r->size[1]);
        const int32_t z = (int32_t)(at / ((uint32_t)r->size[0] * (uint32_t)r->size[1]));
        for (int s = 0; s < 6; s++) {
            const int64_t next = index_of(r, x + SIDES[s][0], y + SIDES[s][1],
                                          z + SIDES[s][2]);
            if (next < 0)
                continue;
            if (r->clearance[next] > r->clearance[at] + 1) {
                r->clearance[next] = (uint16_t)(r->clearance[at] + 1);
                queue[tail++] = (uint32_t)next;
            }
        }
    }
    free(queue);
    return true;
}

/*
 * The sort carries its key with it.
 *
 * Sorting bare indices needs the clearance array inside the comparison, which
 * means a file-scope pointer, which means this file could not be run twice at
 * once - and every other module here is reentrant on purpose. A pair costs
 * four more bytes a cell and keeps that property.
 */
typedef struct {
    uint16_t clearance;
    uint32_t at;
} ranked_t;

static int by_clearance(const void *a, const void *b)
{
    const ranked_t *x = a, *y = b;
    if (x->clearance != y->clearance)
        return x->clearance > y->clearance ? -1 : 1;
    return x->at < y->at ? -1 : (x->at > y->at ? 1 : 0);
}

/*
 * The watershed.
 *
 * Widest first. A cell joins whichever already-assigned neighbour is widest;
 * if nothing next to it is assigned and it is wide enough to be the middle of
 * a room, it starts one. Cells narrower than the threshold still get assigned -
 * a corridor belongs to the room it leads out of - which is what makes the
 * boundary between two rooms fall in the middle of the passage between them,
 * where it belongs.
 */
static bool segment(mapgen_rooms_t *r, float min_clearance)
{
    const uint16_t seed_at = (uint16_t)(min_clearance / MAPGEN_ROOMS_CELL);

    uint32_t empty = 0;
    for (uint32_t i = 0; i < r->count; i++) {
        r->room[i] = NO_ROOM;
        if (!r->solid[i])
            empty++;
    }
    if (!empty)
        return false;

    ranked_t *order = malloc((size_t)empty * sizeof(*order));
    if (!order)
        return false;
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->count; i++) {
        if (!r->solid[i]) {
            order[n].clearance = r->clearance[i];
            order[n].at = i;
            n++;
        }
    }
    qsort(order, n, sizeof(*order), by_clearance);

    for (uint32_t k = 0; k < n; k++) {
        const uint32_t at = order[k].at;
        const int32_t x = (int32_t)(at % (uint32_t)r->size[0]);
        const int32_t y = (int32_t)((at / (uint32_t)r->size[0]) % (uint32_t)r->size[1]);
        const int32_t z = (int32_t)(at / ((uint32_t)r->size[0] * (uint32_t)r->size[1]));

        uint32_t best = NO_ROOM;
        uint16_t widest = 0;
        uint16_t tallest_neighbour = 0;
        for (int s = 0; s < 6; s++) {
            const int64_t next = index_of(r, x + SIDES[s][0], y + SIDES[s][1],
                                          z + SIDES[s][2]);
            if (next < 0)
                continue;
            if (r->clearance[next] > tallest_neighbour)
                tallest_neighbour = r->clearance[next];
            if (r->room[next] == NO_ROOM)
                continue;
            if (r->clearance[next] >= widest) {
                widest = r->clearance[next];
                best = r->room[next];
            }
        }

        if (best != NO_ROOM) {
            r->room[at] = best;
        } else if (r->clearance[at] >= seed_at &&
                   /*
                    * A seed has to be a local maximum.
                    *
                    * Without this, any cell that happened to be reached before
                    * its neighbours started a room of its own: q2dm1 came out
                    * with two hundred and fifty-six of them, which was the cap,
                    * and their bounding boxes all lay on top of each other.
                    * On a flat plateau - the middle of a hall, where a lot of
                    * cells share the widest clearance there is - the first one
                    * processed seeds it and the rest of the plateau inherits,
                    * because a plateau is connected.
                    */
                   r->clearance[at] >= tallest_neighbour &&
                   r->num_rooms < MAPGEN_ROOMS_MAX) {
            r->room[at] = r->num_rooms;
            mapgen_room_t *room = &r->rooms[r->num_rooms++];
            memset(room, 0, sizeof(*room));
            room->clearance = r->clearance[at] * MAPGEN_ROOMS_CELL;
            float c[3];
            centre_of(r, at, c);
            memcpy(room->mins, c, sizeof(room->mins));
            memcpy(room->maxs, c, sizeof(room->maxs));
        }
        /* Anything narrower than the seed threshold with nothing assigned
           around it yet is left alone; a later pass over the same sorted order
           would pick it up, and in practice it is the inside of a solid pocket
           nobody can reach. */
    }
    free(order);
    return true;
}

/* ---- the shape of what was found --------------------------------------------- */

static void measure_rooms(mapgen_rooms_t *r)
{
    for (uint32_t i = 0; i < r->count; i++) {
        const uint32_t which = r->room[i];
        if (which == NO_ROOM)
            continue;
        mapgen_room_t *room = &r->rooms[which];
        float c[3];
        centre_of(r, i, c);
        if (!room->cells) {
            memcpy(room->mins, c, sizeof(room->mins));
            memcpy(room->maxs, c, sizeof(room->maxs));
        }
        for (int a = 0; a < 3; a++) {
            if (c[a] < room->mins[a])
                room->mins[a] = c[a];
            if (c[a] > room->maxs[a])
                room->maxs[a] = c[a];
        }
        room->cells++;
        const float clear = r->clearance[i] * MAPGEN_ROOMS_CELL;
        if (clear > room->clearance)
            room->clearance = clear;
    }
    for (uint32_t i = 0; i < r->num_rooms; i++) {
        mapgen_room_t *room = &r->rooms[i];
        for (int a = 0; a < 3; a++)
            room->centre[a] = (room->mins[a] + room->maxs[a]) * 0.5f;
    }
}

/*
 * Where two rooms meet, and how wide it is there.
 *
 * The narrowest point on the boundary between them: a doorway is exactly the
 * place where the flood from one room ran into the flood from the other, and
 * the clearance there is half the width of the opening.
 */
static void find_links(mapgen_rooms_t *r)
{
    for (int32_t z = 0; z < r->size[2]; z++) {
      for (int32_t y = 0; y < r->size[1]; y++) {
        for (int32_t x = 0; x < r->size[0]; x++) {
            const int64_t at = index_of(r, x, y, z);
            const uint32_t mine = r->room[at];
            if (mine == NO_ROOM)
                continue;
            for (int s = 0; s < 6; s += 2) {   /* +x, +y, +z only: each pair once */
                const int64_t next = index_of(r, x + SIDES[s][0],
                                              y + SIDES[s][1], z + SIDES[s][2]);
                if (next < 0)
                    continue;
                const uint32_t theirs = r->room[next];
                if (theirs == NO_ROOM || theirs == mine)
                    continue;

                const float width = 2.0f * MAPGEN_ROOMS_CELL *
                    (float)(r->clearance[at] < r->clearance[next]
                            ? r->clearance[at] : r->clearance[next]);

                const uint32_t a = mine < theirs ? mine : theirs;
                const uint32_t b = mine < theirs ? theirs : mine;
                mapgen_room_link_t *found = NULL;
                for (uint32_t l = 0; l < r->num_links; l++) {
                    if (r->links[l].a == a && r->links[l].b == b) {
                        found = &r->links[l];
                        break;
                    }
                }
                if (!found) {
                    if (r->num_links >= MAPGEN_ROOMS_MAX_LINKS)
                        continue;
                    found = &r->links[r->num_links++];
                    memset(found, 0, sizeof(*found));
                    found->a = a;
                    found->b = b;
                    found->width = width;      /* a running maximum from here */
                    centre_of(r, at, found->at);
                    r->rooms[a].links++;
                    r->rooms[b].links++;
                } else if (width > found->width) {
                    /*
                     * The WIDEST point of the boundary, not the narrowest.
                     *
                     * How wide a doorway is means the biggest thing that fits
                     * through it, and that is the widest point of the opening -
                     * the saddle between the two basins. Taking the narrowest
                     * point measures the corner where the boundary surface runs
                     * into the floor, which is nearly nothing for every pair of
                     * rooms in every map: q2dm1 came out with two hundred and
                     * thirty-four rooms because no two basins ever looked
                     * joined enough to be one.
                     */
                    found->width = width;
                    centre_of(r, at, found->at);
                }
                for (int k = 0; k < 3; k++)
                    found->normal[k] = (float)SIDES[s][k];
                if (mine != a) {
                    for (int k = 0; k < 3; k++)
                        found->normal[k] = -found->normal[k];
                }
            }
        }
      }
    }
}

/*
 * Two basins with nothing between them are one room.
 *
 * A watershed splits at every local maximum, and a hall with a pillar in the
 * middle of it has two. The test for whether a split is real is the passage
 * between them: a doorway is much narrower than the rooms it joins, and the
 * gap either side of a pillar is not narrower than anything. So where the way
 * between two rooms is nearly as wide as the narrower of them, they are the
 * same place seen twice.
 *
 * Merging is repeated until nothing changes, because merging A into B can make
 * B's link to C wide enough to merge as well - a row of pillars is one hall.
 */
/*
 * A basin is a room only if its middle is meaningfully wider than its entrance.
 *
 * Every local maximum of clearance starts a basin, and most of them are not
 * rooms - a bump beside a pillar, a widening where two corridors cross, the
 * pocket above a staircase. What separates a room from a bump is how far it
 * rises above the saddle it shares with its neighbour: that is the basin's own
 * property and does not depend on what else it is joined to.
 *
 * The links are taken widest saddle first, so the shallowest joins collapse
 * before the deep ones are considered, and the shallower of each pair is the
 * one that goes.
 */
static int by_saddle(const void *a, const void *b)
{
    const mapgen_room_link_t *x = a, *y = b;
    if (x->width != y->width)
        return x->width > y->width ? -1 : 1;
    if (x->a != y->a)
        return x->a < y->a ? -1 : 1;
    return x->b < y->b ? -1 : (x->b > y->b ? 1 : 0);
}

static void merge_shallow(mapgen_rooms_t *r, float persistence)
{
    uint32_t *into = malloc((size_t)MAPGEN_ROOMS_MAX * sizeof(*into));
    float *peak = malloc((size_t)MAPGEN_ROOMS_MAX * sizeof(*peak));
    if (!into || !peak) {
        free(into);
        free(peak);
        return;
    }
    for (uint32_t i = 0; i < r->num_rooms; i++) {
        into[i] = i;
        peak[i] = r->rooms[i].clearance;
    }

    mapgen_room_link_t *ordered = malloc((size_t)(r->num_links ? r->num_links : 1)
                                         * sizeof(*ordered));
    if (!ordered) {
        free(into);
        free(peak);
        return;
    }
    memcpy(ordered, r->links, (size_t)r->num_links * sizeof(*ordered));
    qsort(ordered, r->num_links, sizeof(*ordered), by_saddle);

    for (uint32_t l = 0; l < r->num_links; l++) {
        uint32_t a = ordered[l].a;
        uint32_t b = ordered[l].b;
        while (into[a] != a) a = into[a];
        while (into[b] != b) b = into[b];
        if (a == b)
            continue;

        const float saddle = ordered[l].width * 0.5f;
        const uint32_t shallower = peak[a] <= peak[b] ? a : b;
        const uint32_t deeper = shallower == a ? b : a;
        if (peak[shallower] - saddle >= persistence)
            continue;               /* it rises: it is a room of its own */

        into[shallower] = deeper;
        if (peak[shallower] > peak[deeper])
            peak[deeper] = peak[shallower];
    }
    free(ordered);

    /* Renumber into a dense set, carrying the measurements over. */
    uint32_t *renamed = malloc((size_t)MAPGEN_ROOMS_MAX * sizeof(*renamed));
    mapgen_room_t *kept = calloc(MAPGEN_ROOMS_MAX, sizeof(*kept));
    if (!renamed || !kept) {
        free(into);
        free(peak);
        free(renamed);
        free(kept);
        return;
    }

    uint32_t n = 0;
    for (uint32_t i = 0; i < r->num_rooms; i++) {
        uint32_t root = i;
        while (into[root] != root) root = into[root];
        if (root != i) {
            renamed[i] = UINT32_MAX;
            continue;
        }
        renamed[i] = n;
        kept[n] = r->rooms[i];
        kept[n].links = 0;
        n++;
    }
    for (uint32_t i = 0; i < r->num_rooms; i++) {
        if (renamed[i] != UINT32_MAX)
            continue;
        uint32_t root = i;
        while (into[root] != root) root = into[root];
        const uint32_t dst = renamed[root];
        renamed[i] = dst;
        mapgen_room_t *room = &kept[dst];
        const mapgen_room_t *from = &r->rooms[i];
        for (int a = 0; a < 3; a++) {
            if (from->mins[a] < room->mins[a]) room->mins[a] = from->mins[a];
            if (from->maxs[a] > room->maxs[a]) room->maxs[a] = from->maxs[a];
        }
        room->cells += from->cells;
        if (from->clearance > room->clearance)
            room->clearance = from->clearance;
    }

    for (uint32_t i = 0; i < r->count; i++) {
        if (r->room[i] != NO_ROOM)
            r->room[i] = renamed[r->room[i]];
    }
    memcpy(r->rooms, kept, (size_t)n * sizeof(*kept));
    r->num_rooms = n;
    for (uint32_t i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++)
            r->rooms[i].centre[a] =
                (r->rooms[i].mins[a] + r->rooms[i].maxs[a]) * 0.5f;
    }

    /* The links have to be rebuilt: the ones inside a merged room are gone. */
    r->num_links = 0;
    find_links(r);

    free(kept);
    free(renamed);
    free(peak);
    free(into);
}

/* ---- entities, so a bundle knows what has to go with it ---------------------- */

static void count_entities(const mapgen_bsp_t *bsp, mapgen_rooms_t *r)
{
    uint32_t length = 0;
    const char *text = MapGenBsp_Entities(bsp, &length);
    if (!text)
        return;

    const char *block = text;
    while ((block = strchr(block, '{')) != NULL) {
        const char *end = strchr(block, '}');
        if (!end)
            break;

        const char *cn = strstr(block, "\"classname\"");
        bool item = false, spawn = false;
        if (cn && cn < end) {
            const char *v = strchr(cn + 11, '"');
            if (v && v < end) {
                v++;
                item = !strncmp(v, "item_", 5) || !strncmp(v, "weapon_", 7)
                    || !strncmp(v, "ammo_", 5);
                spawn = !strncmp(v, "info_player_deathmatch", 22)
                     || !strncmp(v, "info_player_start", 17);
            }
        }
        if (item || spawn) {
            const char *o = strstr(block, "\"origin\"");
            if (o && o < end && (o = strchr(o + 8, '"')) != NULL && o < end) {
                char buf[128];
                const char *close = strchr(o + 1, '"');
                size_t n = close ? (size_t)(close - o - 1) : 0;
                if (n >= sizeof(buf))
                    n = sizeof(buf) - 1;
                memcpy(buf, o + 1, n);
                buf[n] = '\0';
                float p[3];
                char *cursor = buf;
                for (int a = 0; a < 3; a++)
                    p[a] = strtof(cursor, &cursor);
                const uint32_t which = MapGenRooms_At(r, p);
                if (which != NO_ROOM) {
                    if (item)
                        r->rooms[which].items++;
                    if (spawn) {
                        r->rooms[which].spawns++;
                        r->rooms[which].reachable = true;
                    }
                }
            }
        }
        block = end + 1;
    }
}

/* ---- the entry point --------------------------------------------------------- */

mapgen_rooms_result_t MapGenRooms_Find(const mapgen_bsp_t *bsp,
                                       float min_clearance,
                                       float persistence,
                                       mapgen_rooms_t **out)
{
    if (out)
        *out = NULL;
    if (!bsp || !out)
        return MAPGEN_ROOMS_ERR_ARGS;
    if (min_clearance < MAPGEN_ROOMS_CELL)
        min_clearance = MAPGEN_ROOMS_CELL * 2.0f;
    if (persistence <= 0.0f)
        persistence = MAPGEN_ROOMS_PERSISTENCE;

    const mapgen_bsp_model_t *world = MapGenBsp_Model(bsp, 0);
    if (!world)
        return MAPGEN_ROOMS_ERR_ARGS;

    mapgen_rooms_t *r = calloc(1, sizeof(*r));
    if (!r)
        return MAPGEN_ROOMS_ERR_MEMORY;

    uint64_t total = 1;
    for (int i = 0; i < 3; i++) {
        r->mins[i] = cell_of(world->mins[i]) - 1;
        r->size[i] = cell_of(world->maxs[i]) + 1 - r->mins[i] + 1;
        if (r->size[i] <= 0) {
            free(r);
            return MAPGEN_ROOMS_ERR_ARGS;
        }
        total *= (uint64_t)r->size[i];
        if (total > MAPGEN_ROOMS_MAX_CELLS) {
            free(r);
            return MAPGEN_ROOMS_ERR_TOO_LARGE;
        }
    }
    r->count = (uint32_t)total;

    r->solid = calloc(r->count, 1);
    r->clearance = malloc((size_t)r->count * sizeof(*r->clearance));
    r->room = malloc((size_t)r->count * sizeof(*r->room));
    if (!r->solid || !r->clearance || !r->room) {
        MapGenRooms_Free(r);
        return MAPGEN_ROOMS_ERR_MEMORY;
    }

    rasterise(bsp, r);
    if (!measure_clearance(r)) {
        MapGenRooms_Free(r);
        return MAPGEN_ROOMS_ERR_MEMORY;
    }
    if (!segment(r, min_clearance)) {
        MapGenRooms_Free(r);
        return MAPGEN_ROOMS_ERR_NO_SPACE;
    }
    measure_rooms(r);
    find_links(r);
    merge_shallow(r, persistence);
    count_entities(bsp, r);

    *out = r;
    return MAPGEN_ROOMS_OK;
}

void MapGenRooms_Free(mapgen_rooms_t *r)
{
    if (!r)
        return;
    free(r->solid);
    free(r->clearance);
    free(r->room);
    free(r);
}

uint32_t MapGenRooms_Count(const mapgen_rooms_t *r)
{
    return r ? r->num_rooms : 0;
}

const mapgen_room_t *MapGenRooms_Room(const mapgen_rooms_t *r, uint32_t i)
{
    return r && i < r->num_rooms ? &r->rooms[i] : NULL;
}

uint32_t MapGenRooms_NumLinks(const mapgen_rooms_t *r)
{
    return r ? r->num_links : 0;
}

const mapgen_room_link_t *MapGenRooms_Link(const mapgen_rooms_t *r, uint32_t i)
{
    return r && i < r->num_links ? &r->links[i] : NULL;
}

uint32_t MapGenRooms_At(const mapgen_rooms_t *r, const float point[3])
{
    if (!r || !point)
        return NO_ROOM;
    const int64_t at = index_of(r, cell_of(point[0]) - r->mins[0],
                                cell_of(point[1]) - r->mins[1],
                                cell_of(point[2]) - r->mins[2]);
    return at < 0 ? NO_ROOM : r->room[at];
}

/*
 * How much rock is behind that wall.
 *
 * Walked out from the room's own cells, one lattice step at a time: as long as
 * every cell across the face is solid, the wall can move that far. The moment
 * one of them is open, moving further would break into whatever is on the
 * other side, and the answer is however far it got.
 *
 * That is the whole safety argument for a reshape operator. Solid with more
 * solid behind it is rock and can be carved; solid with open space behind it
 * is a wall between two places and moving it changes both.
 */
float MapGenRooms_PushRoom(const mapgen_rooms_t *r, uint32_t room,
                           int axis, int sign)
{
    if (!r || room >= r->num_rooms || axis < 0 || axis > 2 || !sign)
        return 0.0f;
    const int step = sign > 0 ? 1 : -1;

    uint32_t reach = UINT32_MAX;
    for (int32_t z = 0; z < r->size[2]; z++) {
      for (int32_t y = 0; y < r->size[1]; y++) {
        for (int32_t x = 0; x < r->size[0]; x++) {
            const int64_t at = index_of(r, x, y, z);
            if (r->room[at] != room)
                continue;
            int32_t c[3] = { x, y, z };
            /* Only the cells actually AT the face of the room in that
               direction: an interior cell is behind another of its own. */
            int32_t ahead[3] = { x, y, z };
            ahead[axis] += step;
            const int64_t next = index_of(r, ahead[0], ahead[1], ahead[2]);
            if (next >= 0 && r->room[next] == room)
                continue;

            uint32_t depth = 0;
            for (;;) {
                c[axis] += step;
                const int64_t probe = index_of(r, c[0], c[1], c[2]);
                if (probe < 0 || !r->solid[probe])
                    break;
                depth++;
                if (depth > 64)
                    break;
            }
            if (depth < reach)
                reach = depth;
        }
      }
    }
    if (reach == UINT32_MAX)
        return 0.0f;
    /* One cell of rock is left standing, so the wall stays a wall. */
    return reach > 1 ? (float)(reach - 1) * MAPGEN_ROOMS_CELL : 0.0f;
}

uint32_t MapGenRooms_ClearanceHistogram(const mapgen_rooms_t *r,
                                        uint32_t *bins, uint32_t num_bins)
{
    if (!r || !bins || !num_bins)
        return 0;
    uint32_t open = 0;
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->solid[i])
            continue;
        open++;
        const uint32_t c = r->clearance[i];
        bins[c < num_bins ? c : num_bins - 1]++;
    }
    return open;
}

/* ---- canonical text ---------------------------------------------------------- */

uint32_t MapGenRooms_CanonicalText(const mapgen_rooms_t *r, char *out,
                                   uint32_t size)
{
    if (!r)
        return 0;
    uint32_t written = 0;
    char line[256];

#define EMIT(...)                                                            \
    do {                                                                     \
        const int n = snprintf(line, sizeof(line), __VA_ARGS__);             \
        if (n > 0) {                                                         \
            if (out && written + (uint32_t)n < size)                         \
                memcpy(out + written, line, (size_t)n);                      \
            written += (uint32_t)n;                                          \
        }                                                                    \
    } while (0)

    EMIT("rooms 1\ncount %u\nlinks %u\n", r->num_rooms, r->num_links);
    for (uint32_t i = 0; i < r->num_rooms; i++) {
        const mapgen_room_t *room = &r->rooms[i];
        EMIT("room %u %d %d %d %d %d %d %u %d %u %u %u\n", i,
             (int)room->mins[0], (int)room->mins[1], (int)room->mins[2],
             (int)room->maxs[0], (int)room->maxs[1], (int)room->maxs[2],
             room->cells, (int)room->clearance, room->links, room->items,
             room->spawns);
    }
    for (uint32_t i = 0; i < r->num_links; i++) {
        const mapgen_room_link_t *l = &r->links[i];
        EMIT("link %u %u %d %d %d %d\n", l->a, l->b, (int)l->at[0],
             (int)l->at[1], (int)l->at[2], (int)l->width);
    }
#undef EMIT

    if (out && size)
        out[written < size ? written : size - 1] = '\0';
    return written;
}

uint64_t MapGenRooms_Digest(const mapgen_rooms_t *r)
{
    const uint32_t need = MapGenRooms_CanonicalText(r, NULL, 0);
    char *text = malloc((size_t)need + 1);
    if (!text)
        return 0;
    MapGenRooms_CanonicalText(r, text, need + 1);
    uint64_t hash = 1469598103934665603ull;      /* FNV-1a 64 offset basis */
    for (uint32_t i = 0; i < need; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 1099511628211ull;
    }
    free(text);
    return hash;
}
