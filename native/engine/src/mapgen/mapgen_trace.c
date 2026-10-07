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
 * MAPGEN-1 - reentrant box/hull tracing.
 *
 * The algorithm is Quake II's, deliberately: MapValidator must agree with what
 * the engine actually does, so this follows `CM_BoxTrace`'s structure - plane
 * offsets for the box, DIST_EPSILON, the fraction clamp - rather than
 * inventing a cleaner sweep that would be subtly different.
 *
 * What is NOT Quake II's is the state. Every value the original keeps in
 * file scope lives in the caller's context or on this stack, so two threads
 * can trace at once. Contract section 18.3.
 */

#include "common/mapgen_trace.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Quake II's epsilon, kept because the answers must match the engine's. */
#define DIST_EPSILON  (0.03125f)

typedef struct {
    mapgen_trace_context_t *ctx;
    const mapgen_bsp_t     *bsp;

    float  start[3];
    float  end[3];
    float  mins[3];
    float  maxs[3];
    float  extents[3];
    bool   ispoint;
    int32_t brushmask;

    mapgen_trace_result_t *trace;
} sweep_t;

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* ------------------------------------------------------------------------ */

bool MapGenTrace_Bind(mapgen_trace_context_t *ctx, const mapgen_bsp_t *bsp)
{
    if (!ctx || !bsp)
        return false;
    uint32_t need = MapGenBsp_NumBrushes(bsp);
    if (need > ctx->capacity) {
        uint32_t *grown = realloc(ctx->stamps, (size_t)need * sizeof(uint32_t));
        if (!grown)
            return false;
        memset(grown + ctx->capacity, 0, (size_t)(need - ctx->capacity) * sizeof(uint32_t));
        ctx->stamps = grown;
        ctx->capacity = need;
    }
    ctx->bsp = bsp;
    return true;
}

void MapGenTrace_Release(mapgen_trace_context_t *ctx)
{
    if (!ctx)
        return;
    free(ctx->stamps);
    ctx->stamps = NULL;
    ctx->capacity = 0;
    ctx->generation = 0;
    ctx->bsp = NULL;
}

/* ------------------------------------------------------------------------ */

static void clip_box_to_brush(sweep_t *s, const mapgen_bsp_brush_t *brush)
{
    if (!brush->numsides)
        return;

    float enterfrac = -1.0f;
    float leavefrac = 1.0f;
    const mapgen_bsp_plane_t *clipplane = NULL;
    bool getout = false;
    bool startout = false;
    const mapgen_bsp_brushside_t *leadside = NULL;

    for (int32_t i = 0; i < brush->numsides; i++) {
        const mapgen_bsp_brushside_t *side =
            MapGenBsp_BrushSide(s->bsp, (uint32_t)(brush->firstside + i));
        if (!side)
            return;
        const mapgen_bsp_plane_t *plane = MapGenBsp_Plane(s->bsp, side->planenum);
        if (!plane)
            return;

        float dist;
        if (!s->ispoint) {
            /* Push the plane out to account for the box, exactly as the engine
               does: the corner that leads into the plane decides. */
            float ofs[3];
            for (int j = 0; j < 3; j++)
                ofs[j] = plane->normal[j] < 0 ? s->maxs[j] : s->mins[j];
            dist = plane->dist - dot3(ofs, plane->normal);
        } else {
            dist = plane->dist;
        }

        float d1 = dot3(s->start, plane->normal) - dist;
        float d2 = dot3(s->end, plane->normal) - dist;

        if (d2 > 0)
            getout = true;      /* endpoint is outside this brush */
        if (d1 > 0)
            startout = true;

        /* Completely in front of this face: the sweep misses the brush. */
        if (d1 > 0 && d2 >= d1)
            return;
        /* Completely behind: this face cannot be the one we enter through. */
        if (d1 <= 0 && d2 <= 0)
            continue;

        if (d1 > d2) {
            float f = (d1 - DIST_EPSILON) / (d1 - d2);
            if (f > enterfrac) {
                enterfrac = f;
                clipplane = plane;
                leadside = side;
            }
        } else {
            float f = (d1 + DIST_EPSILON) / (d1 - d2);
            if (f < leavefrac)
                leavefrac = f;
        }
    }

    if (!startout) {
        /* Started inside this brush. */
        s->trace->startsolid = true;
        if (!getout)
            s->trace->allsolid = true;
        s->trace->contents |= brush->contents;
        return;
    }

    if (enterfrac < leavefrac && enterfrac > -1.0f && enterfrac < s->trace->fraction) {
        if (enterfrac < 0)
            enterfrac = 0;
        s->trace->fraction = enterfrac;
        if (clipplane) {
            s->trace->plane_normal[0] = clipplane->normal[0];
            s->trace->plane_normal[1] = clipplane->normal[1];
            s->trace->plane_normal[2] = clipplane->normal[2];
            s->trace->plane_dist = clipplane->dist;
            s->trace->hit_plane = true;
        }
        s->trace->contents = brush->contents;
        if (leadside && leadside->texinfo >= 0) {
            const mapgen_bsp_texinfo_t *ti =
                MapGenBsp_TexInfo(s->bsp, (uint32_t)leadside->texinfo);
            s->trace->surface_flags = ti ? ti->flags : 0;
        }
    }
}

static void test_box_in_brush(sweep_t *s, const mapgen_bsp_brush_t *brush)
{
    if (!brush->numsides)
        return;

    for (int32_t i = 0; i < brush->numsides; i++) {
        const mapgen_bsp_brushside_t *side =
            MapGenBsp_BrushSide(s->bsp, (uint32_t)(brush->firstside + i));
        if (!side)
            return;
        const mapgen_bsp_plane_t *plane = MapGenBsp_Plane(s->bsp, side->planenum);
        if (!plane)
            return;

        float ofs[3];
        for (int j = 0; j < 3; j++)
            ofs[j] = plane->normal[j] < 0 ? s->maxs[j] : s->mins[j];
        float dist = plane->dist - dot3(ofs, plane->normal);

        if (dot3(s->start, plane->normal) - dist > 0)
            return;             /* outside this face, so outside the brush */
    }

    /* Inside every face: the box is in this brush. */
    s->trace->startsolid = true;
    s->trace->allsolid = true;
    s->trace->fraction = 0.0f;
    s->trace->contents = brush->contents;
}

static void trace_to_leaf(sweep_t *s, uint32_t leafnum, bool position_test)
{
    const mapgen_bsp_leaf_t *leaf = MapGenBsp_Leaf(s->bsp, leafnum);
    if (!leaf || !(leaf->contents & s->brushmask))
        return;

    for (uint32_t i = 0; i < leaf->numleafbrushes; i++) {
        uint32_t brushnum = MapGenBsp_LeafBrush(s->bsp, leaf->firstleafbrush + i);
        if (brushnum >= s->ctx->capacity)
            continue;
        /* The stamp lives in the CONTEXT, not in the brush. That is the single
           change that makes this safe to run on many threads. */
        if (s->ctx->stamps[brushnum] == s->ctx->generation)
            continue;
        s->ctx->stamps[brushnum] = s->ctx->generation;

        const mapgen_bsp_brush_t *brush = MapGenBsp_Brush(s->bsp, brushnum);
        if (!brush || !(brush->contents & s->brushmask))
            continue;

        if (position_test)
            test_box_in_brush(s, brush);
        else
            clip_box_to_brush(s, brush);

        if (s->trace->fraction <= 0.0f)
            return;
    }
}

static void recursive_hull_check(sweep_t *s, int32_t num, float p1f, float p2f,
                                 const float p1[3], const float p2[3], int depth)
{
    if (s->trace->fraction <= p1f)
        return;                 /* already hit something nearer */
    if (depth > 1024)
        return;                 /* a malformed tree must not become a hang */

    if (num < 0) {
        trace_to_leaf(s, (uint32_t)(-1 - num), false);
        return;
    }

    const mapgen_bsp_node_t *node = MapGenBsp_Node(s->bsp, (uint32_t)num);
    if (!node)
        return;
    const mapgen_bsp_plane_t *plane = MapGenBsp_Plane(s->bsp, node->planenum);
    if (!plane)
        return;

    float t1, t2, offset;
    if (plane->type < 3) {
        t1 = p1[plane->type] - plane->dist;
        t2 = p2[plane->type] - plane->dist;
        offset = s->extents[plane->type];
    } else {
        t1 = dot3(plane->normal, p1) - plane->dist;
        t2 = dot3(plane->normal, p2) - plane->dist;
        if (s->ispoint) {
            offset = 0;
        } else {
            offset = fabsf(s->extents[0] * plane->normal[0]) +
                     fabsf(s->extents[1] * plane->normal[1]) +
                     fabsf(s->extents[2] * plane->normal[2]);
        }
    }

    if (t1 >= offset && t2 >= offset) {
        recursive_hull_check(s, node->children[0], p1f, p2f, p1, p2, depth + 1);
        return;
    }
    if (t1 < -offset && t2 < -offset) {
        recursive_hull_check(s, node->children[1], p1f, p2f, p1, p2, depth + 1);
        return;
    }

    /* The sweep straddles the plane: split it. */
    int side;
    float frac, frac2;
    if (t1 < t2) {
        float idist = 1.0f / (t1 - t2);
        side = 1;
        frac2 = (t1 + offset + DIST_EPSILON) * idist;
        frac = (t1 - offset + DIST_EPSILON) * idist;
    } else if (t1 > t2) {
        float idist = 1.0f / (t1 - t2);
        side = 0;
        frac2 = (t1 - offset - DIST_EPSILON) * idist;
        frac = (t1 + offset + DIST_EPSILON) * idist;
    } else {
        side = 0;
        frac = 1.0f;
        frac2 = 0.0f;
    }

    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float midf = p1f + (p2f - p1f) * frac;
    float mid[3];
    for (int i = 0; i < 3; i++)
        mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    recursive_hull_check(s, node->children[side], p1f, midf, p1, mid, depth + 1);

    if (frac2 < 0) frac2 = 0;
    if (frac2 > 1) frac2 = 1;
    midf = p1f + (p2f - p1f) * frac2;
    for (int i = 0; i < 3; i++)
        mid[i] = p1[i] + frac2 * (p2[i] - p1[i]);
    recursive_hull_check(s, node->children[side ^ 1], midf, p2f, mid, p2, depth + 1);
}

/* Collect the leafs a box occupies, for a position test. */
static void box_leafs(sweep_t *s, int32_t num, const float mins[3], const float maxs[3], int depth)
{
    if (depth > 1024)
        return;
    if (num < 0) {
        trace_to_leaf(s, (uint32_t)(-1 - num), true);
        return;
    }
    const mapgen_bsp_node_t *node = MapGenBsp_Node(s->bsp, (uint32_t)num);
    if (!node)
        return;
    const mapgen_bsp_plane_t *plane = MapGenBsp_Plane(s->bsp, node->planenum);
    if (!plane)
        return;

    float dmin, dmax;
    if (plane->type < 3) {
        dmin = mins[plane->type] - plane->dist;
        dmax = maxs[plane->type] - plane->dist;
    } else {
        float corner_min[3], corner_max[3];
        for (int i = 0; i < 3; i++) {
            if (plane->normal[i] < 0) {
                corner_min[i] = maxs[i];
                corner_max[i] = mins[i];
            } else {
                corner_min[i] = mins[i];
                corner_max[i] = maxs[i];
            }
        }
        dmin = dot3(corner_min, plane->normal) - plane->dist;
        dmax = dot3(corner_max, plane->normal) - plane->dist;
    }

    if (dmin >= 0)
        box_leafs(s, node->children[0], mins, maxs, depth + 1);
    else if (dmax < 0)
        box_leafs(s, node->children[1], mins, maxs, depth + 1);
    else {
        box_leafs(s, node->children[0], mins, maxs, depth + 1);
        box_leafs(s, node->children[1], mins, maxs, depth + 1);
    }
}

/* ------------------------------------------------------------------------ */

/*
 * One sweep against one tree, in that tree's own frame.
 *
 * `out` is refined rather than replaced, so the world and every mover can be
 * swept through the same result and the nearest hit wins. The endpos is left
 * to the caller, which knows the world-space ray.
 */
static void sweep_tree(mapgen_trace_context_t *ctx, int32_t headnode,
                       const float start[3], const float end[3],
                       const float mins[3], const float maxs[3],
                       int32_t brushmask, mapgen_trace_result_t *out)
{
    /* A new traversal, so every stamp from the last one is stale. Wrapping is
       handled by clearing rather than by hoping 2^32 traces never happen. */
    ctx->generation++;
    if (ctx->generation == 0) {
        if (ctx->stamps && ctx->capacity)
            memset(ctx->stamps, 0, (size_t)ctx->capacity * sizeof(uint32_t));
        ctx->generation = 1;
    }

    sweep_t s;
    memset(&s, 0, sizeof(s));
    s.ctx = ctx;
    s.bsp = ctx->bsp;
    s.trace = out;
    s.brushmask = brushmask;
    for (int i = 0; i < 3; i++) {
        s.start[i] = start[i];
        s.end[i] = end[i];
        s.mins[i] = mins[i];
        s.maxs[i] = maxs[i];
    }
    s.ispoint = (mins[0] == 0 && mins[1] == 0 && mins[2] == 0 &&
                 maxs[0] == 0 && maxs[1] == 0 && maxs[2] == 0);
    for (int i = 0; i < 3; i++)
        s.extents[i] = -mins[i] > maxs[i] ? -mins[i] : maxs[i];

    if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2]) {
        /* Position test: which leafs does the box occupy right now. */
        float bmins[3], bmaxs[3];
        for (int i = 0; i < 3; i++) {
            bmins[i] = start[i] + mins[i] - 1.0f;
            bmaxs[i] = start[i] + maxs[i] + 1.0f;
        }
        box_leafs(&s, headnode, bmins, bmaxs, 0);
        return;
    }

    recursive_hull_check(&s, headnode, 0.0f, 1.0f, start, end, 0);
}

void MapGenTrace_Box(mapgen_trace_context_t *ctx,
                     const float start[3], const float end[3],
                     const float mins[3], const float maxs[3],
                     int32_t brushmask,
                     mapgen_trace_result_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->fraction = 1.0f;
    if (start) {
        out->endpos[0] = start[0];
        out->endpos[1] = start[1];
        out->endpos[2] = start[2];
    }
    if (!ctx || !ctx->bsp || !start || !end || !mins || !maxs)
        return;

    const mapgen_bsp_model_t *world = MapGenBsp_Model(ctx->bsp, 0);
    if (!world)
        return;

    sweep_tree(ctx, world->headnode, start, end, mins, maxs, brushmask, out);

    /*
     * A zero-length sweep needs no special case here. It comes back with a
     * fraction of one when the box is clear, and end IS start, so the first
     * branch below writes start; and with a fraction of zero when it is in a
     * solid, so the second writes start too.
     */
    if (out->fraction == 1.0f) {
        out->endpos[0] = end[0];
        out->endpos[1] = end[1];
        out->endpos[2] = end[2];
    } else {
        for (int i = 0; i < 3; i++)
            out->endpos[i] = start[i] + out->fraction * (end[i] - start[i]);
    }
}

void MapGenTrace_BoxModel(mapgen_trace_context_t *ctx, uint32_t model,
                          const float displacement[3],
                          const float start[3], const float end[3],
                          const float mins[3], const float maxs[3],
                          int32_t brushmask,
                          mapgen_trace_result_t *out)
{
    if (!ctx || !ctx->bsp || !start || !end || !mins || !maxs || !out)
        return;
    const mapgen_bsp_model_t *m = MapGenBsp_Model(ctx->bsp, model);
    if (!m)
        return;

    /* Into the model's own frame: its brushes are where they were drawn, and
       the displacement is how far the game has since carried them. */
    float p1[3], p2[3];
    for (int i = 0; i < 3; i++) {
        const float d = displacement ? displacement[i] : 0.0f;
        p1[i] = start[i] - d;
        p2[i] = end[i] - d;
    }

    const float before = out->fraction;
    sweep_tree(ctx, m->headnode, p1, p2, mins, maxs, brushmask, out);

    if (out->fraction < before) {
        for (int i = 0; i < 3; i++)
            out->endpos[i] = start[i] + out->fraction * (end[i] - start[i]);
    }
}

int32_t MapGenTrace_ModelContents(mapgen_trace_context_t *ctx, uint32_t model,
                                  const float displacement[3],
                                  const float point[3])
{
    if (!ctx || !ctx->bsp || !point)
        return 0;
    const mapgen_bsp_model_t *m = MapGenBsp_Model(ctx->bsp, model);
    if (!m)
        return 0;
    float p[3];
    for (int i = 0; i < 3; i++)
        p[i] = point[i] - (displacement ? displacement[i] : 0.0f);
    return MapGenBsp_PointContentsAt(ctx->bsp, m->headnode, p);
}

int32_t MapGenTrace_PointContents(mapgen_trace_context_t *ctx, const float point[3])
{
    if (!ctx || !ctx->bsp || !point)
        return 0;
    return MapGenBsp_PointContents(ctx->bsp, point);
}
