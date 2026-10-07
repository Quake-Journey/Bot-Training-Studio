/*
 * MapGenPmove - see inc/common/mapgen_pmove.h.
 *
 * There is almost no movement code here, and that is the point: the friction,
 * the step height, the duck, the jump, the wall slide and the water are all
 * the engine's, reached through `PmoveOld`. What this file does is hold a
 * compiled map still while the engine asks it questions.
 */

#include "shared/shared.h"
#include "common/pmove.h"

#include "common/mapgen_pmove.h"
#include "common/mapgen_trace.h"

#include <math.h>
#include <string.h>

const char *MapGenPmove_ResultName(mapgen_pmove_result_t r)
{
    switch (r) {
    case MAPGEN_PMOVE_OK:                return "OK";
    case MAPGEN_PMOVE_ERR_ARGS:          return "ERR_ARGS";
    case MAPGEN_PMOVE_ERR_MEMORY:        return "ERR_MEMORY";
    case MAPGEN_PMOVE_ERR_ALREADY_BOUND: return "ERR_ALREADY_BOUND";
    case MAPGEN_PMOVE_ERR_NOT_BOUND:     return "ERR_NOT_BOUND";
    }
    return "ERR_UNKNOWN";
}

/*
 * The bound document, at file scope because `pmove_old_t`'s callbacks take no
 * context. Everything else in MAPGEN keeps its state in a caller-owned struct
 * precisely so two threads can work at once; this one cannot, and says so.
 */
/*
 * Per thread, so that more than one walk can happen at once.
 *
 * A bound world is borrowed pointers into a BSP that is read and never
 * written, so one per thread is one set of pointers to the same bytes. The
 * override stop is the reason it MUST be private: `burst` sets it for the
 * length of one expansion, and two walkers sharing it would each be simulating
 * the other's lift.
 */
static _Thread_local struct {
    const mapgen_bsp_t     *bsp;
    mapgen_trace_context_t  trace;
    pmoveParams_t           params;
    bool                    bound;

    /* The map's moving parts, and where they are. Borrowed, never owned. */
    const mapgen_movers_t  *movers;
    uint64_t                opened;
    int32_t                 override_mover;
    uint32_t                override_stop;
} g_world;

/* Where mover `i` is right now, honouring a rider's override. */
static const float *mover_at(uint32_t i)
{
    static const float none[3] = { 0, 0, 0 };
    if (!g_world.movers || i >= g_world.movers->num_movers)
        return none;
    if (g_world.override_mover >= 0 && (uint32_t)g_world.override_mover == i) {
        const mapgen_mover_t *mv = &g_world.movers->movers[i];
        const uint32_t stop = g_world.override_stop < mv->num_stops
                            ? g_world.override_stop : 0;
        return mv->stop[stop];
    }
    return MapGenMovers_Displacement(g_world.movers, i, g_world.opened);
}

/* Handed back inside a trace result, so it belongs to whoever asked. */
static _Thread_local csurface_t g_null_surface;

static trace_t q_gameabi world_trace(const vec3_t start, const vec3_t mins,
                                     const vec3_t maxs, const vec3_t end)
{
    trace_t out;
    memset(&out, 0, sizeof(out));
    out.fraction = 1.0f;
    VectorCopy(end, out.endpos);
    out.surface = &g_null_surface;
    if (!g_world.bound)
        return out;

    /*
     * start, END, mins, maxs - in that order.
     *
     * All four are `const float[3]`, so handing them over in the order the
     * call site happens to name them compiles silently and sweeps the player
     * from where he stands towards his own hull mins, which is a point near
     * the world origin. The symptom was a player whose position ignored his
     * velocity and his yaw entirely and shrank towards (0,0,0) by about three
     * per cent a frame, sinking through the map until the engine reported him
     * underwater. Nothing about it looked like an argument order.
     */
    mapgen_trace_result_t hit;
    MapGenTrace_Box(&g_world.trace, start, end, mins, maxs,
                    MAPGEN_MASK_PLAYERSOLID, &hit);

    /*
     * And then every mover, through the same result, so the nearest thing hit
     * wins. A door in the way stops the sweep exactly as a wall does; the only
     * difference between them is that the door might be somewhere else in a
     * moment, and that is the caller's business, not the trace's.
     */
    if (g_world.movers) {
        for (uint32_t i = 0; i < g_world.movers->num_movers; i++) {
            MapGenTrace_BoxModel(&g_world.trace, g_world.movers->movers[i].model,
                                 mover_at(i), start, end, mins, maxs,
                                 MAPGEN_MASK_PLAYERSOLID, &hit);
        }
    }

    out.allsolid = hit.allsolid;
    out.startsolid = hit.startsolid;
    out.fraction = hit.fraction;
    VectorCopy(hit.endpos, out.endpos);
    out.contents = hit.contents;
    if (hit.hit_plane) {
        VectorCopy(hit.plane_normal, out.plane.normal);
        out.plane.dist = hit.plane_dist;
    }
    /*
     * A surface record is always handed back, never NULL. The engine reads
     * `trace.surface->flags` without checking, and a NULL here is a crash in
     * somebody else's file half a second later.
     */
    g_null_surface.flags = hit.surface_flags;
    out.surface = &g_null_surface;

    /*
     * And an entity for whatever stopped it.
     *
     * The engine decides a player is standing on something by asking whether
     * the downward trace came back with an `ent`, not by looking at the
     * fraction. Handing back NULL - which is what "there are no entities in
     * this simulation" naively means - leaves every player permanently
     * airborne: they accumulate gravity, never take ground friction, and slide
     * down a floor they are resting on at a quarter of a unit a frame. The
     * world itself is that entity here, and one sentinel says so.
     */
    if (hit.fraction < 1.0f || hit.startsolid || hit.allsolid)
        out.ent = (struct edict_s *)&g_world;
    return out;
}

static int world_contents(const vec3_t point)
{
    if (!g_world.bound)
        return 0;
    int contents = MapGenTrace_PointContents(&g_world.trace, point);
    /* A func_water is a brush model like any other, and the water it carries
       is only findable inside its own subtree. A player swimming in a pool
       that rises is standing in nothing at all without this. */
    if (g_world.movers) {
        for (uint32_t i = 0; i < g_world.movers->num_movers; i++) {
            contents |= MapGenTrace_ModelContents(&g_world.trace,
                                                  g_world.movers->movers[i].model,
                                                  mover_at(i), point);
        }
    }
    return contents;
}

mapgen_pmove_result_t MapGenPmove_Bind(const mapgen_bsp_t *bsp)
{
    if (!bsp)
        return MAPGEN_PMOVE_ERR_ARGS;
    if (g_world.bound)
        return MAPGEN_PMOVE_ERR_ALREADY_BOUND;

    memset(&g_world, 0, sizeof(g_world));
    if (!MapGenTrace_Bind(&g_world.trace, bsp))
        return MAPGEN_PMOVE_ERR_MEMORY;

    /* The stock profile, unmodified. A validator that moved at a speed no
       player moves at would be measuring a different game. */
    PmoveInit(&g_world.params);
    g_world.bsp = bsp;
    g_world.override_mover = -1;
    g_world.bound = true;
    return MAPGEN_PMOVE_OK;
}

mapgen_pmove_result_t MapGenPmove_BindWorld(const mapgen_bsp_t *bsp,
                                            const mapgen_movers_t *movers)
{
    const mapgen_pmove_result_t rc = MapGenPmove_Bind(bsp);
    if (rc != MAPGEN_PMOVE_OK)
        return rc;
    g_world.movers = movers;
    return MAPGEN_PMOVE_OK;
}

void MapGenPmove_SetOpened(uint64_t opened)
{
    g_world.opened = opened;
}

uint64_t MapGenPmove_Opened(void)
{
    return g_world.opened;
}

void MapGenPmove_OverrideStop(int32_t mover, uint32_t stop)
{
    g_world.override_mover = mover;
    g_world.override_stop = stop;
}

bool MapGenPmove_Bound(void)
{
    return g_world.bound;
}

void MapGenPmove_Release(void)
{
    if (!g_world.bound)
        return;
    MapGenTrace_Release(&g_world.trace);
    memset(&g_world, 0, sizeof(g_world));
    g_world.override_mover = -1;
}

void MapGenPmove_Spawn(mapgen_pmove_player_t *player, const float origin[3])
{
    if (!player)
        return;
    memset(player, 0, sizeof(*player));
    if (origin)
        VectorCopy(origin, player->origin);
    player->gravity = 800;
    player->pm_type = PM_NORMAL;
    player->freshly_placed = true;
}

void MapGenPmove_Launch(mapgen_pmove_player_t *player, const float origin[3],
                        const float velocity[3])
{
    if (!player)
        return;
    MapGenPmove_Spawn(player, origin);
    if (velocity)
        VectorCopy(velocity, player->velocity);
    /*
     * Off the ground from the first frame. Being thrown while the engine still
     * believes there is ground underfoot loses most of the launch to friction
     * on the frame it matters.
     */
    player->on_ground = false;
}

/* The engine's state travels in fixed point; this is the only place that
   conversion lives. */
static void to_pmove(const mapgen_pmove_player_t *player, pmove_old_t *pm)
{
    memset(pm, 0, sizeof(*pm));
    for (int a = 0; a < 3; a++) {
        pm->s.origin[a] = (short)lrintf(player->origin[a] * 8.0f);
        pm->s.velocity[a] = (short)lrintf(player->velocity[a] * 8.0f);
    }
    pm->s.pm_type = (pmtype_t)player->pm_type;
    pm->s.pm_flags = (byte)player->pm_flags;
    pm->s.pm_time = (byte)player->pm_time;
    pm->s.gravity = player->gravity;
    pm->trace = world_trace;
    pm->pointcontents = world_contents;
    pm->snapinitial = player->freshly_placed;
}

static void from_pmove(const pmove_old_t *pm, mapgen_pmove_player_t *player)
{
    for (int a = 0; a < 3; a++) {
        player->origin[a] = pm->s.origin[a] * 0.125f;
        player->velocity[a] = pm->s.velocity[a] * 0.125f;
    }
    player->pm_flags = pm->s.pm_flags;
    player->pm_time = pm->s.pm_time;
    player->gravity = pm->s.gravity;
    player->pm_type = (uint8_t)pm->s.pm_type;
    player->on_ground = pm->groundentity != NULL;
    player->water_level = pm->waterlevel;
}

bool MapGenPmove_Step(mapgen_pmove_player_t *player,
                      const mapgen_pmove_command_t *cmd)
{
    if (!g_world.bound || !player || !cmd)
        return false;

    pmove_old_t pm;
    to_pmove(player, &pm);
    player->freshly_placed = false;

    pm.cmd.msec = cmd->msec ? cmd->msec : 16;
    pm.cmd.forwardmove = cmd->forward;
    pm.cmd.sidemove = cmd->side;
    pm.cmd.upmove = cmd->up;
    pm.cmd.angles[YAW] = ANGLE2SHORT(cmd->yaw);
    pm.cmd.angles[PITCH] = ANGLE2SHORT(cmd->pitch);
    pm.cmd.angles[ROLL] = 0;
    /*
     * The engine reads a jump out of `upmove`, not out of a button, and it
     * only jumps when the previous frame let go - PMF_JUMP_HELD is what stops
     * a held key from bouncing. Both belong to the caller's intent, so both
     * are stated here rather than left to whatever was in the flags.
     */
    if (cmd->jump && pm.cmd.upmove < 10)
        pm.cmd.upmove = 400;

    /* The world is not moving under the player: there are no entities in this
       simulation, so no ground entity is ever handed in. Movers are the next
       gate's business and are modelled as explicit state, not as physics. */
    PmoveOld(&pm, &g_world.params);
    from_pmove(&pm, player);

    for (int a = 0; a < 3; a++)
        player->view_angles[a] = pm.viewangles[a];
    return true;
}

uint32_t MapGenPmove_Run(mapgen_pmove_player_t *player,
                         const mapgen_pmove_command_t *cmd, uint32_t frames)
{
    if (!player || !cmd)
        return 0;

    uint32_t ran = 0;
    for (uint32_t i = 0; i < frames; i++) {
        float before[3];
        VectorCopy(player->origin, before);
        if (!MapGenPmove_Step(player, cmd))
            break;
        ran++;

        /*
         * Stop when the player has stopped. A frame that moves less than an
         * eighth of a unit - the resolution the state is stored at - has moved
         * nothing, and running another two hundred of them against the same
         * wall is time spent proving the wall again.
         */
        float moved = 0.0f;
        for (int a = 0; a < 3; a++) {
            const float d = player->origin[a] - before[a];
            moved += d * d;
        }
        /*
         * And not before the player has had a chance to start.
         *
         * Acceleration from a standstill moves less than an eighth of a unit
         * in the first frame, so an early-out that looked at frame zero fired
         * immediately and every walk was one frame long. The exploration built
         * on it found thirty-six places to stand in a map with a thousand.
         */
        if (i >= 4 && moved < 0.125f * 0.125f && player->on_ground)
            break;
    }
    return ran;
}

bool MapGenPmove_SweepBody(const float start[3], const float end[3],
                           float endpos[3])
{
    if (!g_world.bound || !start || !end || !endpos)
        return false;
    /* The standing hull, which is what PM_CheckDuck gives an upright player. */
    static const vec3_t mins = { -16, -16, -24 };
    static const vec3_t maxs = { 16, 16, 32 };
    const trace_t hit = world_trace(start, mins, maxs, end);
    VectorCopy(hit.endpos, endpos);
    return !hit.startsolid && !hit.allsolid;
}

bool MapGenPmove_DropToFloor(mapgen_pmove_player_t *player)
{
    if (!g_world.bound || !player)
        return false;

    /*
     * Standing still, under gravity, until the engine says the player has
     * settled - or until long enough that he plainly has not.
     *
     * Settled is ground OR water, not ground alone. A swimmer never touches
     * ground, so a search that only accepted ground abandoned every intent
     * from every state in a pool: the whole pool came out as places a player
     * could reach and never leave, and the map was reported broken because
     * it had water in it.
     */
    mapgen_pmove_command_t still;
    memset(&still, 0, sizeof(still));
    still.msec = 16;

    for (uint32_t i = 0; i < 128; i++) {
        if (!MapGenPmove_Step(player, &still))
            return false;
        if (player->on_ground || player->water_level > 1)
            return true;
    }
    return false;
}
