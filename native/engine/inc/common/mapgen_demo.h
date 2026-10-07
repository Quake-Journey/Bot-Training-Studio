/*
 * MapGenDemo - what people actually did on the map.
 *
 * Contract 6.4. A demo is not a donor and nothing here produces geometry: it
 * answers the one question static analysis cannot, which is which surfaces the
 * movement of a map is built on. Two operators were withdrawn for getting that
 * wrong - a ramp laid over a flight of steps, a wall thickened in the middle of
 * a room - and neither would have been proposed against a map whose hot
 * surfaces were known.
 *
 * The parsing is NOT here. It goes through the engine's existing reentrant
 * offline decoder, which already owns the protocol and the delta rules; a
 * second Quake II demo parser is exactly the thing this must not become. What
 * is here is the consumer: the decoder's neutral events in, a bounded canonical
 * derived trace out.
 *
 * --- what a demo can and cannot say ---------------------------------------
 *
 * It carries about ten server playerstates a second. Not usercmds, not intent,
 * not another player's complete movement. So it yields observed trajectories,
 * occupancy, contact, jumps, drops, landings, warps and rides - and it
 * proves neither exact input nor universal reachability. Everything derived
 * here is an OBSERVATION with a count, never a permission and never a veto:
 *
 *   a hot surface is a cost and a reason to revalidate, not a refusal;
 *   a cold surface is never evidence that an edit is safe.
 *
 * --- provenance -----------------------------------------------------------
 *
 * Every accepted stream is bound to the bytes it came from. Truncation, a map
 * that is not the one being asked about, an undecodable protocol and ambiguous
 * provenance are REJECTED with a reason and never silently skipped, because a
 * corpus that quietly drops what it cannot read reports agreement it has not
 * earned.
 */

#ifndef MAPGEN_DEMO_H
#define MAPGEN_DEMO_H

#include <stdbool.h>
#include <stdint.h>

#define MAPGEN_DEMO_NAME        128
#define MAPGEN_DEMO_MAX_SAMPLES 65536u
#define MAPGEN_DEMO_MAX_CELLS   65536u
#define MAPGEN_DEMO_MAX_MOVES   262144u
#define MAPGEN_DEMO_MAX_EVENTS  16384u

/* The lattice observations are quantised onto. The same size the reachability
   search calls one place, so the two can be compared without either of them
   deciding what the other meant by "here". */
#define MAPGEN_DEMO_CELL        32.0f

typedef enum {
    MAPGEN_DEMO_OK = 0,
    MAPGEN_DEMO_ERR_ARGS,
    MAPGEN_DEMO_ERR_MEMORY,
    MAPGEN_DEMO_ERR_OPEN,
    MAPGEN_DEMO_ERR_FORMAT,       /* the decoder could not read it           */
    MAPGEN_DEMO_ERR_TRUNCATED,
    MAPGEN_DEMO_ERR_UNSUPPORTED,  /* a protocol variant it cannot decode     */
    MAPGEN_DEMO_ERR_MAP_MISMATCH, /* recorded on a different map             */
    MAPGEN_DEMO_ERR_NO_POV,       /* nothing in it is the recorder's own     */
    MAPGEN_DEMO_ERR_AMBIGUOUS,    /* more than one map segment in one file   */
    /* a provenance field does not fit its schema; a trimmed identity is a
       different identity, and recording one would be a lie about the evidence */
    MAPGEN_DEMO_ERR_PROVENANCE_TOO_LONG,
    MAPGEN_DEMO_ERR_EMPTY
} mapgen_demo_result_t;

const char *MapGenDemo_ResultName(mapgen_demo_result_t r);

typedef enum {
    MAPGEN_DEMO_JUMP = 0,
    MAPGEN_DEMO_LAND,
    MAPGEN_DEMO_DROP,
    MAPGEN_DEMO_WARP,      /* the trajectory does not continue here: a
                              teleporter, or a respawn, and a demo cannot
                              tell them apart                              */
    MAPGEN_DEMO_RIDE,
    MAPGEN_DEMO_EVENT_KINDS
} mapgen_demo_event_kind_t;

const char *MapGenDemo_EventName(mapgen_demo_event_kind_t k);

typedef struct {
    int32_t  cell[3];
    uint32_t frames;      /* how long a player stood or moved through here  */
    uint32_t contacts;    /* ... of those, with his feet on something       */
} mapgen_demo_cell_t;

/* A directed pair of cells a player was observed to pass between in one step
   of the recording. This is the movement obligation: somebody did this. */
typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t seen;
} mapgen_demo_move_t;

typedef struct {
    mapgen_demo_event_kind_t kind;
    int32_t  time_ms;
    float    origin[3];
    float    magnitude;   /* jump/drop height, ride distance, in units      */
} mapgen_demo_event_t;

typedef struct {
    char     map[MAPGEN_DEMO_NAME];      /* as the stream named it          */
    char     gamedir[MAPGEN_DEMO_NAME];
    char     source[MAPGEN_DEMO_NAME];   /* archive member or file name     */
    char     digest[65];                 /* sha256 of the demo bytes        */
    int32_t  protocol;
    int32_t  pov_slot;
    uint32_t quality;                    /* the decoder's DOF_Q_* flags     */
    int32_t  duration_ms;
    uint32_t samples;                    /* playerstates the POV contributed */
} mapgen_demo_provenance_t;

typedef struct mapgen_demo_s mapgen_demo_t;

/*
 * Read one demo file and derive its bounded trace.
 *
 * `expect_map` is the map the caller is asking about - a stream recorded on
 * another one is rejected rather than mixed in. Pass NULL to accept whatever
 * the stream says it is, which is only for surveying a corpus.
 *
 * The file is opened read-only and never written to. `source` is recorded as
 * the provenance name; pass the archive member's name when the bytes were
 * unpacked from one.
 */
mapgen_demo_result_t MapGenDemo_Read(const char *path, const char *expect_map,
                                     const char *source, mapgen_demo_t **out);

void MapGenDemo_Free(mapgen_demo_t *demo);

const mapgen_demo_provenance_t *MapGenDemo_Provenance(const mapgen_demo_t *d);

uint32_t MapGenDemo_NumCells(const mapgen_demo_t *d);
const mapgen_demo_cell_t *MapGenDemo_Cell(const mapgen_demo_t *d, uint32_t i);
uint32_t MapGenDemo_NumMoves(const mapgen_demo_t *d);
const mapgen_demo_move_t *MapGenDemo_Move(const mapgen_demo_t *d, uint32_t i);
uint32_t MapGenDemo_NumEvents(const mapgen_demo_t *d);
const mapgen_demo_event_t *MapGenDemo_Event(const mapgen_demo_t *d, uint32_t i);
uint32_t MapGenDemo_EventCount(const mapgen_demo_t *d,
                               mapgen_demo_event_kind_t kind);

/*
 * The canonical text of the derived trace, and its digest.
 *
 * Locale-free, ordered, and carrying the provenance - so two runs over the
 * same bytes produce the same string, and a Snapshot stores this rather than
 * the demo. Raw demo bytes are never copied anywhere by this module and the
 * corpus is never modified.
 */
uint32_t MapGenDemo_CanonicalText(const mapgen_demo_t *d, char *out,
                                  uint32_t size);
uint64_t MapGenDemo_Digest(const mapgen_demo_t *d);

#endif /* MAPGEN_DEMO_H */
