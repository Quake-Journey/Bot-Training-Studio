/*
 * MapGenMovers - the parts of a map that are not where they were drawn.
 *
 * A door, a lift, a train and a button are brush models: their geometry sits
 * in the file exactly where the mapper put it, and the game carries it around
 * at runtime. A reachability answer that ignores them is wrong in both
 * directions at once - it walks the player through closed doors, and it tells
 * him a floor he can only get to by lift is unreachable.
 *
 * So this reads them out of the entity string and says three things about each
 * one, which is all a reachability search needs to know:
 *
 *   where it can BE          - the stops, as displacements from where it was
 *                              drawn, so a trace can put it in any of them;
 *   who can MOVE it          - a player standing near it, a player who found a
 *                              button somewhere else, or nobody at all;
 *   whether it CARRIES       - a lift and a train take a player with them, and
 *                              that is a way to get somewhere, not an obstacle.
 *
 * What it deliberately does NOT do is simulate the game logic. There is no
 * timing, no queue, no partial travel: a mover is at one of its stops. The one
 * place that matters is stated where it is relied on.
 */

#ifndef MAPGEN_MOVERS_H
#define MAPGEN_MOVERS_H

#include <stdbool.h>
#include <stdint.h>

#include "common/mapgen_bsp.h"
#include "common/mapgen_trace.h"

#define MAPGEN_MOVER_NAME    64
#define MAPGEN_MOVER_STOPS   16
#define MAPGEN_MOVER_MAX     64
#define MAPGEN_MOVER_OPS     64
#define MAPGEN_MOVER_PORTALS 32
#define MAPGEN_MOVER_RELAYS  64

typedef enum {
    MAPGEN_MOVER_OTHER = 0,
    MAPGEN_MOVER_DOOR,
    MAPGEN_MOVER_PLAT,
    MAPGEN_MOVER_TRAIN,
    MAPGEN_MOVER_BUTTON,
    MAPGEN_MOVER_LIQUID
} mapgen_mover_kind_t;

const char *MapGenMovers_KindName(mapgen_mover_kind_t kind);

typedef struct {
    mapgen_mover_kind_t kind;
    uint32_t model;                      /* brush model index, from "*N"     */
    char     classname[MAPGEN_MOVER_NAME];
    char     targetname[MAPGEN_MOVER_NAME];
    char     target[MAPGEN_MOVER_NAME];

    float    mins[3];                    /* the model's bounds, as drawn     */
    float    maxs[3];

    /*
     * Where it can be, as displacements from where it was drawn. Stop 0 is
     * where it rests with nothing done to it, which for an untargeted lift is
     * the BOTTOM and for a door is closed.
     */
    float    stop[MAPGEN_MOVER_STOPS][3];
    uint32_t num_stops;

    /* A player who comes near is enough to work it - no button, no key. */
    bool     player_operated;
    /* A player standing on it goes where it goes. */
    bool     carries;
    /* Something has to fire it, and nothing in the map ever can - or its own
       path is blocked, which comes to the same thing for a player waiting on
       it. */
    bool     inoperable;
    /* The swept volume between two of its stops runs into the world. */
    bool     obstructed;
} mapgen_mover_t;

/*
 * A thing a player can reach that fires a targetname: a button, a touch
 * trigger, or a relay's own source. `volume` is the space he has to be in;
 * for a button that is the button's own brush, which he has to be next to.
 */
typedef struct {
    char     target[MAPGEN_MOVER_NAME];
    float    mins[3];
    float    maxs[3];
    bool     shootable;    /* a button that only takes damage, not a touch   */
} mapgen_mover_operator_t;

/* A one-way jump from a volume to a point. */
typedef struct {
    char     target[MAPGEN_MOVER_NAME];   /* the destination entity's name  */
    float    mins[3];
    float    maxs[3];
    float    destination[3];
    bool     has_destination;
} mapgen_mover_portal_t;

/*
 * A name that fires another name, with nothing in the world to stand next to.
 *
 * A `trigger_relay` has no brush and no place; it exists only to pass a firing
 * on. Leaving them out breaks the chain a mapper actually built - in q2dm3 the
 * three buttons fire a relay and the relay opens the doors, so a search that
 * only knows about buttons and doors reports two doors that nothing in the map
 * can ever open, and then reports the rooms behind them unreachable.
 */
typedef struct {
    char name[MAPGEN_MOVER_NAME];     /* fired when this name is fired      */
    char target[MAPGEN_MOVER_NAME];   /* ... and it fires this one          */
} mapgen_mover_relay_t;

/*
 * A volume that throws whoever walks into it.
 *
 * Not a mover - nothing about it moves - but a TRANSITION, and the
 * reachability search has nowhere else to learn about one. Quake II gives the
 * player `movedir * speed * 10` and lets him fly.
 */
typedef struct {
    float    mins[3];
    float    maxs[3];
    float    velocity[3];      /* what the game sets, in units per second   */
} mapgen_mover_push_t;

typedef struct {
    mapgen_mover_t          movers[MAPGEN_MOVER_MAX];
    uint32_t                num_movers;
    mapgen_mover_operator_t operators[MAPGEN_MOVER_OPS];
    uint32_t                num_operators;
    mapgen_mover_portal_t   portals[MAPGEN_MOVER_PORTALS];
    uint32_t                num_portals;
    mapgen_mover_relay_t    relays[MAPGEN_MOVER_RELAYS];
    uint32_t                num_relays;
    mapgen_mover_push_t     pushes[MAPGEN_MOVER_PORTALS];
    uint32_t                num_pushes;
    /* The map had more of something than this can hold. A search that ran
       anyway would be answering about a different map, so callers refuse. */
    bool                    overflowed;
} mapgen_movers_t;

/*
 * Read every mover out of a compiled map. Never fails on content: a map with
 * no movers yields an empty set, and an entity this does not understand is
 * skipped rather than guessed at.
 */
bool MapGenMovers_Read(const mapgen_bsp_t *bsp, mapgen_movers_t *out);

/*
 * Where mover `i` is, given the set of targetnames that have been fired.
 *
 * `opened` is a bitmask over movers, one bit each. A mover that is open sits
 * at its LAST stop; one that is not sits at its first. Movers with more than
 * two stops - a train - are asked about by stop index instead.
 */
const float *MapGenMovers_Displacement(const mapgen_movers_t *m, uint32_t i,
                                       uint64_t opened);

/* True when this operator's volume contains the point, with the reach a
   player has standing next to it. */
bool MapGenMovers_OperatorTouched(const mapgen_mover_operator_t *op,
                                  const float point[3]);

/*
 * Which movers a fired targetname moves, following relays all the way through.
 * Returns a mask over movers.
 */
uint64_t MapGenMovers_Fired(const mapgen_movers_t *m, const char *target);

/*
 * Check that every carrying mover can actually travel between its stops, and
 * mark the ones that cannot.
 *
 * Separate from reading the entities because it needs to trace: a machine whose
 * swept volume runs into the world is blocked, and a search that placed a rider
 * at the far stop anyway would report a route through a lift that cannot rise.
 * `ctx` must be bound to the same map the movers came from.
 */
void MapGenMovers_CheckSweeps(mapgen_movers_t *m, mapgen_trace_context_t *ctx);

#endif /* MAPGEN_MOVERS_H */
