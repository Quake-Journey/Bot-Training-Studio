/*
 * MapGenDemo - see inc/common/mapgen_demo.h.
 *
 * This file contains no protocol. Every byte of the stream is read by the
 * engine's own reentrant offline decoder, which already owns the delta rules
 * and the two protocol families; what is written here is the sink that turns
 * its neutral events into a bounded canonical trace, and the arithmetic that
 * decides what a run of playerstates means.
 *
 * The arithmetic is deliberately conservative. Ten samples a second is a
 * coarse thing to reason about: a jump and a step off a kerb look alike at a
 * hundred milliseconds apart, and two frames of a lift ride look like standing
 * still. So every classification below is stated with the reason it is safe to
 * make at that rate, and where it cannot be made safely nothing is claimed.
 */

#include "common/mapgen_demo.h"
#include "common/mapgen_digest.h"

#include "shared/shared.h"
#include "client/demo_offline_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *MapGenDemo_ResultName(mapgen_demo_result_t r)
{
    switch (r) {
    case MAPGEN_DEMO_OK:               return "OK";
    case MAPGEN_DEMO_ERR_ARGS:         return "ERR_ARGS";
    case MAPGEN_DEMO_ERR_MEMORY:       return "ERR_MEMORY";
    case MAPGEN_DEMO_ERR_OPEN:         return "ERR_OPEN";
    case MAPGEN_DEMO_ERR_FORMAT:       return "ERR_FORMAT";
    case MAPGEN_DEMO_ERR_TRUNCATED:    return "ERR_TRUNCATED";
    case MAPGEN_DEMO_ERR_UNSUPPORTED:  return "ERR_UNSUPPORTED";
    case MAPGEN_DEMO_ERR_MAP_MISMATCH: return "ERR_MAP_MISMATCH";
    case MAPGEN_DEMO_ERR_NO_POV:       return "ERR_NO_POV";
    case MAPGEN_DEMO_ERR_AMBIGUOUS:    return "ERR_AMBIGUOUS";
    case MAPGEN_DEMO_ERR_PROVENANCE_TOO_LONG:
        return "ERR_PROVENANCE_TOO_LONG";
    case MAPGEN_DEMO_ERR_EMPTY:        return "ERR_EMPTY";
    }
    return "ERR_UNKNOWN";
}

const char *MapGenDemo_EventName(mapgen_demo_event_kind_t k)
{
    switch (k) {
    case MAPGEN_DEMO_JUMP:     return "jump";
    case MAPGEN_DEMO_LAND:     return "land";
    case MAPGEN_DEMO_DROP:     return "drop";
    case MAPGEN_DEMO_WARP: return "warp";
    case MAPGEN_DEMO_RIDE:     return "ride";
    case MAPGEN_DEMO_EVENT_KINDS: break;
    }
    return "?";
}

/* ---- the document ----------------------------------------------------------- */

#define CELL_BUCKETS 65536u

struct mapgen_demo_s {
    mapgen_demo_provenance_t prov;

    mapgen_demo_cell_t  *cells;
    uint32_t             num_cells;
    uint32_t             cap_cells;
    uint32_t            *cell_head;      /* CELL_BUCKETS entries            */
    uint32_t            *cell_next;

    mapgen_demo_move_t  *moves;
    uint32_t             num_moves;
    uint32_t             cap_moves;
    uint32_t            *move_head;      /* keyed on `from`                 */
    uint32_t            *move_next;

    mapgen_demo_event_t *events;
    uint32_t             num_events;
    uint32_t             cap_events;
    uint32_t             event_counts[MAPGEN_DEMO_EVENT_KINDS];
};

static uint32_t cell_hash(const int32_t c[3])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; i++)
        h = (h ^ (uint32_t)c[i]) * 16777619u;
    return h & (CELL_BUCKETS - 1u);
}

static void quantize(const float p[3], int32_t out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = (int32_t)floorf(p[i] / MAPGEN_DEMO_CELL);
}

static uint32_t intern_cell(mapgen_demo_t *d, const int32_t c[3])
{
    const uint32_t bucket = cell_hash(c);
    for (uint32_t at = d->cell_head[bucket]; at != UINT32_MAX;
         at = d->cell_next[at]) {
        if (d->cells[at].cell[0] == c[0] && d->cells[at].cell[1] == c[1] &&
            d->cells[at].cell[2] == c[2])
            return at;
    }
    if (d->num_cells >= d->cap_cells) {
        const uint32_t want = d->cap_cells ? d->cap_cells * 2 : 4096;
        if (want > MAPGEN_DEMO_MAX_CELLS)
            return UINT32_MAX;
        mapgen_demo_cell_t *cells = realloc(d->cells, want * sizeof(*cells));
        uint32_t *next = realloc(d->cell_next, want * sizeof(*next));
        if (!cells || !next) {
            free(cells ? cells : d->cells);
            free(next ? next : d->cell_next);
            d->cells = NULL;
            d->cell_next = NULL;
            return UINT32_MAX;
        }
        d->cells = cells;
        d->cell_next = next;
        d->cap_cells = want;
    }
    const uint32_t id = d->num_cells++;
    memset(&d->cells[id], 0, sizeof(d->cells[id]));
    memcpy(d->cells[id].cell, c, sizeof(d->cells[id].cell));
    d->cell_next[id] = d->cell_head[bucket];
    d->cell_head[bucket] = id;
    return id;
}

static bool note_move(mapgen_demo_t *d, uint32_t from, uint32_t to)
{
    if (from == to)
        return true;
    const uint32_t bucket = from & (CELL_BUCKETS - 1u);
    for (uint32_t at = d->move_head[bucket]; at != UINT32_MAX;
         at = d->move_next[at]) {
        if (d->moves[at].from == from && d->moves[at].to == to) {
            d->moves[at].seen++;
            return true;
        }
    }
    if (d->num_moves >= d->cap_moves) {
        const uint32_t want = d->cap_moves ? d->cap_moves * 2 : 8192;
        if (want > MAPGEN_DEMO_MAX_MOVES)
            return false;
        mapgen_demo_move_t *moves = realloc(d->moves, want * sizeof(*moves));
        uint32_t *next = realloc(d->move_next, want * sizeof(*next));
        if (!moves || !next) {
            free(moves ? moves : d->moves);
            free(next ? next : d->move_next);
            d->moves = NULL;
            d->move_next = NULL;
            return false;
        }
        d->moves = moves;
        d->move_next = next;
        d->cap_moves = want;
    }
    const uint32_t id = d->num_moves++;
    d->moves[id].from = from;
    d->moves[id].to = to;
    d->moves[id].seen = 1;
    d->move_next[id] = d->move_head[bucket];
    d->move_head[bucket] = id;
    return true;
}

static void note_event(mapgen_demo_t *d, mapgen_demo_event_kind_t kind,
                       int32_t time_ms, const float origin[3], float magnitude)
{
    d->event_counts[kind]++;
    if (d->num_events >= d->cap_events) {
        const uint32_t want = d->cap_events ? d->cap_events * 2 : 1024;
        if (want > MAPGEN_DEMO_MAX_EVENTS)
            return;             /* counted, not listed: the cap is a bound */
        mapgen_demo_event_t *ev = realloc(d->events, want * sizeof(*ev));
        if (!ev)
            return;
        d->events = ev;
        d->cap_events = want;
    }
    mapgen_demo_event_t *e = &d->events[d->num_events++];
    e->kind = kind;
    e->time_ms = time_ms;
    memcpy(e->origin, origin, sizeof(e->origin));
    e->magnitude = magnitude;
}

/* ---- the sink ---------------------------------------------------------------- */

typedef struct {
    mapgen_demo_t *d;
    const char    *expect_map;

    uint32_t       segments;
    bool           have_map;
    bool           map_mismatch;
    bool           provenance_too_long;

    int32_t        first_time;
    bool           have_previous;
    float          previous[3];
    uint32_t       previous_cell;
    int32_t        previous_time;
    bool           previous_ground;
    float          fall_from;      /* the height a descent began at         */
    bool           falling;
    bool           overflowed;
} ingest_t;

/*
 * The map is found by what the string SAYS, not by which slot it arrived in.
 *
 * CS_MODELS moves: the extended protocols remap the configstring bands, and
 * the decoder deliberately reports the raw index rather than a normalised one.
 * A hardcoded slot therefore reads a player's name as a map name on one
 * protocol and nothing at all on another. "maps/<something>.bsp" is
 * unambiguous and does not move.
 */
static bool is_world_model(const char *text)
{
    if (!text || strncmp(text, "maps/", 5))
        return false;
    const size_t n = strlen(text);
    return n > 9 && !Q_strcasecmp(text + n - 4, ".bsp");
}

/*
 * Copy a provenance field, or say it does not fit.
 *
 * A trimmed identity is a DIFFERENT identity: a map name cut short is another
 * map's name, and a source path cut in the middle is a file nobody can find
 * again. Both would be recorded as though they were the identity of the
 * evidence, which is worse than having no evidence.
 */
static bool copy_field(char *dst, size_t size, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return true;
    }
    const size_t n = strlen(src);
    if (n >= size)
        return false;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

/* "maps/q2dm1.bsp" -> "q2dm1" */
static void map_name_of(const char *model, char *out, size_t size)
{
    const char *slash = strrchr(model, '/');
    const char *name = slash ? slash + 1 : model;
    size_t n = strlen(name);
    if (n > 4 && !Q_strcasecmp(name + n - 4, ".bsp"))
        n -= 4;
    if (n >= size)
        n = size - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

static void on_segment(void *ud, const dof_segment_info_t *info)
{
    ingest_t *in = (ingest_t *)ud;
    in->segments++;
    in->d->prov.protocol = info->protocol;
    in->d->prov.pov_slot = info->client_num;
    if (!copy_field(in->d->prov.gamedir, sizeof(in->d->prov.gamedir),
                    info->gamedir))
        in->provenance_too_long = true;
    /* A new segment is a new connection: the trajectory does not continue
       across it, whatever the timestamps say. */
    in->have_previous = false;
    in->falling = false;
}

static void on_configstring(void *ud, int time_ms, int index, const char *text)
{
    ingest_t *in = (ingest_t *)ud;
    (void)time_ms;
    (void)index;
    if (!is_world_model(text))
        return;
    char name[MAPGEN_DEMO_NAME];
    map_name_of(text, name, sizeof(name));
    if (in->have_map && Q_strcasecmp(in->d->prov.map, name)) {
        /* Two different maps in one file: which one the movement belongs to
           is not knowable, so neither is claimed. */
        in->map_mismatch = true;
        return;
    }
    if (!copy_field(in->d->prov.map, sizeof(in->d->prov.map), name)) {
        in->provenance_too_long = true;
        return;
    }
    in->have_map = true;
    if (in->expect_map && Q_strcasecmp(name, in->expect_map))
        in->map_mismatch = true;
}

/*
 * One observation of where the recorder was.
 *
 * Everything below is derived from a PAIR of samples about a tenth of a second
 * apart, which is what bounds the claims:
 *
 *   OCCUPANCY and CONTACT are direct readings and are always safe.
 *
 *   A JUMP is the frame the ground flag clears with upward velocity. At this
 *   rate the peak is usually missed, so the height recorded is the velocity he
 *   left with and not how high he got.
 *
 *   A DROP is a descent that ended: the height between where a fall began and
 *   where the ground flag came back. Recorded on landing rather than on
 *   leaving, because the interesting number is how far it was.
 *
 *   A WARP is a discontinuity: a displacement no velocity could have produced
 *   in the time between two samples, or the engine's own teleport flag. It is
 *   NOT called a teleport, because a respawn is one too - the server sets the
 *   same flag - and q2dm1 has no teleporters in it at all. What the event
 *   means is only that the trajectory does not continue here, which is exactly
 *   what a consumer needs to know and all a demo can support.
 *
 *   A RIDE is being carried: on the ground, moving, with almost no velocity of
 *   his own. That is what standing on a lift looks like from the outside, and
 *   it is the only one of these that a demo shows more clearly than a
 *   simulation does.
 */
static void on_player_state(void *ud, const dof_player_state_t *ps)
{
    ingest_t *in = (ingest_t *)ud;
    if (!ps->pov)
        return;                 /* another player's state is not a trajectory */
    if (ps->pm_type != PM_NORMAL) {
        /*
         * Dead, spectating or frozen is not movement - and it BREAKS the
         * trajectory rather than being skipped over. Joining the frame before
         * a death to the frame after the respawn produces a displacement no
         * player could have made, which the teleport test then believes: q2dm1
         * has no teleporters and reported forty-eight of them in one match.
         */
        in->have_previous = false;
        in->falling = false;
        return;
    }

    mapgen_demo_t *d = in->d;
    /*
     * Elapsed, not the clock reading. The decoder's timestamps are the
     * server's own and a match recorded on a server that had been up for an
     * hour starts at an hour; taking the last one as a duration turned
     * fifty-three duels into eighty-six hours of play.
     */
    if (!d->prov.samples)
        in->first_time = ps->time_ms;
    d->prov.samples++;
    const int32_t elapsed = ps->time_ms - in->first_time;
    if (elapsed > d->prov.duration_ms)
        d->prov.duration_ms = elapsed;

    int32_t c[3];
    quantize(ps->origin, c);
    const uint32_t cell = intern_cell(d, c);
    if (cell == UINT32_MAX) {
        in->overflowed = true;
        return;
    }

    const bool on_ground = (ps->pm_flags & PMF_ON_GROUND) != 0;
    d->cells[cell].frames++;
    if (on_ground)
        d->cells[cell].contacts++;

    if (in->have_previous) {
        const float step[3] = { ps->origin[0] - in->previous[0],
                                ps->origin[1] - in->previous[1],
                                ps->origin[2] - in->previous[2] };
        const float distance = sqrtf(step[0] * step[0] + step[1] * step[1] +
                                     step[2] * step[2]);
        const int32_t dt = ps->time_ms - in->previous_time;

        /* Farther than any running player covers in the gap, or the engine
           said so outright. 1500 units a second is well above the game's own
           ceiling and still below a room-to-room jump. */
        const bool teleported =
            (ps->pm_flags & PMF_TIME_TELEPORT) ||
            (dt > 0 && dt < 1000 && distance > 1.5f * (float)dt);

        if (teleported) {
            note_event(d, MAPGEN_DEMO_WARP, ps->time_ms, ps->origin,
                       distance);
            in->falling = false;
        } else {
            if (!note_move(d, in->previous_cell, cell))
                in->overflowed = true;

            const float speed = sqrtf(ps->velocity[0] * ps->velocity[0] +
                                      ps->velocity[1] * ps->velocity[1]);
            const float travelled = sqrtf(step[0] * step[0] +
                                          step[1] * step[1]);
            if (on_ground && in->previous_ground && travelled > 8.0f &&
                speed < 20.0f)
                note_event(d, MAPGEN_DEMO_RIDE, ps->time_ms, ps->origin,
                           travelled);

            if (in->previous_ground && !on_ground) {
                if (ps->velocity[2] > 100.0f)
                    note_event(d, MAPGEN_DEMO_JUMP, ps->time_ms, ps->origin,
                               ps->velocity[2]);
                in->falling = true;
                in->fall_from = in->previous[2];
            } else if (!in->previous_ground && on_ground) {
                note_event(d, MAPGEN_DEMO_LAND, ps->time_ms, ps->origin, 0.0f);
                if (in->falling) {
                    const float fell = in->fall_from - ps->origin[2];
                    if (fell > 64.0f)
                        note_event(d, MAPGEN_DEMO_DROP, ps->time_ms,
                                   ps->origin, fell);
                }
                in->falling = false;
            } else if (in->falling && ps->origin[2] > in->fall_from) {
                in->fall_from = ps->origin[2];
            }
        }
    }

    memcpy(in->previous, ps->origin, sizeof(in->previous));
    in->previous_cell = cell;
    in->previous_time = ps->time_ms;
    in->previous_ground = on_ground;
    in->have_previous = true;
}

/* ---- reading a file ---------------------------------------------------------- */

typedef struct {
    FILE           *fp;
    mapgen_sha256_t sha;
} file_reader_t;

static int file_read(void *ud, void *dst, int len)
{
    file_reader_t *r = (file_reader_t *)ud;
    if (len <= 0)
        return 0;
    const size_t got = fread(dst, 1, (size_t)len, r->fp);
    if (got)
        MapGenDigest_Sha256Update(&r->sha, dst, got);
    return (int)got;
}

static mapgen_demo_t *new_demo(void)
{
    mapgen_demo_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->cell_head = malloc(CELL_BUCKETS * sizeof(*d->cell_head));
    d->move_head = malloc(CELL_BUCKETS * sizeof(*d->move_head));
    if (!d->cell_head || !d->move_head) {
        MapGenDemo_Free(d);
        return NULL;
    }
    for (uint32_t i = 0; i < CELL_BUCKETS; i++) {
        d->cell_head[i] = UINT32_MAX;
        d->move_head[i] = UINT32_MAX;
    }
    return d;
}

void MapGenDemo_Free(mapgen_demo_t *d)
{
    if (!d)
        return;
    free(d->cells);
    free(d->cell_head);
    free(d->cell_next);
    free(d->moves);
    free(d->move_head);
    free(d->move_next);
    free(d->events);
    free(d);
}

mapgen_demo_result_t MapGenDemo_Read(const char *path, const char *expect_map,
                                     const char *source, mapgen_demo_t **out)
{
    if (out)
        *out = NULL;
    if (!path || !out)
        return MAPGEN_DEMO_ERR_ARGS;

    file_reader_t fr;
    fr.fp = fopen(path, "rb");
    if (!fr.fp)
        return MAPGEN_DEMO_ERR_OPEN;
    MapGenDigest_Sha256Init(&fr.sha);

    mapgen_demo_t *d = new_demo();
    if (!d) {
        fclose(fr.fp);
        return MAPGEN_DEMO_ERR_MEMORY;
    }
    if (!copy_field(d->prov.source, sizeof(d->prov.source),
                    source ? source : path)) {
        MapGenDemo_Free(d);
        fclose(fr.fp);
        return MAPGEN_DEMO_ERR_PROVENANCE_TOO_LONG;
    }

    ingest_t in;
    memset(&in, 0, sizeof(in));
    in.d = d;
    in.expect_map = expect_map;

    const dof_reader_t reader = { &fr, file_read };
    const dof_sink_t sink = {
        .ud = &in,
        .segment_start = on_segment,
        .configstring = on_configstring,
        .player_state = on_player_state,
    };

    uint32_t quality = 0;
    const int rc = DOF_Decode(&reader, &sink, &quality);
    fclose(fr.fp);

    uint8_t sha[MAPGEN_SHA256_BYTES];
    MapGenDigest_Sha256Final(&fr.sha, sha);
    MapGenDigest_Sha256Hex(sha, d->prov.digest);
    d->prov.quality = quality;

    /*
     * Rejections are explicit and each says which one it was. A corpus that
     * silently drops what it could not read reports agreement it has not
     * earned, and the whole point of this is to be evidence.
     */
    mapgen_demo_result_t verdict = MAPGEN_DEMO_OK;
    if (rc == DOF_ERR_OPEN)
        verdict = MAPGEN_DEMO_ERR_OPEN;
    /*
     * The NAMED reasons before the general one.
     *
     * A protocol this build does not know makes the decoder mark the stream
     * unsupported AND stop, which is also a format error - and asking about
     * the format first threw the specific answer away, so a demo from another
     * build came back indistinguishable from a corrupt file.
     */
    else if (quality & DOF_Q_UNSUPPORTED)
        verdict = MAPGEN_DEMO_ERR_UNSUPPORTED;
    else if (rc == DOF_ERR_TRUNCATED || (quality & DOF_Q_TRUNCATED))
        verdict = MAPGEN_DEMO_ERR_TRUNCATED;
    else if (rc == DOF_ERR_FORMAT)
        verdict = MAPGEN_DEMO_ERR_FORMAT;
    else if (rc < 0)
        verdict = MAPGEN_DEMO_ERR_UNSUPPORTED;
    else if (in.provenance_too_long)
        verdict = MAPGEN_DEMO_ERR_PROVENANCE_TOO_LONG;
    else if (in.map_mismatch)
        verdict = MAPGEN_DEMO_ERR_MAP_MISMATCH;
    else if (in.segments > 1 && !in.have_map)
        verdict = MAPGEN_DEMO_ERR_AMBIGUOUS;
    else if (!d->prov.samples)
        verdict = MAPGEN_DEMO_ERR_NO_POV;
    else if (!d->num_cells)
        verdict = MAPGEN_DEMO_ERR_EMPTY;

    if (verdict != MAPGEN_DEMO_OK) {
        /* Handed back anyway, because the provenance of a rejected stream is
           part of the record: which file, which bytes, and why. */
        *out = d;
        return verdict;
    }
    *out = d;
    return MAPGEN_DEMO_OK;
}

const mapgen_demo_provenance_t *MapGenDemo_Provenance(const mapgen_demo_t *d)
{
    return d ? &d->prov : NULL;
}

uint32_t MapGenDemo_NumCells(const mapgen_demo_t *d)
{
    return d ? d->num_cells : 0;
}

const mapgen_demo_cell_t *MapGenDemo_Cell(const mapgen_demo_t *d, uint32_t i)
{
    return d && i < d->num_cells ? &d->cells[i] : NULL;
}

uint32_t MapGenDemo_NumMoves(const mapgen_demo_t *d)
{
    return d ? d->num_moves : 0;
}

const mapgen_demo_move_t *MapGenDemo_Move(const mapgen_demo_t *d, uint32_t i)
{
    return d && i < d->num_moves ? &d->moves[i] : NULL;
}

uint32_t MapGenDemo_NumEvents(const mapgen_demo_t *d)
{
    return d ? d->num_events : 0;
}

const mapgen_demo_event_t *MapGenDemo_Event(const mapgen_demo_t *d, uint32_t i)
{
    return d && i < d->num_events ? &d->events[i] : NULL;
}

uint32_t MapGenDemo_EventCount(const mapgen_demo_t *d,
                               mapgen_demo_event_kind_t kind)
{
    if (!d || kind >= MAPGEN_DEMO_EVENT_KINDS)
        return 0;
    return d->event_counts[kind];
}

/* ---- canonical text ---------------------------------------------------------- */

static int compare_cells(const void *a, const void *b)
{
    const mapgen_demo_cell_t *x = a, *y = b;
    for (int i = 0; i < 3; i++) {
        if (x->cell[i] != y->cell[i])
            return x->cell[i] < y->cell[i] ? -1 : 1;
    }
    return 0;
}

uint32_t MapGenDemo_CanonicalText(const mapgen_demo_t *d, char *out,
                                  uint32_t size)
{
    if (!d)
        return 0;

    /* Sorted, so the text does not depend on the order the hash table grew.
       A copy, because the document is const to its caller. */
    mapgen_demo_cell_t *sorted = NULL;
    if (d->num_cells) {
        sorted = malloc(d->num_cells * sizeof(*sorted));
        if (!sorted)
            return 0;
        memcpy(sorted, d->cells, d->num_cells * sizeof(*sorted));
        qsort(sorted, d->num_cells, sizeof(*sorted), compare_cells);
    }

    uint32_t written = 0;
    char line[512];

#define EMIT(...)                                                           \
    do {                                                                    \
        const int n = snprintf(line, sizeof(line), __VA_ARGS__);            \
        if (n > 0) {                                                        \
            if (out && written + (uint32_t)n < size)                        \
                memcpy(out + written, line, (size_t)n);                     \
            written += (uint32_t)n;                                         \
        }                                                                   \
    } while (0)

    EMIT("demo 1\n");
    EMIT("map %s\n", d->prov.map);
    EMIT("gamedir %s\n", d->prov.gamedir);
    EMIT("source %s\n", d->prov.source);
    EMIT("sha256 %s\n", d->prov.digest);
    EMIT("protocol %d\npov %d\nquality %u\n", d->prov.protocol,
         d->prov.pov_slot, d->prov.quality);
    EMIT("duration_ms %d\nsamples %u\n", d->prov.duration_ms, d->prov.samples);
    EMIT("cells %u\nmoves %u\n", d->num_cells, d->num_moves);
    for (uint32_t k = 0; k < MAPGEN_DEMO_EVENT_KINDS; k++)
        EMIT("%s %u\n", MapGenDemo_EventName((mapgen_demo_event_kind_t)k),
             d->event_counts[k]);
    for (uint32_t i = 0; i < d->num_cells; i++)
        EMIT("cell %d %d %d %u %u\n", sorted[i].cell[0], sorted[i].cell[1],
             sorted[i].cell[2], sorted[i].frames, sorted[i].contacts);

#undef EMIT

    free(sorted);
    if (out && size)
        out[written < size ? written : size - 1] = '\0';
    return written;
}

uint64_t MapGenDemo_Digest(const mapgen_demo_t *d)
{
    const uint32_t need = MapGenDemo_CanonicalText(d, NULL, 0);
    char *text = malloc((size_t)need + 1);
    if (!text)
        return 0;
    MapGenDemo_CanonicalText(d, text, need + 1);

    uint64_t hash = 1469598103934665603ull;      /* FNV-1a 64 offset basis */
    for (uint32_t i = 0; i < need; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 1099511628211ull;
    }
    free(text);
    return hash;
}
