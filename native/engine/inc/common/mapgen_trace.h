/*
Copyright (C) 2026 Q2PRO-X

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

/*
==============================================================================

MAPGEN-1 - MapGenTraceContext, reentrant player-hull tracing

Contract section 18.3, and the Codex activation review of 2026-08-31.

--- Why this exists ---------------------------------------------------------

`CM_BoxTrace` cannot be called from parallel Training. It keeps its whole
traversal in file-scope scratch - `trace_start`, `trace_end`, `trace_extents`,
`trace_trace`, `trace_contents`, `trace_ispoint` at `src/common/cmodel.c:448-455`
- and it stamps `mbrush_t::checkcount` against a global counter at `:38`. Two
threads tracing at once corrupt each other silently.

So this carries EVERY piece of that state in a caller-owned context. Nothing is
static, nothing is global, and two contexts never touch the same byte.

--- Scope, deliberately narrow --------------------------------------------

Box and player-hull tracing only.

A reentrant read-only POINT trace ALREADY exists in the engine
(`CM_PointTraceStaticWorkspace` and siblings, `inc/common/cmodel.h:74-97`), and
the review was explicit that it must be preserved and reused rather than
duplicated. This is not a general replacement for the engine's collision code
and does not claim to be one: it is the private workspace `MapValidator` uses
to prove reachability, behind the same `Submit / Observe / Cancel` Seam as
everything else. It is not a new Seam, not an Adapter, and appears in no
caller interface.

==============================================================================
*/

#pragma once

#include "common/mapgen_bsp.h"

#include <stdbool.h>
#include <stdint.h>

/* Content bits, the format's own values. */
#define MAPGEN_TRACE_SOLID        0x00000001
#define MAPGEN_TRACE_WINDOW       0x00000002
#define MAPGEN_TRACE_LAVA         0x00000008
#define MAPGEN_TRACE_SLIME        0x00000010
#define MAPGEN_TRACE_WATER        0x00000020
#define MAPGEN_TRACE_PLAYERCLIP   0x00010000
#define MAPGEN_TRACE_MONSTER      0x02000000

/* What a player hull collides with. Matches the engine's MASK_PLAYERSOLID
   (`inc/shared/shared.h`): a reachability answer that used a different mask
   would not be an answer about the game. */
#define MAPGEN_MASK_PLAYERSOLID  (MAPGEN_TRACE_SOLID | MAPGEN_TRACE_PLAYERCLIP | \
                                  MAPGEN_TRACE_WINDOW | MAPGEN_TRACE_MONSTER)

typedef struct {
    /* Per-brush stamps, so one traversal does not test a brush twice. The
       engine's equivalent is a GLOBAL counter written into every brush; this
       one belongs to the context, which is the whole point. */
    uint32_t *stamps;
    uint32_t  capacity;
    uint32_t  generation;
    /* The document this context is bound to. Tracing with a context bound to
       a different document is refused rather than silently indexing into the
       wrong brush array. */
    const mapgen_bsp_t *bsp;
} mapgen_trace_context_t;

typedef struct {
    bool    allsolid;      /* the whole sweep was inside a solid            */
    bool    startsolid;    /* it started inside a solid                     */
    float   fraction;      /* 0..1 along start->end, 1 when nothing was hit */
    float   endpos[3];
    float   plane_normal[3];
    float   plane_dist;
    bool    hit_plane;
    int32_t contents;      /* contents of the brush that stopped it         */
    int32_t surface_flags;
} mapgen_trace_result_t;

/*
 * Bind a context to a document. Allocates the stamp array; safe to call again
 * with the same or another document. Returns false only on allocation failure.
 *
 * The context must be ZEROED before its first bind. It grows the stamp array
 * with `realloc`, and it cannot tell a context it has never seen from one that
 * already owns memory - so an uninitialised one on the stack reallocs whatever
 * was there and the process dies inside the allocator, a long way from here.
 */
bool MapGenTrace_Bind(mapgen_trace_context_t *ctx, const mapgen_bsp_t *bsp);
void MapGenTrace_Release(mapgen_trace_context_t *ctx);

/*
 * Sweep an axis-aligned box from `start` to `end` against the world model.
 *
 * `mins`/`maxs` are the hull relative to the trace point; pass zeros for a
 * point trace. `brushmask` selects which contents stop the sweep.
 *
 * Reentrant: the only mutable state touched is `ctx` and `out`.
 */
void MapGenTrace_Box(mapgen_trace_context_t *ctx,
                     const float start[3], const float end[3],
                     const float mins[3], const float maxs[3],
                     int32_t brushmask,
                     mapgen_trace_result_t *out);

/*
 * Sweep against ONE brush model, which may have been moved.
 *
 * A door, a lift and a train are submodels: their brushes are stored where the
 * mapper drew them, and the game moves them by translating the whole model. So
 * a sweep against one is the same sweep against its own tree, taken in the
 * model's frame - `displacement` is where the model is now, relative to where
 * it was drawn.
 *
 * `out` is REFINED, not replaced: a caller sweeps the world and then every
 * mover through the same result, and what comes back is the nearest thing hit.
 * Pass a result already initialised by `MapGenTrace_Box`, or one zeroed with
 * `fraction` set to 1.
 */
void MapGenTrace_BoxModel(mapgen_trace_context_t *ctx, uint32_t model,
                          const float displacement[3],
                          const float start[3], const float end[3],
                          const float mins[3], const float maxs[3],
                          int32_t brushmask,
                          mapgen_trace_result_t *out);

/* Contents at a point, using the same masking rules as the sweep. */
int32_t MapGenTrace_PointContents(mapgen_trace_context_t *ctx, const float point[3]);

/* Contents at a point inside one moved brush model, 0 if the point is not in
   it. The liquid a `func_water` carries is only findable this way. */
int32_t MapGenTrace_ModelContents(mapgen_trace_context_t *ctx, uint32_t model,
                                  const float displacement[3],
                                  const float point[3]);
