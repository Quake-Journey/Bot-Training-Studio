/*
 * MapGenRooms - where the rooms are, and what joins them.
 *
 * Every operator so far has worked on one brush side at a time, and that is
 * why the forks came out the way they did. A side is not a piece of
 * architecture: pushing one out by twenty units makes the boulder the PO
 * called "torchashchie valuny", and pushing four hundred of them out by twenty
 * units makes four hundred boulders and a map that is still q2dm1. The
 * divergence oracle put a number on it - fidelity 0 changed five per cent of
 * the map when it was asked for a hundred - and the number will not move until
 * something knows that a room is a room.
 *
 * --- how a room is found ---------------------------------------------------
 *
 * From the empty space, not the solid. Solid in a Quake II map is one
 * connected mass with the level carved out of it; the ROOMS are the holes, and
 * holes are what a player experiences.
 *
 * So: rasterise the map, measure at every empty cell how far it is from the
 * nearest solid - its clearance - and let the local maxima of that be the
 * middles of rooms. A room is wide and its clearance is high; a corridor is
 * narrow and its clearance is low; a doorway is the narrowest thing between
 * two wide ones. Flooding outward from each maximum in decreasing clearance
 * assigns every cell to the room it belongs to, and where two floods meet is a
 * CONNECTOR whose width is what the clearance says it is.
 *
 * That segmentation is the same one a level designer would draw, and it is
 * derived from the compiled map rather than from the recipe that made it, so
 * it works on a donor nobody has annotated.
 *
 * --- what a bundle is ------------------------------------------------------
 *
 * A room plus its dependency closure: the walls that bound it, the movers
 * whose models open into it, and the items and player starts inside it. An
 * operator that moves or reshapes a room has to take all of that with it, and
 * this is what says what "all of that" is.
 */

#ifndef MAPGEN_ROOMS_H
#define MAPGEN_ROOMS_H

#include <stdbool.h>
#include <stdint.h>

#include "common/mapgen_bsp.h"

/* The lattice, in units. The same thirty-two the reachability search, the demo
   corpus and the divergence oracle use, so all four can be talked about in the
   same sentence. */
#define MAPGEN_ROOMS_CELL       32.0f
#define MAPGEN_ROOMS_MAX_CELLS  8388608u
#define MAPGEN_ROOMS_MAX        256u
#define MAPGEN_ROOMS_MAX_LINKS  1024u
#define MAPGEN_ROOMS_MAX_ITEMS  64u

typedef enum {
    MAPGEN_ROOMS_OK = 0,
    MAPGEN_ROOMS_ERR_ARGS,
    MAPGEN_ROOMS_ERR_MEMORY,
    MAPGEN_ROOMS_ERR_TOO_LARGE,
    MAPGEN_ROOMS_ERR_NO_SPACE
} mapgen_rooms_result_t;

const char *MapGenRooms_ResultName(mapgen_rooms_result_t r);

typedef struct {
    float    mins[3];
    float    maxs[3];
    float    centre[3];
    uint32_t cells;          /* how big it is, in lattice cells             */
    float    clearance;      /* the widest point in it, in units            */
    uint32_t links;          /* how many other rooms it opens onto          */
    bool     reachable;      /* a player can get here                       */
    uint32_t items;          /* pickups inside it                           */
    uint32_t spawns;         /* player starts inside it                     */
} mapgen_room_t;

/*
 * A way between two rooms.
 *
 * `width` is twice the clearance at the narrowest point of the passage, which
 * is the diameter of the largest sphere that fits through it - so a doorway a
 * player fits through is at least sixty-four and one he has to duck for is
 * less.
 */
typedef struct {
    uint32_t a;
    uint32_t b;
    float    at[3];          /* the narrowest point                         */
    float    width;
    float    normal[3];      /* which way through it points, a to b         */
} mapgen_room_link_t;

typedef struct mapgen_rooms_s mapgen_rooms_t;

/*
 * Segment a compiled map.
 *
 * `min_clearance` is how wide a place has to be before it can be the middle of
 * a room rather than part of a passage; 64 units - a player's own width twice
 * over - is the sane default and is what a caller should pass unless it is
 * measuring something specific.
 */
mapgen_rooms_result_t MapGenRooms_Find(const mapgen_bsp_t *bsp,
                                       float min_clearance,
                                       float persistence,
                                       mapgen_rooms_t **out);

/*
 * How much wider a room has to be in the middle than at its entrance, in
 * units, before it is a room at all.
 *
 * A watershed splits at every local maximum of clearance, and a hall with a
 * pillar in it has two of them. The test for whether a split is real is how
 * far the basin rises above the saddle it shares with its neighbour: a bump
 * beside a pillar rises a few units and is part of the hall, and a chamber off
 * a corridor rises by most of its own width.
 *
 * A ratio was tried first and is not the knob. Merging is transitive, so one
 * wide saddle chains a dozen basins together however the ratio is set: q2dm1
 * gave two hundred and thirty-four rooms at one extreme and two covering the
 * whole map at the other, with almost nothing in between. Depth below the
 * saddle is a property of the basin itself and does not chain.
 */
#define MAPGEN_ROOMS_PERSISTENCE 32.0f

void MapGenRooms_Free(mapgen_rooms_t *rooms);

uint32_t MapGenRooms_Count(const mapgen_rooms_t *r);
const mapgen_room_t *MapGenRooms_Room(const mapgen_rooms_t *r, uint32_t i);
uint32_t MapGenRooms_NumLinks(const mapgen_rooms_t *r);
const mapgen_room_link_t *MapGenRooms_Link(const mapgen_rooms_t *r, uint32_t i);

/* Which room a point is in, or UINT32_MAX for solid and for the void. */
uint32_t MapGenRooms_At(const mapgen_rooms_t *r, const float point[3]);

/*
 * How far a wall of this room could be pushed outward along `axis` in the
 * direction `sign`, in units, before it would break through into somewhere
 * else.
 *
 * This is the question a reshape operator has to ask before it moves
 * anything: solid that has open space behind it is holding the map together
 * somewhere, and solid that has more solid behind it for a long way is just
 * rock. Returns 0 when the wall cannot move at all.
 */
float MapGenRooms_PushRoom(const mapgen_rooms_t *r, uint32_t room,
                           int axis, int sign);

/* How many open cells sit at each clearance, in cells. A segmentation that
   looks wrong is usually a distance transform that is wrong, and this is the
   cheapest way to see which. Returns the number of open cells. */
uint32_t MapGenRooms_ClearanceHistogram(const mapgen_rooms_t *r,
                                        uint32_t *bins, uint32_t num_bins);

/* The canonical text of the segmentation and its digest, so two runs over the
   same map produce the same rooms and a Snapshot can carry them. */
uint32_t MapGenRooms_CanonicalText(const mapgen_rooms_t *r, char *out,
                                   uint32_t size);
uint64_t MapGenRooms_Digest(const mapgen_rooms_t *r);

#endif /* MAPGEN_ROOMS_H */
