/*
 * MapGenPmove - the engine's own player movement, run over a compiled map.
 *
 * Every reachability answer this project has given so far came from a graph it
 * built itself: stances on a 32-unit grid joined by edges it decided were walks
 * or jumps from the numbers. That graph called a ledge a route because a
 * `rise` field said eighteen, and a player who dropped into the pocket below it
 * could not get out. Contract 18.3 was rewritten around that screenshot, and it
 * says the authority is the shared pmove over the compiled BSP - not a model of
 * it, the same code the client and the server run.
 *
 * So this binds `PmoveOld` to a `mapgen_bsp_t`: its trace and pointcontents
 * callbacks go to the reentrant box trace, and everything else - friction,
 * step height, water, the duck, the jump, the wall-slide - is the engine's,
 * unchanged and unmodelled.
 *
 * One caveat is in the ABI and cannot be designed away: `pmove_old_t`'s
 * callbacks take no context, so the bound document lives at file scope and one
 * of these may be bound at a time. It is a Bind/Release pair for that reason,
 * and it is checked rather than assumed.
 */

#ifndef MAPGEN_PMOVE_H
#define MAPGEN_PMOVE_H

#include <stdbool.h>
#include <stdint.h>

#include "common/mapgen_bsp.h"
#include "common/mapgen_movers.h"

typedef enum {
    MAPGEN_PMOVE_OK = 0,
    MAPGEN_PMOVE_ERR_ARGS,
    MAPGEN_PMOVE_ERR_MEMORY,
    MAPGEN_PMOVE_ERR_ALREADY_BOUND,
    MAPGEN_PMOVE_ERR_NOT_BOUND
} mapgen_pmove_result_t;

const char *MapGenPmove_ResultName(mapgen_pmove_result_t r);

/* What a simulated player is, between frames. Deliberately a small, copyable
   record: an exploration keeps thousands of them. */
typedef struct {
    float    origin[3];
    float    velocity[3];
    float    view_angles[3];
    uint16_t pm_flags;
    uint16_t pm_time;
    int16_t  gravity;
    uint8_t  pm_type;
    bool     on_ground;
    int32_t  water_level;
    /* Set once by Spawn, cleared by the first step. The engine treats
       `snapinitial` as "this state was changed from outside" and re-snaps the
       position when it sees it; asserting it every frame made the engine
       discard the velocity it had just accumulated, and a player fell eighty
       units in two seconds instead of sixteen hundred. */
    bool     freshly_placed;
} mapgen_pmove_player_t;

/* What a frame of input is. Angles are the direction the player faces; the
   moves are the engine's own -400..400 scale. */
typedef struct {
    uint8_t msec;
    float   yaw;
    float   pitch;
    int16_t forward;
    int16_t side;
    int16_t up;
    bool    jump;
} mapgen_pmove_command_t;

/*
 * Bind one compiled map. Refused if another is already bound, because the
 * engine's callback signature leaves nowhere to put a second one and a silent
 * rebind would make one exploration's answers come from another one's map.
 */
mapgen_pmove_result_t MapGenPmove_Bind(const mapgen_bsp_t *bsp);
void                  MapGenPmove_Release(void);
/* Does THIS thread have a world bound? A walk shared across threads has to
   know whether it is borrowing one or already holds it. */
bool                  MapGenPmove_Bound(void);

/*
 * Bind the map's movers as well, so the world a player walks in has its doors
 * and lifts in it.
 *
 * `movers` is borrowed and must outlive the binding. Without this a player
 * walks through every closed door in the map and falls through every lift,
 * and the answers are about a map nobody ever plays.
 */
mapgen_pmove_result_t MapGenPmove_BindWorld(const mapgen_bsp_t *bsp,
                                            const mapgen_movers_t *movers);

/*
 * Which movers are at their far stop, one bit each.
 *
 * A mover at a stop is the whole of the world's state here: there is no
 * travel, no timing and no queue. That is a deliberate limit - the question
 * being asked is whether a route EXISTS, and a route that exists only during
 * three seconds of a lift's rise is not one this can speak about.
 */
void MapGenPmove_SetOpened(uint64_t opened);
uint64_t MapGenPmove_Opened(void);

/*
 * Put one mover at one of its stops regardless of `opened`, which is how a
 * rider is simulated: the lift he is standing on is where he is riding it to.
 * Pass a negative index to clear the override.
 */
void MapGenPmove_OverrideStop(int32_t mover, uint32_t stop);

/* A player standing at a point, as the game would spawn one. */
void MapGenPmove_Spawn(mapgen_pmove_player_t *player, const float origin[3]);

/*
 * Throw a player, the way a `trigger_push` does.
 *
 * The game sets his velocity outright and lets the ordinary movement carry him;
 * so does this. It is a separate entry point rather than a field on the spawn
 * because being launched is a thing that HAPPENS to a player, and a caller that
 * had to remember to clear a velocity would eventually not.
 */
void MapGenPmove_Launch(mapgen_pmove_player_t *player, const float origin[3],
                        const float velocity[3]);

/*
 * One frame of movement, by the engine's rules.
 *
 * `player` is updated in place. Returns false only when nothing is bound - a
 * player who cannot move because a wall is in the way has moved successfully
 * by zero units, which is a fact about the map and not an error.
 */
bool MapGenPmove_Step(mapgen_pmove_player_t *player,
                      const mapgen_pmove_command_t *cmd);

/*
 * Run `frames` of the same command, stopping early if the player stops moving.
 * Returns how many frames actually ran, which is how a caller tells "walked
 * there" from "walked into a wall".
 */
uint32_t MapGenPmove_Run(mapgen_pmove_player_t *player,
                         const mapgen_pmove_command_t *cmd, uint32_t frames);

/* Where a player standing here would come to rest, if anywhere. False when the
   point is inside a solid or has no floor within a fall the engine survives. */
bool MapGenPmove_DropToFloor(mapgen_pmove_player_t *player);

/*
 * Slide a standing player from one point towards another through the bound
 * world - movers included - and say where he ended up.
 *
 * Not a movement: no gravity, no friction, no input. It answers one geometric
 * question the search needs and the engine's own trace already answers, which
 * is how far a body gets before something stops it.
 */
bool MapGenPmove_SweepBody(const float start[3], const float end[3],
                           float endpos[3]);

#endif /* MAPGEN_PMOVE_H */
