/*
 * MapGenGeometry - see inc/common/mapgen_geometry.h for why this exists.
 *
 * Three jobs live here and nowhere else: deriving a brush's real shape from
 * the half-spaces a compiler left behind, proving which model owns it, and
 * writing the result back out as a `.map` the pinned compiler will rebuild
 * into the same map. Callers see brushes, sides and entities; they never see a
 * winding, a plane index or an entity string.
 */

#include "common/mapgen_geometry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *MapGenGeometry_ResultName(mapgen_geometry_result_t r)
{
    switch (r) {
    case MAPGEN_GEOMETRY_OK:              return "OK";
    case MAPGEN_GEOMETRY_ERR_ARGS:        return "ERR_ARGS";
    case MAPGEN_GEOMETRY_ERR_MEMORY:      return "ERR_MEMORY";
    case MAPGEN_GEOMETRY_ERR_LIMIT:       return "ERR_LIMIT";
    case MAPGEN_GEOMETRY_ERR_DEGENERATE:  return "ERR_DEGENERATE";
    case MAPGEN_GEOMETRY_ERR_NON_FINITE:  return "ERR_NON_FINITE";
    case MAPGEN_GEOMETRY_ERR_OWNERSHIP:   return "ERR_OWNERSHIP";
    case MAPGEN_GEOMETRY_ERR_UNSUPPORTED: return "DONOR_GEOMETRY_UNSUPPORTED";
    }
    return "ERR_UNKNOWN";
}

struct mapgen_geometry_s {
    mapgen_geometry_brush_t  *brushes;
    uint32_t                  num_brushes;
    mapgen_geometry_side_t   *sides;
    uint32_t                  num_sides;
    mapgen_geometry_entity_t *entities;
    uint32_t                  num_entities;

    /* Key/value bytes, NUL-separated; `pair_at[2i]` and `[2i+1]` are the
       offsets of the i-th key and its value. One block so a clone is one
       memcpy and a bound is one number. */
    char                     *text;
    size_t                    text_len;
    size_t                    text_cap;
    uint32_t                 *pair_at;
    uint32_t                  num_pairs;
    uint32_t                  pair_cap;

    /* The visible mesh, independent of the brushes above. */
    mapgen_geometry_face_t   *faces;
    uint32_t                  num_faces;
    float                    *points;      /* three floats per point */
    uint32_t                  num_points;

    uint32_t                  num_models;
    uint32_t                  orphan_brushes;

    /* Capacity, so a candidate can grow. The donor's own extraction sizes
       these exactly and they only move when an edit adds something. */
    uint32_t                  brush_cap;
    uint32_t                  side_cap;
    uint32_t                  entity_cap;
};

/* ---- windings ------------------------------------------------------------- */

/*
 * A brush arrives as a pile of half-spaces with no shape of its own. Its
 * shape is what is left of each plane after every other plane has taken its
 * bite - so that is how it is derived, by clipping a plane-sized square down
 * against all of the brush's other planes.
 *
 * A side left with nothing is not an error and not something to discard: it is
 * a compiler bevel, redundant against its neighbours, and it is kept because
 * proving one redundant is a separate question from noticing it has no face.
 */

#ifndef MAPGEN_BASE_WINDING_EXTENT
#define MAPGEN_BASE_WINDING_EXTENT 65536.0f
#endif

#define WINDING_MAX_POINTS 256
#define WINDING_EPSILON    0.01f

/*
 * Double, deliberately. The square a winding starts as is 65536 units across,
 * where a float has 0.008 of slack per coordinate, and four clips later that
 * decides whether a hairline side has a face or is a bevel - which in turn
 * decides whether the writer may drop its plane. The independent oracle, which
 * works in double, disagreed with this file about exactly one such side.
 */
typedef struct {
    uint32_t count;
    double   p[WINDING_MAX_POINTS][3];
} winding_t;

static bool finite3(const float v[3])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static void base_winding(const float normal[3], float dist, winding_t *w)
{
    const double n[3] = { normal[0], normal[1], normal[2] };
    const double d = (double)dist;
    /* Any tangent will do; the one built from the smallest component is the
       one furthest from parallel, which is the one that stays well
       conditioned. */
    int minor = 0;
    for (int i = 1; i < 3; i++) {
        if (fabs(n[i]) < fabs(n[minor]))
            minor = i;
    }

    double t[3] = { 0, 0, 0 };
    t[minor] = 1.0;

    double u[3], v[3];
    const double dot = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
    for (int i = 0; i < 3; i++)
        u[i] = t[i] - n[i] * dot;
    const double ulen = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; i++)
        u[i] /= ulen;

    /* v = n x u, so that u x v is n and the winding runs counter-clockwise
       seen from the front - the order the compiler reads a face in. */
    v[0] = n[1] * u[2] - n[2] * u[1];
    v[1] = n[2] * u[0] - n[0] * u[2];
    v[2] = n[0] * u[1] - n[1] * u[0];

    /*
     * How far the plane-sized square starts out.
     *
     * A variable of the seam diagnosis, because it is also a precision
     * decision: at 65536 a float32 coordinate is spaced 0.0078 apart, so a
     * corner that survives four clips can land a hundredth of a unit from
     * where it belongs - and the plane stated through that corner is then a
     * hundredth out, which is exactly the tolerance the compiler uses to
     * decide whether two planes are the same one.
     */
    const double R = MAPGEN_BASE_WINDING_EXTENT;
    double c[3];
    for (int i = 0; i < 3; i++)
        c[i] = n[i] * d;

    const double su[4] = { -1, +1, +1, -1 };
    const double sv[4] = { -1, -1, +1, +1 };
    w->count = 4;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 3; i++)
            w->p[k][i] = c[i] + u[i] * (su[k] * R) + v[i] * (sv[k] * R);
    }
}

/* Keep the half that is BEHIND the plane - inside the solid. */
static void clip_winding(winding_t *w, const float normal[3], float dist)
{
    double   dists[WINDING_MAX_POINTS + 1];
    int      sides[WINDING_MAX_POINTS + 1];
    uint32_t counts[3] = { 0, 0, 0 };

    for (uint32_t i = 0; i < w->count; i++) {
        const double d = w->p[i][0] * (double)normal[0]
                       + w->p[i][1] * (double)normal[1]
                       + w->p[i][2] * (double)normal[2] - (double)dist;
        dists[i] = d;
        sides[i] = d > WINDING_EPSILON ? 0 : (d < -WINDING_EPSILON ? 1 : 2);
        counts[sides[i]]++;
    }
    if (!counts[0]) {
        return;                  /* nothing in front; the whole side survives */
    }
    if (!counts[1]) {
        w->count = 0;            /* nothing behind; this plane cuts it away */
        return;
    }
    dists[w->count] = dists[0];
    sides[w->count] = sides[0];

    winding_t out;
    out.count = 0;
    for (uint32_t i = 0; i < w->count; i++) {
        if (sides[i] != 0 && out.count < WINDING_MAX_POINTS)
            memcpy(out.p[out.count++], w->p[i], sizeof(out.p[0]));
        if (sides[i] == 2 || sides[i + 1] == 2 || sides[i + 1] == sides[i])
            continue;

        const uint32_t next = (i + 1) % w->count;
        const double f = dists[i] / (dists[i] - dists[i + 1]);
        if (out.count >= WINDING_MAX_POINTS)
            continue;
        for (int a = 0; a < 3; a++) {
            /* Snapping a crossing that lands on an axis keeps a plane that
               was axis-aligned in the donor axis-aligned here. */
            const double mid = w->p[i][a] + f * (w->p[next][a] - w->p[i][a]);
            out.p[out.count][a] = normal[a] == 1.0f  ? (double)dist
                                : normal[a] == -1.0f ? -(double)dist
                                : mid;
        }
        out.count++;
    }
    *w = out;
}

/*
 * Give a set of planes their shape: each side's winding, and from it the
 * brush's bounds and the side's centroid, area and samples.
 *
 * Returns how many sides ended up with a face of their own. Zero means the
 * planes enclose nothing.
 */
static uint32_t shape_brush(mapgen_geometry_side_t *sides, uint32_t count,
                            float mins[3], float maxs[3])
{
    winding_t *windings = malloc((size_t)count * sizeof(*windings));
    if (!windings)
        return 0;

    for (int a = 0; a < 3; a++) {
        mins[a] = MAPGEN_GEOMETRY_WORLD_EXTENT;
        maxs[a] = -MAPGEN_GEOMETRY_WORLD_EXTENT;
    }

    uint32_t shaped = 0;
    for (uint32_t s = 0; s < count; s++) {
        base_winding(sides[s].normal, sides[s].dist, &windings[s]);
        for (uint32_t o = 0; o < count && windings[s].count; o++) {
            if (o == s)
                continue;
            clip_winding(&windings[s], sides[o].normal, sides[o].dist);
        }

        sides[s].bevel = windings[s].count < 3;
        sides[s].area = 0.0f;
        sides[s].num_samples = 0;
        if (sides[s].bevel)
            continue;
        shaped++;

        double area2 = 0.0, cx = 0.0, cy = 0.0, cz = 0.0;
        for (uint32_t p = 1; p + 1 < windings[s].count; p++) {
            double e1[3], e2[3], cross[3];
            for (int a = 0; a < 3; a++) {
                e1[a] = windings[s].p[p][a] - windings[s].p[0][a];
                e2[a] = windings[s].p[p + 1][a] - windings[s].p[0][a];
            }
            cross[0] = e1[1] * e2[2] - e1[2] * e2[1];
            cross[1] = e1[2] * e2[0] - e1[0] * e2[2];
            cross[2] = e1[0] * e2[1] - e1[1] * e2[0];
            const double tri = sqrt(cross[0] * cross[0] + cross[1] * cross[1]
                                  + cross[2] * cross[2]);
            area2 += tri;
            cx += tri * (windings[s].p[0][0] + windings[s].p[p][0]
                         + windings[s].p[p + 1][0]) / 3.0;
            cy += tri * (windings[s].p[0][1] + windings[s].p[p][1]
                         + windings[s].p[p + 1][1]) / 3.0;
            cz += tri * (windings[s].p[0][2] + windings[s].p[p][2]
                         + windings[s].p[p + 1][2]) / 3.0;
        }
        sides[s].area = (float)(area2 * 0.5);
        if (area2 > 0.0) {
            sides[s].center[0] = (float)(cx / area2);
            sides[s].center[1] = (float)(cy / area2);
            sides[s].center[2] = (float)(cz / area2);
        }

        sides[s].num_samples = 1;
        memcpy(sides[s].sample[0], sides[s].center, sizeof(sides[s].center));
        const uint32_t step = windings[s].count
                            / (MAPGEN_GEOMETRY_FACE_SAMPLES - 1) + 1;
        for (uint32_t p = 0; p < windings[s].count
             && sides[s].num_samples < MAPGEN_GEOMETRY_FACE_SAMPLES; p += step) {
            for (int a = 0; a < 3; a++) {
                sides[s].sample[sides[s].num_samples][a] =
                    0.5f * (sides[s].center[a] + windings[s].p[p][a]);
            }
            sides[s].num_samples++;
        }

        for (uint32_t p = 0; p < windings[s].count; p++) {
            for (int a = 0; a < 3; a++) {
                if (windings[s].p[p][a] < mins[a])
                    mins[a] = windings[s].p[p][a];
                if (windings[s].p[p][a] > maxs[a])
                    maxs[a] = windings[s].p[p][a];
            }
        }
    }

    free(windings);
    return shaped;
}

/* ---- ownership ------------------------------------------------------------ */

/*
 * Which model a brush belongs to is decided by which model's collision tree
 * reaches it, never by where it happens to be. A door's solids are the door's
 * because the door's tree owns the leaf they sit in; two doors standing in the
 * same doorway would defeat any answer based on proximity.
 */
static mapgen_geometry_result_t mark_owner(const mapgen_bsp_t *bsp,
                                           int32_t root, uint32_t model,
                                           uint32_t *owner, uint8_t *claimed)
{
    const uint32_t num_nodes = MapGenBsp_NumNodes(bsp);
    const uint32_t num_leafs = MapGenBsp_NumLeafs(bsp);
    const uint32_t num_brushes = MapGenBsp_NumBrushes(bsp);

    int32_t *stack = malloc((size_t)(num_nodes + 1) * sizeof(*stack));
    uint8_t *seen = calloc(num_nodes ? num_nodes : 1, sizeof(*seen));
    if (!stack || !seen) {
        free(stack);
        free(seen);
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    }

    size_t top = 0;
    stack[top++] = root;
    mapgen_geometry_result_t result = MAPGEN_GEOMETRY_OK;

    while (top) {
        const int32_t at = stack[--top];
        if (at < 0) {
            const uint32_t leaf = (uint32_t)(-1 - at);
            if (leaf >= num_leafs)
                continue;
            const mapgen_bsp_leaf_t *l = MapGenBsp_Leaf(bsp, leaf);
            for (uint32_t i = 0; i < (uint32_t)l->numleafbrushes; i++) {
                const uint32_t b = MapGenBsp_LeafBrush(bsp, l->firstleafbrush + i);
                if (b >= num_brushes)
                    continue;
                if (claimed[b] && owner[b] != model) {
                    result = MAPGEN_GEOMETRY_ERR_OWNERSHIP;
                    top = 0;
                    break;
                }
                owner[b] = model;
                claimed[b] = 1;
            }
            continue;
        }
        if ((uint32_t)at >= num_nodes || seen[at])
            continue;
        seen[at] = 1;
        const mapgen_bsp_node_t *n = MapGenBsp_Node(bsp, (uint32_t)at);
        if (top + 2 > (size_t)num_nodes + 1)
            continue;
        stack[top++] = n->children[0];
        stack[top++] = n->children[1];
    }

    free(stack);
    free(seen);
    return result;
}

/* ---- text storage --------------------------------------------------------- */

static bool reserve_text(mapgen_geometry_t *g, size_t extra)
{
    if (g->text_len + extra <= g->text_cap)
        return true;
    size_t want = g->text_cap ? g->text_cap : 4096;
    while (want < g->text_len + extra)
        want *= 2;
    if (want > MAPGEN_GEOMETRY_MAX_KEYVALUES)
        return false;
    char *grown = realloc(g->text, want);
    if (!grown)
        return false;
    g->text = grown;
    g->text_cap = want;
    return true;
}

static bool add_pair(mapgen_geometry_t *g, const char *key, size_t klen,
                     const char *value, size_t vlen)
{
    if (g->num_pairs == g->pair_cap) {
        const uint32_t want = g->pair_cap ? g->pair_cap * 2 : 64;
        uint32_t *grown = realloc(g->pair_at, (size_t)want * 2 * sizeof(*grown));
        if (!grown)
            return false;
        g->pair_at = grown;
        g->pair_cap = want;
    }
    if (!reserve_text(g, klen + vlen + 2))
        return false;

    g->pair_at[g->num_pairs * 2] = (uint32_t)g->text_len;
    memcpy(g->text + g->text_len, key, klen);
    g->text_len += klen;
    g->text[g->text_len++] = '\0';

    g->pair_at[g->num_pairs * 2 + 1] = (uint32_t)g->text_len;
    memcpy(g->text + g->text_len, value, vlen);
    g->text_len += vlen;
    g->text[g->text_len++] = '\0';

    g->num_pairs++;
    return true;
}

/* ---- entities -------------------------------------------------------------- */

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static mapgen_geometry_result_t parse_entities(mapgen_geometry_t *g,
                                               const mapgen_bsp_t *bsp)
{
    uint32_t length = 0;
    const char *at = MapGenBsp_Entities(bsp, &length);
    if (!at)
        return MAPGEN_GEOMETRY_OK;

    uint32_t capacity = 64;
    g->entities = calloc(capacity, sizeof(*g->entities));
    if (!g->entities)
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    g->entity_cap = capacity;

    while (*at) {
        at = skip_space(at);
        if (*at != '{')
            break;
        at++;

        if (g->num_entities == capacity) {
            if (capacity * 2 > MAPGEN_GEOMETRY_MAX_ENTITIES)
                return MAPGEN_GEOMETRY_ERR_LIMIT;
            mapgen_geometry_entity_t *grown =
                realloc(g->entities, (size_t)capacity * 2 * sizeof(*grown));
            if (!grown)
                return MAPGEN_GEOMETRY_ERR_MEMORY;
            memset(grown + capacity, 0, (size_t)capacity * sizeof(*grown));
            g->entities = grown;
            capacity *= 2;
            g->entity_cap = capacity;
        }

        mapgen_geometry_entity_t *e = &g->entities[g->num_entities];
        e->first_pair = g->num_pairs;

        for (;;) {
            at = skip_space(at);
            if (*at == '}') {
                at++;
                break;
            }
            if (*at != '"')
                return MAPGEN_GEOMETRY_ERR_UNSUPPORTED;
            const char *key = ++at;
            while (*at && *at != '"')
                at++;
            if (*at != '"')
                return MAPGEN_GEOMETRY_ERR_UNSUPPORTED;
            const size_t klen = (size_t)(at - key);
            at = skip_space(at + 1);
            if (*at != '"')
                return MAPGEN_GEOMETRY_ERR_UNSUPPORTED;
            const char *value = ++at;
            while (*at && *at != '"')
                at++;
            if (*at != '"')
                return MAPGEN_GEOMETRY_ERR_UNSUPPORTED;
            const size_t vlen = (size_t)(at - value);
            at++;

            if (!add_pair(g, key, klen, value, vlen))
                return MAPGEN_GEOMETRY_ERR_LIMIT;

            /*
             * The two keys that bind an entity to geometry are read as they
             * arrive. `model "*n"` is the ONLY thing that makes a brush model
             * a door - the compiler will renumber it on the way out, so the
             * binding is what has to survive, not the ordinal.
             */
            if (klen == 5 && !memcmp(key, "model", 5) && vlen > 1
                && value[0] == '*') {
                e->model = (uint32_t)strtoul(value + 1, NULL, 10);
            } else if (klen == 6 && !memcmp(key, "origin", 6)) {
                char buf[128];
                const size_t n = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
                memcpy(buf, value, n);
                buf[n] = '\0';
                char *cursor = buf;
                for (int i = 0; i < 3; i++)
                    e->origin[i] = strtof(cursor, &cursor);
                e->has_origin = true;
            }
        }

        e->num_pairs = g->num_pairs - e->first_pair;
        g->num_entities++;
    }
    return MAPGEN_GEOMETRY_OK;
}

/* ---- the visible mesh -------------------------------------------------------- */

/*
 * Which model a face belongs to, from the model's own face range. Faces are
 * contiguous per model in a compiled BSP, which is the one place ownership can
 * be read directly rather than proven.
 */
static uint32_t face_model(const mapgen_bsp_t *bsp, uint32_t face)
{
    for (uint32_t m = 0; m < MapGenBsp_NumModels(bsp); m++) {
        const mapgen_bsp_model_t *model = MapGenBsp_Model(bsp, m);
        if (model->firstface >= 0 && face >= (uint32_t)model->firstface
            && face < (uint32_t)(model->firstface + model->numfaces))
            return m;
    }
    return 0;
}

static mapgen_geometry_result_t extract_faces(mapgen_geometry_t *g,
                                              const mapgen_bsp_t *bsp)
{
    const uint32_t count = MapGenBsp_NumFaces(bsp);
    const uint32_t num_texinfo = MapGenBsp_NumTexInfo(bsp);
    const uint32_t num_planes = MapGenBsp_NumPlanes(bsp);
    const uint32_t num_verts = MapGenBsp_NumVertices(bsp);
    const uint32_t num_edges = MapGenBsp_NumEdges(bsp);
    const uint32_t num_surfedges = MapGenBsp_NumSurfEdges(bsp);

    if (count > MAPGEN_GEOMETRY_MAX_FACES
        || num_surfedges > MAPGEN_GEOMETRY_MAX_POINTS)
        return MAPGEN_GEOMETRY_ERR_LIMIT;
    if (!count)
        return MAPGEN_GEOMETRY_OK;

    g->faces = calloc(count, sizeof(*g->faces));
    g->points = calloc((size_t)num_surfedges * 3 + 3, sizeof(*g->points));
    if (!g->faces || !g->points)
        return MAPGEN_GEOMETRY_ERR_MEMORY;

    for (uint32_t f = 0; f < count; f++) {
        const mapgen_bsp_face_t *src = MapGenBsp_Face(bsp, f);
        if (src->numedges < 3 || src->firstedge < 0
            || (uint32_t)(src->firstedge + src->numedges) > num_surfedges)
            continue;
        if (src->planenum >= num_planes)
            return MAPGEN_GEOMETRY_ERR_LIMIT;

        mapgen_geometry_face_t *dst = &g->faces[g->num_faces];
        const mapgen_bsp_plane_t *plane = MapGenBsp_Plane(bsp, src->planenum);
        const float sign = src->side ? -1.0f : 1.0f;
        for (int a = 0; a < 3; a++)
            dst->normal[a] = plane->normal[a] * sign;
        dst->dist = plane->dist * sign;
        dst->model = face_model(bsp, f);
        dst->first_point = g->num_points;
        dst->num_points = (uint32_t)src->numedges;

        if (src->texinfo >= 0 && (uint32_t)src->texinfo < num_texinfo) {
            const mapgen_bsp_texinfo_t *ti =
                MapGenBsp_TexInfo(bsp, (uint32_t)src->texinfo);
            dst->flags = ti->flags;
            dst->value = ti->value;
            memcpy(dst->texture, ti->texture, sizeof(dst->texture));
        }

        for (int32_t e = 0; e < src->numedges; e++) {
            const int32_t se =
                MapGenBsp_SurfEdge(bsp, (uint32_t)(src->firstedge + e));
            const uint32_t index = (uint32_t)(se < 0 ? -se : se);
            if (index >= num_edges)
                return MAPGEN_GEOMETRY_ERR_LIMIT;
            const mapgen_bsp_edge_t *edge = MapGenBsp_Edge(bsp, index);
            const uint32_t v = se < 0 ? edge->v[1] : edge->v[0];
            if (v >= num_verts)
                return MAPGEN_GEOMETRY_ERR_LIMIT;
            const mapgen_bsp_vertex_t *point = MapGenBsp_Vertex(bsp, v);
            memcpy(&g->points[g->num_points * 3], point->point,
                   3 * sizeof(float));
            g->num_points++;
        }

        /* The fan sum about the first point, exact for the convex polygons a
           BSP face always is. */
        double area2 = 0.0;
        const float *p0 = &g->points[dst->first_point * 3];
        for (uint32_t p = 1; p + 1 < dst->num_points; p++) {
            const float *pa = &g->points[(dst->first_point + p) * 3];
            const float *pb = &g->points[(dst->first_point + p + 1) * 3];
            double e1[3], e2[3], cross[3];
            for (int a = 0; a < 3; a++) {
                e1[a] = pa[a] - p0[a];
                e2[a] = pb[a] - p0[a];
            }
            cross[0] = e1[1] * e2[2] - e1[2] * e2[1];
            cross[1] = e1[2] * e2[0] - e1[0] * e2[2];
            cross[2] = e1[0] * e2[1] - e1[1] * e2[0];
            area2 += sqrt(cross[0] * cross[0] + cross[1] * cross[1]
                          + cross[2] * cross[2]);
        }
        dst->area = (float)(area2 * 0.5);
        g->num_faces++;
    }
    return MAPGEN_GEOMETRY_OK;
}

/* ---- extraction ------------------------------------------------------------ */

mapgen_geometry_result_t MapGenGeometry_FromBsp(const mapgen_bsp_t *bsp,
                                                mapgen_geometry_t **out)
{
    if (out)
        *out = NULL;
    if (!bsp || !out)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    const uint32_t num_brushes = MapGenBsp_NumBrushes(bsp);
    const uint32_t num_sides = MapGenBsp_NumBrushSides(bsp);
    const uint32_t num_models = MapGenBsp_NumModels(bsp);
    const uint32_t num_planes = MapGenBsp_NumPlanes(bsp);
    const uint32_t num_texinfo = MapGenBsp_NumTexInfo(bsp);

    /* Sized before it is trusted. */
    if (num_brushes > MAPGEN_GEOMETRY_MAX_BRUSHES
        || num_sides > MAPGEN_GEOMETRY_MAX_SIDES
        || num_models > MAPGEN_GEOMETRY_MAX_MODELS)
        return MAPGEN_GEOMETRY_ERR_LIMIT;
    if (!num_brushes || !num_models)
        return MAPGEN_GEOMETRY_ERR_UNSUPPORTED;

    mapgen_geometry_t *g = calloc(1, sizeof(*g));
    if (!g)
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    g->num_models = num_models;

    uint32_t *owner = calloc(num_brushes, sizeof(*owner));
    uint8_t *claimed = calloc(num_brushes, sizeof(*claimed));
    g->brushes = calloc(num_brushes, sizeof(*g->brushes));
    g->sides = calloc(num_sides ? num_sides : 1, sizeof(*g->sides));
    g->brush_cap = num_brushes;
    g->side_cap = num_sides;
    if (!owner || !claimed || !g->brushes || !g->sides) {
        free(owner);
        free(claimed);
        MapGenGeometry_Free(g);
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    }

    mapgen_geometry_result_t rc = MAPGEN_GEOMETRY_OK;
    for (uint32_t m = 0; m < num_models && rc == MAPGEN_GEOMETRY_OK; m++) {
        const mapgen_bsp_model_t *model = MapGenBsp_Model(bsp, m);
        rc = mark_owner(bsp, model->headnode, m, owner, claimed);
    }
    if (rc != MAPGEN_GEOMETRY_OK) {
        free(owner);
        free(claimed);
        MapGenGeometry_Free(g);
        return rc;
    }

    winding_t *windings = malloc(MAPGEN_GEOMETRY_MAX_SIDES_PER_BRUSH
                                 * sizeof(*windings));
    if (!windings) {
        free(owner);
        free(claimed);
        MapGenGeometry_Free(g);
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    }

    for (uint32_t b = 0; b < num_brushes; b++) {
        const mapgen_bsp_brush_t *src = MapGenBsp_Brush(bsp, b);
        if (src->numsides <= 0
            || (uint32_t)src->numsides > MAPGEN_GEOMETRY_MAX_SIDES_PER_BRUSH) {
            rc = MAPGEN_GEOMETRY_ERR_LIMIT;
            break;
        }
        const uint32_t count = (uint32_t)src->numsides;
        const uint32_t first = (uint32_t)src->firstside;
        if (first > num_sides || first + count > num_sides) {
            rc = MAPGEN_GEOMETRY_ERR_LIMIT;
            break;
        }

        /* Each side's face is its plane, minus every bite the others take. */
        for (uint32_t s = 0; s < count; s++) {
            const mapgen_bsp_brushside_t *bs = MapGenBsp_BrushSide(bsp, first + s);
            if (bs->planenum >= num_planes) {
                rc = MAPGEN_GEOMETRY_ERR_LIMIT;
                break;
            }
            const mapgen_bsp_plane_t *pl = MapGenBsp_Plane(bsp, bs->planenum);
            if (!finite3(pl->normal) || !isfinite(pl->dist)) {
                rc = MAPGEN_GEOMETRY_ERR_NON_FINITE;
                break;
            }
            base_winding(pl->normal, pl->dist, &windings[s]);
            for (uint32_t o = 0; o < count && windings[s].count; o++) {
                if (o == s)
                    continue;
                const mapgen_bsp_brushside_t *ob =
                    MapGenBsp_BrushSide(bsp, first + o);
                if (ob->planenum >= num_planes || ob->planenum == bs->planenum)
                    continue;
                const mapgen_bsp_plane_t *op = MapGenBsp_Plane(bsp, ob->planenum);
                clip_winding(&windings[s], op->normal, op->dist);
            }
        }
        if (rc != MAPGEN_GEOMETRY_OK)
            break;

        mapgen_geometry_brush_t *dst = &g->brushes[g->num_brushes];
        dst->first_side = g->num_sides;
        dst->num_sides = count;
        dst->contents = src->contents;
        dst->model = claimed[b] ? owner[b] : 0;
        if (!claimed[b])
            g->orphan_brushes++;
        for (int a = 0; a < 3; a++) {
            dst->mins[a] = MAPGEN_GEOMETRY_WORLD_EXTENT;
            dst->maxs[a] = -MAPGEN_GEOMETRY_WORLD_EXTENT;
        }

        uint32_t shaped = 0;
        for (uint32_t s = 0; s < count; s++) {
            const mapgen_bsp_brushside_t *bs = MapGenBsp_BrushSide(bsp, first + s);
            const mapgen_bsp_plane_t *pl = MapGenBsp_Plane(bsp, bs->planenum);
            mapgen_geometry_side_t *side = &g->sides[g->num_sides + s];

            memcpy(side->normal, pl->normal, sizeof(side->normal));
            side->dist = pl->dist;
            side->bevel = windings[s].count < 3;
            if (!side->bevel)
                shaped++;

            if (bs->texinfo >= 0 && (uint32_t)bs->texinfo < num_texinfo) {
                const mapgen_bsp_texinfo_t *ti =
                    MapGenBsp_TexInfo(bsp, (uint32_t)bs->texinfo);
                memcpy(side->axis, ti->axis, sizeof(side->axis));
                side->flags = ti->flags;
                side->value = ti->value;
                memcpy(side->texture, ti->texture, sizeof(side->texture));
            } else {
                /* A side the compiler added for collision and never textured.
                   It still bounds the solid, so it is kept and given a mapping
                   that cannot stretch. */
                side->axis[0][0] = 1.0f;
                side->axis[1][2] = -1.0f;
                side->texture[0] = '\0';
            }

            for (uint32_t p = 0; p < windings[s].count; p++) {
                for (int a = 0; a < 3; a++) {
                    if (windings[s].p[p][a] < dst->mins[a])
                        dst->mins[a] = windings[s].p[p][a];
                    if (windings[s].p[p][a] > dst->maxs[a])
                        dst->maxs[a] = windings[s].p[p][a];
                }
            }

            /*
             * The face's centroid and area, kept rather than discarded with
             * the winding: an operator asking "is there room beyond this
             * surface" needs somewhere to ask from, and an area-weighted
             * coverage axis needs the area. Both are the fan sum about the
             * first point, which is exact for a convex polygon.
             */
            if (windings[s].count >= 3) {
                double area2 = 0.0;
                double cx = 0.0, cy = 0.0, cz = 0.0;
                for (uint32_t p = 1; p + 1 < windings[s].count; p++) {
                    double e1[3], e2[3], cross[3];
                    for (int a = 0; a < 3; a++) {
                        e1[a] = windings[s].p[p][a] - windings[s].p[0][a];
                        e2[a] = windings[s].p[p + 1][a] - windings[s].p[0][a];
                    }
                    cross[0] = e1[1] * e2[2] - e1[2] * e2[1];
                    cross[1] = e1[2] * e2[0] - e1[0] * e2[2];
                    cross[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    const double tri = sqrt(cross[0] * cross[0]
                                          + cross[1] * cross[1]
                                          + cross[2] * cross[2]);
                    area2 += tri;
                    cx += tri * (windings[s].p[0][0] + windings[s].p[p][0]
                                 + windings[s].p[p + 1][0]) / 3.0;
                    cy += tri * (windings[s].p[0][1] + windings[s].p[p][1]
                                 + windings[s].p[p + 1][1]) / 3.0;
                    cz += tri * (windings[s].p[0][2] + windings[s].p[p][2]
                                 + windings[s].p[p + 1][2]) / 3.0;
                }
                side->area = (float)(area2 * 0.5);
                if (area2 > 0.0) {
                    side->center[0] = (float)(cx / area2);
                    side->center[1] = (float)(cy / area2);
                    side->center[2] = (float)(cz / area2);
                }

                /* The centroid, then halfway out to each corner: inside the
                   face by construction, and spread across all of it. */
                /*
                 * Three widely separated winding points: the first, and the two
                 * furthest from it and from each other. Widely separated
                 * because the compiler derives the plane from a cross product,
                 * and three points huddled together derive it badly.
                 */
                if (windings[s].count >= 3) {
                    uint32_t b = 1, c = 2;
                    double best = -1.0;
                    for (uint32_t i = 1; i < windings[s].count; i++) {
                        for (uint32_t j = i + 1; j < windings[s].count; j++) {
                            double area = 0.0;
                            double e1[3], e2[3], cross[3];
                            for (int a = 0; a < 3; a++) {
                                e1[a] = windings[s].p[i][a] - windings[s].p[0][a];
                                e2[a] = windings[s].p[j][a] - windings[s].p[0][a];
                            }
                            cross[0] = e1[1] * e2[2] - e1[2] * e2[1];
                            cross[1] = e1[2] * e2[0] - e1[0] * e2[2];
                            cross[2] = e1[0] * e2[1] - e1[1] * e2[0];
                            area = cross[0] * cross[0] + cross[1] * cross[1]
                                 + cross[2] * cross[2];
                            if (area > best) {
                                best = area;
                                b = i;
                                c = j;
                            }
                        }
                    }
                    if (best > 0.0) {
                        memcpy(side->anchor[0], windings[s].p[0], 3 * sizeof(float));
                        memcpy(side->anchor[1], windings[s].p[b], 3 * sizeof(float));
                        memcpy(side->anchor[2], windings[s].p[c], 3 * sizeof(float));
                        side->has_anchor = true;
                    }
                }

                side->num_samples = 1;
                memcpy(side->sample[0], side->center, sizeof(side->center));
                const uint32_t step = windings[s].count
                                    / (MAPGEN_GEOMETRY_FACE_SAMPLES - 1) + 1;
                for (uint32_t p = 0; p < windings[s].count
                     && side->num_samples < MAPGEN_GEOMETRY_FACE_SAMPLES;
                     p += step) {
                    for (int a = 0; a < 3; a++) {
                        side->sample[side->num_samples][a] =
                            0.5f * (side->center[a] + windings[s].p[p][a]);
                    }
                    side->num_samples++;
                }
            }
        }

        /* A pile of half-spaces enclosing nothing is not a brush, and writing
           one out would hand the compiler a solid it cannot build. */
        if (!shaped) {
            rc = MAPGEN_GEOMETRY_ERR_DEGENERATE;
            break;
        }

        g->num_sides += count;
        g->num_brushes++;
    }

    free(windings);
    free(owner);
    free(claimed);
    if (rc != MAPGEN_GEOMETRY_OK) {
        MapGenGeometry_Free(g);
        return rc;
    }

    rc = extract_faces(g, bsp);
    if (rc != MAPGEN_GEOMETRY_OK) {
        MapGenGeometry_Free(g);
        return rc;
    }

    rc = parse_entities(g, bsp);
    if (rc != MAPGEN_GEOMETRY_OK) {
        MapGenGeometry_Free(g);
        return rc;
    }

    *out = g;
    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_Clone(const mapgen_geometry_t *src,
                                              mapgen_geometry_t **out)
{
    if (out)
        *out = NULL;
    if (!src || !out)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    mapgen_geometry_t *g = calloc(1, sizeof(*g));
    if (!g)
        return MAPGEN_GEOMETRY_ERR_MEMORY;

    g->num_brushes = src->num_brushes;
    g->num_sides = src->num_sides;
    g->num_entities = src->num_entities;
    g->num_pairs = src->num_pairs;
    g->pair_cap = src->num_pairs;
    g->text_len = g->text_cap = src->text_len;
    g->num_models = src->num_models;
    g->orphan_brushes = src->orphan_brushes;

    g->brushes = malloc((src->num_brushes ? src->num_brushes : 1) * sizeof(*g->brushes));
    g->sides = malloc((src->num_sides ? src->num_sides : 1) * sizeof(*g->sides));
    g->entities = malloc((src->num_entities ? src->num_entities : 1) * sizeof(*g->entities));
    g->pair_at = malloc((src->num_pairs ? src->num_pairs : 1) * 2 * sizeof(*g->pair_at));
    g->text = malloc(src->text_len ? src->text_len : 1);
    if (!g->brushes || !g->sides || !g->entities || !g->pair_at || !g->text) {
        MapGenGeometry_Free(g);
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    }

    memcpy(g->brushes, src->brushes, src->num_brushes * sizeof(*g->brushes));
    memcpy(g->sides, src->sides, src->num_sides * sizeof(*g->sides));
    memcpy(g->entities, src->entities, src->num_entities * sizeof(*g->entities));
    memcpy(g->pair_at, src->pair_at, (size_t)src->num_pairs * 2 * sizeof(*g->pair_at));
    memcpy(g->text, src->text, src->text_len);

    /*
     * The mesh comes across as the DONOR's, and a candidate that edits its
     * geometry must not be believed about it afterwards: a face table copied
     * from before an edit describes a map that no longer exists. It is carried
     * so that an unedited clone can still be compared, and every consumer is
     * required to re-extract from the compiled candidate instead of trusting
     * this - which is what "compiled truth is the final authority" means.
     */
    g->num_faces = src->num_faces;
    g->num_points = src->num_points;
    if (src->num_faces) {
        g->faces = malloc((size_t)src->num_faces * sizeof(*g->faces));
        g->points = malloc((size_t)src->num_points * 3 * sizeof(*g->points) + 3);
        if (!g->faces || !g->points) {
            MapGenGeometry_Free(g);
            return MAPGEN_GEOMETRY_ERR_MEMORY;
        }
        memcpy(g->faces, src->faces, (size_t)src->num_faces * sizeof(*g->faces));
        memcpy(g->points, src->points,
               (size_t)src->num_points * 3 * sizeof(*g->points));
    }
    g->brush_cap = src->num_brushes;
    g->side_cap = src->num_sides;
    g->entity_cap = src->num_entities;

    *out = g;
    return MAPGEN_GEOMETRY_OK;
}

void MapGenGeometry_Free(mapgen_geometry_t *g)
{
    if (!g)
        return;
    free(g->brushes);
    free(g->sides);
    free(g->entities);
    free(g->pair_at);
    free(g->text);
    free(g->faces);
    free(g->points);
    free(g);
}

uint32_t MapGenGeometry_NumBrushes(const mapgen_geometry_t *g)
{
    return g ? g->num_brushes : 0;
}

uint32_t MapGenGeometry_NumSides(const mapgen_geometry_t *g)
{
    return g ? g->num_sides : 0;
}

uint32_t MapGenGeometry_NumModels(const mapgen_geometry_t *g)
{
    return g ? g->num_models : 0;
}

uint32_t MapGenGeometry_NumEntities(const mapgen_geometry_t *g)
{
    return g ? g->num_entities : 0;
}

uint32_t MapGenGeometry_NumFaces(const mapgen_geometry_t *g)
{
    return g ? g->num_faces : 0;
}

/* ---- surfaces a compiler cannot build ------------------------------------- */

/*
 * Two faces are on the same plane when they face the same way from the same
 * distance. The tolerances are the writer's: a .map states a plane as three
 * integer points, so anything finer than this cannot survive the round trip
 * and anything coarser would call two different walls one wall.
 */
#define SAME_NORMAL   0.001f
#define SAME_DIST     0.05f
#define ON_EDGE       0.10f
#define OFF_CORNER    0.50f
#define FACE_MIN_AREA 0.10f

typedef struct {
    uint32_t brush;
    uint32_t first, count;      /* into the shared point pool */
    float    normal[3];
    float    dist;
} flat_face_t;

static bool same_plane(const flat_face_t *a, const flat_face_t *b)
{
    return fabsf(a->normal[0] - b->normal[0]) <= SAME_NORMAL
        && fabsf(a->normal[1] - b->normal[1]) <= SAME_NORMAL
        && fabsf(a->normal[2] - b->normal[2]) <= SAME_NORMAL
        && fabsf(a->dist - b->dist) <= SAME_DIST;
}

/*
 * Does this point sit in the middle of that edge?
 *
 * Not at either end - a shared corner is how two faces are SUPPOSED to meet -
 * and near enough to the line that the compiler will put a vertex of one face
 * where the other face has none. That is the T.
 */
static bool splits_edge(const double p[3], const double a[3], const double b[3])
{
    double ab[3], ap[3];
    for (int i = 0; i < 3; i++) {
        ab[i] = b[i] - a[i];
        ap[i] = p[i] - a[i];
    }
    const double len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    if (len2 <= 0.0)
        return false;
    const double t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2;
    const double len = sqrt(len2);
    if (t * len <= OFF_CORNER || (1.0 - t) * len <= OFF_CORNER)
        return false;

    double off = 0.0;
    for (int i = 0; i < 3; i++) {
        const double d = ap[i] - t * ab[i];
        off += d * d;
    }
    return sqrt(off) <= ON_EDGE;
}

/*
 * Could anybody stand in front of this face?
 *
 * A step off the plane along its own normal, asked of the compiled map: inside
 * a wall reads solid, and so does the void outside the hull, which is the
 * answer that matters here - the outside of a sealed box is not a place.
 */
/* The same bit the collision hull uses; a leaf outside the map has it too. */
#define CONTENTS_SOLID_BIT  0x00000001

static bool anyone_can_see(const mapgen_bsp_t *space, const double at[3],
                           const float normal[3])
{
    if (!space)
        return true;
    const float off[3] = { (float)at[0] + normal[0] * 2.0f,
                           (float)at[1] + normal[1] * 2.0f,
                           (float)at[2] + normal[2] * 2.0f };
    return !(MapGenBsp_PointContents(space, off) & CONTENTS_SOLID_BIT);
}

uint32_t MapGenGeometry_SurfaceFaults(const mapgen_geometry_t *g,
                                      const mapgen_bsp_t *space)
{
    if (!g || !g->num_brushes)
        return 0;

    flat_face_t *faces = calloc(g->num_sides ? g->num_sides : 1, sizeof(*faces));
    double *pool = calloc((size_t)(g->num_sides ? g->num_sides : 1)
                          * WINDING_MAX_POINTS * 3, sizeof(*pool));
    winding_t *w = malloc(sizeof(*w));
    if (!faces || !pool || !w) {
        free(faces);
        free(pool);
        free(w);
        return 0;
    }

    uint32_t faults = 0, num_faces = 0, used = 0;

    for (uint32_t b = 0; b < g->num_brushes; b++) {
        const mapgen_geometry_brush_t *brush = &g->brushes[b];
        const mapgen_geometry_side_t *sides = &g->sides[brush->first_side];
        uint32_t shaped = 0;

        for (uint32_t s = 0; s < brush->num_sides; s++) {
            base_winding(sides[s].normal, sides[s].dist, w);
            for (uint32_t o = 0; o < brush->num_sides && w->count; o++) {
                if (o == s)
                    continue;
                clip_winding(w, sides[o].normal, sides[o].dist);
            }
            if (w->count < 3) {
                /*
                 * A side with nothing left of it is a bevel, which is normal
                 * and kept; only a side the donor itself called a face and
                 * which an edit has since cut away is a fault.
                 */
                if (!sides[s].bevel)
                    faults++;
                continue;
            }

            double area2 = 0.0;
            for (uint32_t p = 1; p + 1 < w->count; p++) {
                double e1[3], e2[3], cross[3];
                for (int a = 0; a < 3; a++) {
                    e1[a] = w->p[p][a] - w->p[0][a];
                    e2[a] = w->p[p + 1][a] - w->p[0][a];
                }
                cross[0] = e1[1] * e2[2] - e1[2] * e2[1];
                cross[1] = e1[2] * e2[0] - e1[0] * e2[2];
                cross[2] = e1[0] * e2[1] - e1[1] * e2[0];
                area2 += sqrt(cross[0] * cross[0] + cross[1] * cross[1]
                              + cross[2] * cross[2]);
            }
            if (area2 * 0.5 < FACE_MIN_AREA) {
                faults++;
                continue;
            }
            shaped++;

            if (num_faces >= g->num_sides)
                continue;
            flat_face_t *f = &faces[num_faces++];
            f->brush = b;
            f->count = w->count;
            f->first = used;
            memcpy(f->normal, sides[s].normal, sizeof(f->normal));
            f->dist = sides[s].dist;
            for (uint32_t p = 0; p < w->count; p++)
                for (int a = 0; a < 3; a++)
                    pool[(size_t)(used + p) * 3 + a] = w->p[p][a];
            used += w->count;
        }

        /* Planes that enclose nothing are not a brush at all. */
        if (!shaped && brush->num_sides)
            faults++;
    }

    /*
     * And the seams between them.
     *
     * Only faces of DIFFERENT brushes on the same plane can leave one: within
     * one convex solid every side meets its neighbours at a shared edge by
     * construction.
     */
    for (uint32_t i = 0; i < num_faces; i++) {
        for (uint32_t j = i + 1; j < num_faces; j++) {
            if (faces[i].brush == faces[j].brush || !same_plane(&faces[i],
                                                                &faces[j]))
                continue;
            bool split = false;
            for (int way = 0; way < 2 && !split; way++) {
                const flat_face_t *pt = way ? &faces[j] : &faces[i];
                const flat_face_t *ed = way ? &faces[i] : &faces[j];
                for (uint32_t p = 0; p < pt->count && !split; p++) {
                    const double *v = &pool[(size_t)(pt->first + p) * 3];
                    for (uint32_t e = 0; e < ed->count && !split; e++) {
                        const double *a = &pool[(size_t)(ed->first + e) * 3];
                        const double *b2 =
                            &pool[(size_t)(ed->first + (e + 1) % ed->count) * 3];
                        /* Both faces are on one plane facing one way, so where
                           the vertex splits the edge is where the crack is and
                           one look off it answers for both. */
                        split = splits_edge(v, a, b2)
                             && anyone_can_see(space, v, pt->normal);
                    }
                }
            }
            if (split)
                faults++;
        }
    }

    free(faces);
    free(pool);
    free(w);
    return faults;
}

const mapgen_geometry_face_t *MapGenGeometry_Face(const mapgen_geometry_t *g,
                                                  uint32_t i)
{
    return g && i < g->num_faces ? &g->faces[i] : NULL;
}

const float *MapGenGeometry_FacePoint(const mapgen_geometry_t *g, uint32_t i)
{
    return g && i < g->num_points ? &g->points[i * 3] : NULL;
}

const mapgen_geometry_brush_t *MapGenGeometry_Brush(const mapgen_geometry_t *g,
                                                    uint32_t i)
{
    return g && i < g->num_brushes ? &g->brushes[i] : NULL;
}

const mapgen_geometry_side_t *MapGenGeometry_Side(const mapgen_geometry_t *g,
                                                  uint32_t i)
{
    return g && i < g->num_sides ? &g->sides[i] : NULL;
}

const mapgen_geometry_entity_t *MapGenGeometry_Entity(const mapgen_geometry_t *g,
                                                      uint32_t i)
{
    return g && i < g->num_entities ? &g->entities[i] : NULL;
}

void MapGenGeometry_Pair(const mapgen_geometry_t *g, uint32_t pair,
                         const char **key, const char **value)
{
    if (key)
        *key = NULL;
    if (value)
        *value = NULL;
    if (!g || pair >= g->num_pairs)
        return;
    if (key)
        *key = g->text + g->pair_at[pair * 2];
    if (value)
        *value = g->text + g->pair_at[pair * 2 + 1];
}

const char *MapGenGeometry_EntityValue(const mapgen_geometry_t *g,
                                       uint32_t entity, const char *key)
{
    const mapgen_geometry_entity_t *e = MapGenGeometry_Entity(g, entity);
    if (!e || !key)
        return NULL;
    for (uint32_t i = 0; i < e->num_pairs; i++) {
        const char *k, *v;
        MapGenGeometry_Pair(g, e->first_pair + i, &k, &v);
        if (k && !strcmp(k, key))
            return v;
    }
    return NULL;
}

/* ---- the rigid transform --------------------------------------------------- */

static void rotate_xy(float v[3], uint32_t quarter_turns, bool mirror_x)
{
    for (uint32_t t = 0; t < quarter_turns; t++) {
        const float x = v[0], y = v[1];
        v[0] = -y;
        v[1] = x;
    }
    if (mirror_x)
        v[0] = -v[0];
}

/*
 * An angle in degrees, turned by the same quarter turns and mirror.
 *
 * Quake II writes a facing as one number, and two of its values are not
 * facings at all: -1 means up and -2 means down, and turning those is how a
 * door that rises becomes a door that opens sideways.
 */
static float turn_angle(float degrees, uint32_t quarter_turns, bool mirror_x)
{
    if (degrees == -1.0f || degrees == -2.0f)
        return degrees;
    float turned = degrees + 90.0f * (float)quarter_turns;
    if (mirror_x)
        turned = 180.0f - turned;
    while (turned < 0.0f)
        turned += 360.0f;
    while (turned >= 360.0f)
        turned -= 360.0f;
    return turned;
}

/* The keys that carry a facing, and how many numbers each of them holds. */
static void turn_entity_angles(mapgen_geometry_t *g, uint32_t entity,
                               uint32_t quarter_turns, bool mirror_x)
{
    const char *one = MapGenGeometry_EntityValue(g, entity, "angle");
    if (one && *one) {
        char text[32];
        snprintf(text, sizeof(text), "%g",
                 (double)turn_angle((float)atof(one), quarter_turns,
                                    mirror_x));
        MapGenGeometry_SetEntityValue(g, entity, "angle", text);
    }

    const char *three = MapGenGeometry_EntityValue(g, entity, "angles");
    if (three && *three) {
        float pitch = 0, yaw = 0, roll = 0;
        if (sscanf(three, "%f %f %f", &pitch, &yaw, &roll) == 3) {
            char text[96];
            snprintf(text, sizeof(text), "%g %g %g", (double)pitch,
                     (double)turn_angle(yaw, quarter_turns, mirror_x),
                     (double)roll);
            MapGenGeometry_SetEntityValue(g, entity, "angles", text);
        }
    }
}

mapgen_geometry_result_t MapGenGeometry_TransformSubset(
    mapgen_geometry_t *g, const uint8_t *brush_mask,
    const uint8_t *entity_mask, const float pivot[3],
    uint32_t quarter_turns, bool mirror_x, const float offset[3])
{
    if (!g || !offset || !pivot || quarter_turns > 3)
        return MAPGEN_GEOMETRY_ERR_ARGS;
    if (!finite3(offset) || !finite3(pivot))
        return MAPGEN_GEOMETRY_ERR_NON_FINITE;

    for (uint32_t b = 0; b < g->num_brushes; b++) {
        if (brush_mask && !brush_mask[b])
            continue;
        mapgen_geometry_brush_t *brush = &g->brushes[b];

        for (uint32_t s = 0; s < brush->num_sides; s++) {
            mapgen_geometry_side_t *side = &g->sides[brush->first_side + s];
            /*
             * A plane turned about a pivot: turn its normal, then put the
             * distance back where the pivot says it belongs. Doing it as
             * "translate to the origin, turn, translate back" on the distance
             * alone is the same arithmetic written twice, and it is the part
             * that is easy to get subtly wrong.
             */
            float on_plane[3];
            for (int a = 0; a < 3; a++)
                on_plane[a] = side->normal[a] * side->dist - pivot[a];
            rotate_xy(side->normal, quarter_turns, mirror_x);
            rotate_xy(on_plane, quarter_turns, mirror_x);
            side->dist = side->normal[0] * (on_plane[0] + pivot[0] + offset[0])
                       + side->normal[1] * (on_plane[1] + pivot[1] + offset[1])
                       + side->normal[2] * (on_plane[2] + pivot[2] + offset[2]);

            for (int a = 0; a < 2; a++) {
                float axis[3] = { side->axis[a][0], side->axis[a][1],
                                  side->axis[a][2] };
                rotate_xy(axis, quarter_turns, mirror_x);
                side->axis[a][0] = axis[0];
                side->axis[a][1] = axis[1];
                side->axis[a][2] = axis[2];
            }

            float centre[3];
            for (int a = 0; a < 3; a++)
                centre[a] = side->center[a] - pivot[a];
            rotate_xy(centre, quarter_turns, mirror_x);
            for (int a = 0; a < 3; a++)
                side->center[a] = centre[a] + pivot[a] + offset[a];

            for (uint8_t k = 0; k < side->num_samples; k++) {
                float sample[3];
                for (int a = 0; a < 3; a++)
                    sample[a] = side->sample[k][a] - pivot[a];
                rotate_xy(sample, quarter_turns, mirror_x);
                for (int a = 0; a < 3; a++)
                    side->sample[k][a] = sample[a] + pivot[a] + offset[a];
            }
            if (side->has_anchor)
                for (int k = 0; k < 3; k++) {
                    float anchor[3];
                    for (int a = 0; a < 3; a++)
                        anchor[a] = side->anchor[k][a] - pivot[a];
                    rotate_xy(anchor, quarter_turns, mirror_x);
                    for (int a = 0; a < 3; a++)
                        side->anchor[k][a] = anchor[a] + pivot[a] + offset[a];
                }
        }

        float lo[3], hi[3];
        for (int a = 0; a < 3; a++) {
            lo[a] = brush->mins[a] - pivot[a];
            hi[a] = brush->maxs[a] - pivot[a];
        }
        rotate_xy(lo, quarter_turns, mirror_x);
        rotate_xy(hi, quarter_turns, mirror_x);
        for (int a = 0; a < 3; a++) {
            const float x = lo[a] < hi[a] ? lo[a] : hi[a];
            const float y = lo[a] > hi[a] ? lo[a] : hi[a];
            brush->mins[a] = x + pivot[a] + offset[a];
            brush->maxs[a] = y + pivot[a] + offset[a];
        }
    }

    for (uint32_t e = 0; e < g->num_entities; e++) {
        if (entity_mask && !entity_mask[e])
            continue;
        if (g->entities[e].has_origin) {
            float origin[3];
            for (int a = 0; a < 3; a++)
                origin[a] = g->entities[e].origin[a] - pivot[a];
            rotate_xy(origin, quarter_turns, mirror_x);
            for (int a = 0; a < 3; a++)
                g->entities[e].origin[a] = origin[a] + pivot[a] + offset[a];
        }
        /* And what it is FACING, which the whole-map transform never carried:
           a rotated map whose doors keep their old angle is a map whose doors
           open into the wall. */
        turn_entity_angles(g, e, quarter_turns, mirror_x);
    }

    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_Transform(mapgen_geometry_t *g,
                                                  uint32_t quarter_turns,
                                                  bool mirror_x,
                                                  const float offset[3])
{
    if (!g || !offset || quarter_turns > 3)
        return MAPGEN_GEOMETRY_ERR_ARGS;
    if (!finite3(offset))
        return MAPGEN_GEOMETRY_ERR_NON_FINITE;

    for (uint32_t s = 0; s < g->num_sides; s++) {
        mapgen_geometry_side_t *side = &g->sides[s];
        rotate_xy(side->normal, quarter_turns, mirror_x);
        side->dist += side->normal[0] * offset[0] + side->normal[1] * offset[1]
                    + side->normal[2] * offset[2];

        /*
         * The texture goes with the surface. Rotate the axes and then pull the
         * offset back by what the translation added, or the map turns and the
         * texture on it does not.
         */
        for (int a = 0; a < 2; a++) {
            rotate_xy(side->axis[a], quarter_turns, mirror_x);
            side->axis[a][3] -= side->axis[a][0] * offset[0]
                              + side->axis[a][1] * offset[1]
                              + side->axis[a][2] * offset[2];
        }
    }

    for (uint32_t b = 0; b < g->num_brushes; b++) {
        mapgen_geometry_brush_t *brush = &g->brushes[b];
        float lo[3], hi[3];
        memcpy(lo, brush->mins, sizeof(lo));
        memcpy(hi, brush->maxs, sizeof(hi));
        rotate_xy(lo, quarter_turns, mirror_x);
        rotate_xy(hi, quarter_turns, mirror_x);
        for (int a = 0; a < 3; a++) {
            brush->mins[a] = (lo[a] < hi[a] ? lo[a] : hi[a]) + offset[a];
            brush->maxs[a] = (lo[a] > hi[a] ? lo[a] : hi[a]) + offset[a];
        }
    }

    for (uint32_t e = 0; e < g->num_entities; e++) {
        if (!g->entities[e].has_origin)
            continue;
        rotate_xy(g->entities[e].origin, quarter_turns, mirror_x);
        for (int a = 0; a < 3; a++)
            g->entities[e].origin[a] += offset[a];
    }

    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_AddBrush(mapgen_geometry_t *g,
                                                 const mapgen_geometry_side_t *sides,
                                                 uint32_t num_sides,
                                                 int32_t contents,
                                                 uint32_t model)
{
    if (!g || !sides || num_sides < 4
        || num_sides > MAPGEN_GEOMETRY_MAX_SIDES_PER_BRUSH)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    mapgen_geometry_side_t *shaped =
        malloc((size_t)num_sides * sizeof(*shaped));
    if (!shaped)
        return MAPGEN_GEOMETRY_ERR_MEMORY;
    memcpy(shaped, sides, (size_t)num_sides * sizeof(*shaped));

    float mins[3], maxs[3];
    if (!shape_brush(shaped, num_sides, mins, maxs)) {
        free(shaped);
        return MAPGEN_GEOMETRY_ERR_DEGENERATE;
    }

    if (g->num_brushes == g->brush_cap) {
        const uint32_t want = g->brush_cap ? g->brush_cap * 2 : 64;
        mapgen_geometry_brush_t *grown =
            realloc(g->brushes, (size_t)want * sizeof(*grown));
        if (!grown) {
            free(shaped);
            return MAPGEN_GEOMETRY_ERR_MEMORY;
        }
        g->brushes = grown;
        g->brush_cap = want;
    }
    if (g->num_sides + num_sides > g->side_cap) {
        uint32_t want = g->side_cap ? g->side_cap : 64;
        while (want < g->num_sides + num_sides)
            want *= 2;
        mapgen_geometry_side_t *grown =
            realloc(g->sides, (size_t)want * sizeof(*grown));
        if (!grown) {
            free(shaped);
            return MAPGEN_GEOMETRY_ERR_MEMORY;
        }
        g->sides = grown;
        g->side_cap = want;
    }

    mapgen_geometry_brush_t *brush = &g->brushes[g->num_brushes++];
    brush->first_side = g->num_sides;
    brush->num_sides = num_sides;
    brush->contents = contents;
    brush->model = model;
    memcpy(brush->mins, mins, sizeof(brush->mins));
    memcpy(brush->maxs, maxs, sizeof(brush->maxs));

    memcpy(&g->sides[g->num_sides], shaped,
           (size_t)num_sides * sizeof(*g->sides));
    g->num_sides += num_sides;

    free(shaped);
    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_AddEntity(mapgen_geometry_t *g,
                                                  uint32_t model,
                                                  uint32_t *out_entity)
{
    if (out_entity)
        *out_entity = 0;
    if (!g)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    if (g->num_entities == g->entity_cap) {
        const uint32_t want = g->entity_cap ? g->entity_cap * 2 : 64;
        if (want > MAPGEN_GEOMETRY_MAX_ENTITIES)
            return MAPGEN_GEOMETRY_ERR_LIMIT;
        mapgen_geometry_entity_t *grown =
            realloc(g->entities, (size_t)want * sizeof(*grown));
        if (!grown)
            return MAPGEN_GEOMETRY_ERR_MEMORY;
        g->entities = grown;
        g->entity_cap = want;
    }

    mapgen_geometry_entity_t *e = &g->entities[g->num_entities];
    memset(e, 0, sizeof(*e));
    e->first_pair = g->num_pairs;
    e->model = model;

    /*
     * And the count moves up with it.
     *
     * Without this, a caller asking for "the next model number" got the same
     * answer twice: the second lift on a map claimed the first one's model,
     * the writer put both platforms inside both entities, and neither of them
     * moved. One lift worked and two did not, which is why it looked like the
     * lift was broken rather than the numbering.
     */
    if (model >= g->num_models)
        g->num_models = model + 1;
    if (out_entity)
        *out_entity = g->num_entities;
    g->num_entities++;
    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_AddEntityPair(mapgen_geometry_t *g,
                                                      uint32_t entity,
                                                      const char *key,
                                                      const char *value)
{
    if (!g || entity >= g->num_entities || !key || !value)
        return MAPGEN_GEOMETRY_ERR_ARGS;
    /* Only the entity written last can still take pairs: they live in one
       shared block in order, and inserting into the middle would renumber
       every entity after it. */
    if (entity != g->num_entities - 1)
        return MAPGEN_GEOMETRY_ERR_ARGS;
    if (!add_pair(g, key, strlen(key), value, strlen(value)))
        return MAPGEN_GEOMETRY_ERR_LIMIT;
    g->entities[entity].num_pairs++;
    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_SetEntityValue(mapgen_geometry_t *g,
                                                       uint32_t entity,
                                                       const char *key,
                                                       const char *value)
{
    if (!g || entity >= g->num_entities || !key || !value)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    const mapgen_geometry_entity_t *e = &g->entities[entity];
    for (uint32_t i = 0; i < e->num_pairs; i++) {
        const uint32_t pair = e->first_pair + i;
        if (strcmp(g->text + g->pair_at[pair * 2], key))
            continue;

        /* The new bytes go on the end and the pair is repointed at them. The
           old ones stay where they are, unreferenced: the block is written out
           by walking the pairs, never by walking the bytes. */
        const size_t len = strlen(value);
        if (!reserve_text(g, len + 1))
            return MAPGEN_GEOMETRY_ERR_LIMIT;
        g->pair_at[pair * 2 + 1] = (uint32_t)g->text_len;
        memcpy(g->text + g->text_len, value, len);
        g->text_len += len;
        g->text[g->text_len++] = '\0';
        return MAPGEN_GEOMETRY_OK;
    }
    return MAPGEN_GEOMETRY_ERR_ARGS;
}

/* ---- the edit operators ---------------------------------------------------- */

/*
 * Move one plane along its own outward normal.
 *
 * A positive delta grows the solid. That direction is the safe one and it is
 * worth saying why: growing a brush only ever ADDS solid, so a map that was
 * sealed before is still sealed after - no displacement of this kind can open
 * a hole to the void. Shrinking can, by pulling a face back from the neighbour
 * it used to meet, so a caller that shrinks owes a compiled leak check.
 *
 * Convexity survives either way: a convex solid is the intersection of its
 * half-spaces, and sliding one half-space along its own normal leaves an
 * intersection of half-spaces.
 */
mapgen_geometry_result_t MapGenGeometry_DisplaceSide(mapgen_geometry_t *g,
                                                     uint32_t side, float delta)
{
    if (!g || side >= g->num_sides || !isfinite(delta))
        return MAPGEN_GEOMETRY_ERR_ARGS;
    g->sides[side].dist += delta;
    for (int a = 0; a < 3; a++)
        g->sides[side].center[a] += g->sides[side].normal[a] * delta;
    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_RetextureSide(mapgen_geometry_t *g,
                                                      uint32_t side,
                                                      const char *texture,
                                                      int32_t flags,
                                                      int32_t value)
{
    if (!g || side >= g->num_sides || !texture)
        return MAPGEN_GEOMETRY_ERR_ARGS;
    if (strlen(texture) > MAPGEN_BSP_TEXNAME)
        return MAPGEN_GEOMETRY_ERR_ARGS;
    memset(g->sides[side].texture, 0, sizeof(g->sides[side].texture));
    memcpy(g->sides[side].texture, texture, strlen(texture));
    g->sides[side].flags = flags;
    g->sides[side].value = value;
    return MAPGEN_GEOMETRY_OK;
}

mapgen_geometry_result_t MapGenGeometry_DropBrush(mapgen_geometry_t *g,
                                                  uint32_t brush)
{
    if (!g || brush >= g->num_brushes)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    const mapgen_geometry_brush_t *gone = &g->brushes[brush];
    const uint32_t first = gone->first_side;
    const uint32_t count = gone->num_sides;

    memmove(&g->sides[first], &g->sides[first + count],
            (size_t)(g->num_sides - first - count) * sizeof(*g->sides));
    g->num_sides -= count;

    memmove(&g->brushes[brush], &g->brushes[brush + 1],
            (size_t)(g->num_brushes - brush - 1) * sizeof(*g->brushes));
    g->num_brushes--;

    /* Every brush after it owned sides further along the array, and now does
       not. A brush whose first_side still points past the hole would be
       wearing another brush's faces. */
    for (uint32_t b = brush; b < g->num_brushes; b++)
        g->brushes[b].first_side -= count;

    return MAPGEN_GEOMETRY_OK;
}

/* ---- canonical text -------------------------------------------------------- */

/*
 * Integers only, so the answer cannot depend on a locale, a libc's rounding of
 * the seventeenth digit or a second implementation's choice of format. Values
 * are carried at ten-thousandths, which is finer than any coordinate a Quake
 * II compiler will keep.
 */
#define CANON_SCALE 10000.0

typedef struct {
    char  *out;
    size_t capacity;
    size_t needed;
} canon_t;

static void canon_raw(canon_t *c, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (c->needed + 1 < c->capacity && c->out)
            c->out[c->needed] = s[i];
        c->needed++;
    }
}

static void canon_str(canon_t *c, const char *s)
{
    canon_raw(c, s, strlen(s));
}

static void canon_i64(canon_t *c, int64_t v)
{
    char digits[24];
    size_t n = 0;
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (v < 0)
        canon_raw(c, "-", 1);
    do {
        digits[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    while (n--)
        canon_raw(c, &digits[n], 1);
}

static void canon_f(canon_t *c, float v)
{
    const double scaled = (double)v * CANON_SCALE;
    canon_i64(c, (int64_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5));
}

size_t MapGenGeometry_CanonicalText(const mapgen_geometry_t *g, char *out,
                                    size_t capacity)
{
    canon_t c = { out, capacity, 0 };
    if (!g) {
        if (out && capacity)
            out[0] = '\0';
        return 0;
    }

    canon_str(&c, "geometry 1\nb=");
    canon_i64(&c, g->num_brushes);
    canon_str(&c, " s=");
    canon_i64(&c, g->num_sides);
    canon_str(&c, " m=");
    canon_i64(&c, g->num_models);
    canon_str(&c, " e=");
    canon_i64(&c, g->num_entities);
    canon_str(&c, " orphan=");
    canon_i64(&c, g->orphan_brushes);
    canon_str(&c, "\n");

    for (uint32_t b = 0; b < g->num_brushes; b++) {
        const mapgen_geometry_brush_t *brush = &g->brushes[b];
        canon_str(&c, "B ");
        canon_i64(&c, brush->contents);
        canon_str(&c, " ");
        canon_i64(&c, brush->model);
        canon_str(&c, " ");
        canon_i64(&c, brush->num_sides);
        canon_str(&c, "\n");
        for (uint32_t s = 0; s < brush->num_sides; s++) {
            const mapgen_geometry_side_t *side = &g->sides[brush->first_side + s];
            canon_str(&c, "S ");
            for (int a = 0; a < 3; a++) {
                canon_f(&c, side->normal[a]);
                canon_str(&c, " ");
            }
            canon_f(&c, side->dist);
            canon_str(&c, " ");
            canon_i64(&c, side->flags);
            canon_str(&c, " ");
            canon_i64(&c, side->value);
            canon_str(&c, " ");
            canon_i64(&c, side->bevel ? 1 : 0);
            for (int a = 0; a < 2; a++) {
                for (int k = 0; k < 4; k++) {
                    canon_str(&c, " ");
                    canon_f(&c, side->axis[a][k]);
                }
            }
            canon_str(&c, " ");
            canon_str(&c, side->texture);
            canon_str(&c, "\n");
        }
    }

    for (uint32_t e = 0; e < g->num_entities; e++) {
        const mapgen_geometry_entity_t *ent = &g->entities[e];
        canon_str(&c, "E ");
        canon_i64(&c, ent->model);
        canon_str(&c, " ");
        canon_i64(&c, ent->num_pairs);
        canon_str(&c, "\n");
        for (uint32_t p = 0; p < ent->num_pairs; p++) {
            const char *k, *v;
            MapGenGeometry_Pair(g, ent->first_pair + p, &k, &v);
            canon_str(&c, "K ");
            canon_str(&c, k ? k : "");
            canon_str(&c, "=");
            canon_str(&c, v ? v : "");
            canon_str(&c, "\n");
        }
    }

    if (out && capacity)
        out[c.needed < capacity ? c.needed : capacity - 1] = '\0';
    return c.needed + 1;
}

/*
 * One row per distinct plane and material: area, extent and how the surface is
 * flagged. Sorted, so the order faces arrived in cannot change the answer.
 */
typedef struct {
    float    normal[3];
    float    dist;
    int32_t  flags;
    int32_t  value;
    uint32_t model;
    char     texture[MAPGEN_BSP_TEXNAME + 1];
    double   area;
    float    mins[3];
    float    maxs[3];
} surface_row_t;

/*
 * Rounded, not truncated, and one quantum coarser than the compiler's own
 * NORMAL_EPSILON.
 *
 * Truncating at a hundred-thousandth turns 0.2095000 and 0.2094999 into
 * different surfaces, and the difference is in the seventh decimal of a value
 * the compiler considers identical. Keyed that way the audit reported 288 of
 * the donor's surfaces vanished from a rebuild whose planes are all reproduced
 * to better than a millionth - the tool was measuring its own arithmetic.
 */
static int64_t quantize(float v, double quantum)
{
    const double scaled = (double)v * quantum;
    return (int64_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
}

#define SURFACE_NORMAL_QUANTUM 10000.0
#define SURFACE_DIST_QUANTUM   100.0

static int surface_order(const void *a, const void *b)
{
    const surface_row_t *x = a, *y = b;
    for (int i = 0; i < 3; i++) {
        const int64_t xn = quantize(x->normal[i], SURFACE_NORMAL_QUANTUM);
        const int64_t yn = quantize(y->normal[i], SURFACE_NORMAL_QUANTUM);
        if (xn != yn)
            return xn < yn ? -1 : 1;
    }
    const int64_t xd = quantize(x->dist, SURFACE_DIST_QUANTUM);
    const int64_t yd = quantize(y->dist, SURFACE_DIST_QUANTUM);
    if (xd != yd)
        return xd < yd ? -1 : 1;
    if (x->model != y->model)
        return x->model < y->model ? -1 : 1;
    return strcmp(x->texture, y->texture);
}

static bool same_surface(const surface_row_t *r, const mapgen_geometry_face_t *f)
{
    for (int i = 0; i < 3; i++) {
        if (quantize(r->normal[i], SURFACE_NORMAL_QUANTUM)
            != quantize(f->normal[i], SURFACE_NORMAL_QUANTUM))
            return false;
    }
    return quantize(r->dist, SURFACE_DIST_QUANTUM)
        == quantize(f->dist, SURFACE_DIST_QUANTUM)
        && r->model == f->model && r->flags == f->flags && r->value == f->value
        && !strcmp(r->texture, f->texture);
}

size_t MapGenGeometry_RenderCanonicalText(const mapgen_geometry_t *g,
                                          char *out, size_t capacity)
{
    canon_t c = { out, capacity, 0 };
    if (!g) {
        if (out && capacity)
            out[0] = '\0';
        return 0;
    }

    surface_row_t *rows = calloc(g->num_faces ? g->num_faces : 1, sizeof(*rows));
    if (!rows) {
        if (out && capacity)
            out[0] = '\0';
        return 0;
    }
    uint32_t num_rows = 0;

    for (uint32_t f = 0; f < g->num_faces; f++) {
        const mapgen_geometry_face_t *face = &g->faces[f];
        uint32_t at = num_rows;
        for (uint32_t r = 0; r < num_rows; r++) {
            if (same_surface(&rows[r], face)) {
                at = r;
                break;
            }
        }
        if (at == num_rows) {
            surface_row_t *row = &rows[num_rows++];
            memcpy(row->normal, face->normal, sizeof(row->normal));
            row->dist = face->dist;
            row->flags = face->flags;
            row->value = face->value;
            row->model = face->model;
            memcpy(row->texture, face->texture, sizeof(row->texture));
            for (int a = 0; a < 3; a++) {
                row->mins[a] = MAPGEN_GEOMETRY_WORLD_EXTENT;
                row->maxs[a] = -MAPGEN_GEOMETRY_WORLD_EXTENT;
            }
        }
        rows[at].area += face->area;
        for (uint32_t p = 0; p < face->num_points; p++) {
            const float *point = &g->points[(face->first_point + p) * 3];
            for (int a = 0; a < 3; a++) {
                if (point[a] < rows[at].mins[a])
                    rows[at].mins[a] = point[a];
                if (point[a] > rows[at].maxs[a])
                    rows[at].maxs[a] = point[a];
            }
        }
    }

    qsort(rows, num_rows, sizeof(*rows), surface_order);

    canon_str(&c, "render 1\nsurfaces=");
    canon_i64(&c, num_rows);
    canon_str(&c, "\n");
    for (uint32_t r = 0; r < num_rows; r++) {
        canon_str(&c, "R ");
        for (int a = 0; a < 3; a++) {
            canon_f(&c, rows[r].normal[a]);
            canon_str(&c, " ");
        }
        canon_f(&c, rows[r].dist);
        canon_str(&c, " ");
        canon_i64(&c, rows[r].flags);
        canon_str(&c, " ");
        canon_i64(&c, rows[r].value);
        canon_str(&c, " ");
        canon_i64(&c, rows[r].model);
        canon_str(&c, " ");
        /* Area to the nearest whole unit: the compiler's own arithmetic moves
           the last fraction, and a digest that noticed would be noise. */
        canon_i64(&c, (int64_t)(rows[r].area + 0.5));
        for (int a = 0; a < 3; a++) {
            canon_str(&c, " ");
            canon_f(&c, rows[r].mins[a]);
        }
        for (int a = 0; a < 3; a++) {
            canon_str(&c, " ");
            canon_f(&c, rows[r].maxs[a]);
        }
        canon_str(&c, " ");
        canon_str(&c, rows[r].texture);
        canon_str(&c, "\n");
    }

    free(rows);
    if (out && capacity)
        out[c.needed < capacity ? c.needed : capacity - 1] = '\0';
    return c.needed + 1;
}

uint64_t MapGenGeometry_RenderDigest(const mapgen_geometry_t *g)
{
    const size_t needed = MapGenGeometry_RenderCanonicalText(g, NULL, 0);
    char *text = malloc(needed ? needed : 1);
    if (!text)
        return 0;
    MapGenGeometry_RenderCanonicalText(g, text, needed);

    uint64_t hash = 14695981039346656037ull;
    for (const char *p = text; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 1099511628211ull;
    }
    free(text);
    return hash;
}

uint64_t MapGenGeometry_CanonicalDigest(const mapgen_geometry_t *g)
{
    const size_t needed = MapGenGeometry_CanonicalText(g, NULL, 0);
    char *text = malloc(needed ? needed : 1);
    if (!text)
        return 0;
    MapGenGeometry_CanonicalText(g, text, needed);

    uint64_t hash = 14695981039346656037ull;
    for (const char *p = text; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 1099511628211ull;
    }
    free(text);
    return hash;
}

/* ---- the Valve 220 writer ---------------------------------------------------- */

/* One variable of the seam diagnosis, overridable at build time so a
   controlled experiment changes exactly this and nothing else. */
/* The second variable of the seam diagnosis: whether the sides that clip
   nothing off the solid are handed to the compiler or left for it to derive
   again. */
#ifndef MAPGEN_REVERSE_BRUSH_ORDER
#define MAPGEN_REVERSE_BRUSH_ORDER 0
#endif

#ifndef MAPGEN_WRITE_BEVELS
/*
 * On, and measured rather than assumed.
 *
 * These sides clip nothing off their own brush, so dropping them looked free
 * and an aggregate count of loose edges moved from 407 to 401, which proved
 * nothing. Held against the faceted-arc fixture one variable at a time, the
 * worst plane the rebuild reproduces goes from being 0.0625 units out of place
 * to 0.000092 - seven hundred times closer - and the surfaces that come back
 * on the wrong plane drop from fourteen to six. A plane that clips nothing off
 * a convex body still tells the compiler where that body ends.
 */
#define MAPGEN_WRITE_BEVELS 1
#endif

#ifndef PLANE_POINT_DECIMALS
#define PLANE_POINT_DECIMALS 4
#endif

/*
 * Decimals rendered from integers, for the same reason the canonical text is:
 * `printf("%f")` answers to the C locale and would write a comma on a machine
 * set to one, and a `.map` with commas in its coordinates is not a map.
 */
static void write_f(FILE *f, double v, int decimals)
{
    double scale = 1.0;
    for (int i = 0; i < decimals; i++)
        scale *= 10.0;

    double scaled = v * scale;
    if (!isfinite(scaled))
        scaled = 0.0;
    int64_t fixed = (int64_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
    if (fixed < 0) {
        fputc('-', f);
        fixed = -fixed;
    }

    const int64_t whole = fixed / (int64_t)scale;
    int64_t frac = fixed % (int64_t)scale;

    char digits[24];
    size_t n = 0;
    int64_t u = whole;
    do {
        digits[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    while (n--)
        fputc(digits[n], f);

    /* Trailing zeroes carry no information and make a diff unreadable. */
    if (!frac)
        return;

    char tail[24];
    size_t t = 0;
    for (int i = 0; i < decimals; i++) {
        tail[t++] = (char)('0' + (frac % 10));
        frac /= 10;
    }
    while (t > 1 && tail[0] == '0') {
        memmove(tail, tail + 1, --t);
    }
    fputc('.', f);
    while (t--)
        fputc(tail[t], f);
}

/*
 * Three points on the plane, in the order the compiler derives a normal from:
 * it computes (p0 - p1) x (p2 - p1), so p1 is the corner and p0, p2 run along
 * two tangents whose cross product is the outward normal. Get the order wrong
 * and every solid in the map is inside out.
 */
#ifndef MAPGEN_PLANE_POINTS_FROM_WINDING
#define MAPGEN_PLANE_POINTS_FROM_WINDING 1
#endif

/* Three points on a plane, built from its normal and distance alone. */
static void plane_points_synthetic(const mapgen_geometry_side_t *side,
                                   double p[3][3]);

#ifndef MAPGEN_CANONICAL_PLANE_POINTS
#define MAPGEN_CANONICAL_PLANE_POINTS 1
#endif

/*
 * Is this side's normal the canonical orientation of its plane, or the
 * opposite one? First non-zero component positive, with the components tested
 * in a fixed order so that two sides sharing a plane always agree on which of
 * them is canonical.
 */
static bool plane_is_canonical(const float normal[3])
{
    for (int a = 0; a < 3; a++) {
        if (normal[a] > 0.0f)
            return true;
        if (normal[a] < 0.0f)
            return false;
    }
    return true;
}

static void plane_points(const mapgen_geometry_side_t *side, double p[3][3])
{
#if MAPGEN_CANONICAL_PLANE_POINTS
    /*
     * Derive for the canonical orientation and flip the order for the other
     * one. The winding anchors cannot be used here: they belong to this side's
     * own brush, and the neighbour's anchors are different points on the same
     * plane, which is the whole problem.
     */
    /*
     * ALWAYS, not only for the flipped side.
     *
     * Deriving the canonical side from its own winding corners and the opposite
     * side from the negated plane still gives the two of them different
     * coordinates, because the corners belong to one brush and the negation to
     * the other. Both have to come from the same arithmetic or they do not meet.
     */
    {
        mapgen_geometry_side_t canon = *side;
        canon.has_anchor = false;
        const bool flip = !plane_is_canonical(side->normal);
        if (flip) {
            for (int a = 0; a < 3; a++)
                canon.normal[a] = -side->normal[a];
            canon.dist = -side->dist;
        }

        double q[3][3];
        plane_points_synthetic(&canon, q);
        for (int i = 0; i < 3; i++) {
            const int from = flip ? 2 - i : i;
            for (int a = 0; a < 3; a++)
                p[i][a] = q[from][a];
        }
        return;
    }
#endif

#if MAPGEN_PLANE_POINTS_FROM_WINDING
    /*
     * The side's own corners, when it has them, in the order the compiler
     * derives a normal from: it computes (p0 - p1) x (p2 - p1), so the middle
     * point is the corner and the other two run along two edges. The winding
     * is wound so that this comes out along the outward normal already; if the
     * chosen pair happens to give the other sign, they are swapped.
     */
    if (side->has_anchor) {
        double t1[3], t2[3], n[3];
        for (int a = 0; a < 3; a++) {
            t1[a] = side->anchor[1][a] - side->anchor[0][a];
            t2[a] = side->anchor[2][a] - side->anchor[0][a];
        }
        n[0] = t1[1] * t2[2] - t1[2] * t2[1];
        n[1] = t1[2] * t2[0] - t1[0] * t2[2];
        n[2] = t1[0] * t2[1] - t1[1] * t2[0];
        const double facing = n[0] * side->normal[0] + n[1] * side->normal[1]
                            + n[2] * side->normal[2];
        const int first = facing >= 0.0 ? 1 : 2;
        const int last = facing >= 0.0 ? 2 : 1;
        for (int a = 0; a < 3; a++) {
            p[0][a] = side->anchor[first][a];
            p[1][a] = side->anchor[0][a];
            p[2][a] = side->anchor[last][a];
        }
        return;
    }
#endif

    plane_points_synthetic(side, p);
}

/*
 * Three points on the plane, in the order the compiler derives a normal from:
 * it computes (p0 - p1) x (p2 - p1), so p1 is the corner and p0, p2 run along
 * two tangents whose cross product is the outward normal.
 */
static void plane_points_synthetic(const mapgen_geometry_side_t *side,
                                   double p[3][3])
{
    int minor = 0;
    for (int i = 1; i < 3; i++) {
        if (fabsf(side->normal[i]) < fabsf(side->normal[minor]))
            minor = i;
    }
    float t[3] = { 0, 0, 0 };
    t[minor] = 1.0f;

    float u[3], v[3];
    const float d = t[0] * side->normal[0] + t[1] * side->normal[1]
                  + t[2] * side->normal[2];
    for (int i = 0; i < 3; i++)
        u[i] = t[i] - side->normal[i] * d;
    const float ulen = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; i++)
        u[i] /= ulen;
    v[0] = side->normal[1] * u[2] - side->normal[2] * u[1];
    v[1] = side->normal[2] * u[0] - side->normal[0] * u[2];
    v[2] = side->normal[0] * u[1] - side->normal[1] * u[0];

    /* Far enough apart that the compiler's own normal comes back accurate. */
    const double S = 512.0;
    for (int i = 0; i < 3; i++) {
        const double c = (double)side->normal[i] * side->dist;
        p[0][i] = c + u[i] * S;
        p[1][i] = c;
        p[2][i] = c + v[i] * S;
    }
}

mapgen_geometry_result_t MapGenGeometry_WriteValve220(const mapgen_geometry_t *g,
                                                      const char *path)
{
    if (!g || !path)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    FILE *f = fopen(path, "wb");
    if (!f)
        return MAPGEN_GEOMETRY_ERR_ARGS;

    fputs("// Game: Quake 2\n// Format: Valve\n"
          "// Generated by Q2PRO-X MAPGEN-1 (donor geometry)\n", f);

    for (uint32_t e = 0; e < g->num_entities; e++) {
        const mapgen_geometry_entity_t *ent = &g->entities[e];
        fputs("{\n", f);
        for (uint32_t p = 0; p < ent->num_pairs; p++) {
            const char *k, *v;
            MapGenGeometry_Pair(g, ent->first_pair + p, &k, &v);
            if (!k || !v)
                continue;
            /*
             * The compiler assigns its own `*n` as it writes the submodels,
             * so the donor's ordinal is dropped and the binding is expressed
             * by which brushes sit inside this entity's braces instead.
             */
            if (!strcmp(k, "model") && v[0] == '*')
                continue;
            if (!strcmp(k, "mapversion"))
                continue;          /* written once, below, and only as 220 */
            fputc('"', f);
            fputs(k, f);
            fputs("\" \"", f);
            fputs(v, f);
            fputs("\"\n", f);
        }

        /*
         * The dialect declaration, and it is not optional.
         *
         * The compiler decides how to read a face from this key alone: without
         * it the parser expects `shift shift rotate scale scale` and reads the
         * `[` of a Valve 220 texture axis as a number, then desynchronizes and
         * dies on the next brush. A donor BSP carries no mapversion of its
         * own, so it is emitted here as output metadata rather than inherited.
         */
        if (e == 0)
            fputs("\"mapversion\" \"220\"\n", f);

        /*
         * The brushes this entity owns, written inside it.
         *
         * Order is a variable of the seam diagnosis, not a detail: the
         * compiler resolves overlapping solids by CSG in the order it reads
         * them, so which of two brushes keeps the shared surface depends on
         * which came first. The donor's own .map order is gone - what survives
         * in the BSP is the order AFTER it was processed - so this is a
         * deliberate choice rather than a restoration.
         */
        for (uint32_t k = 0; k < g->num_brushes; k++) {
            const uint32_t b = MAPGEN_REVERSE_BRUSH_ORDER
                             ? g->num_brushes - 1 - k : k;
            const mapgen_geometry_brush_t *brush = &g->brushes[b];
            if (brush->model != ent->model)
                continue;
            if (ent->model == 0 && e != 0)
                continue;          /* only worldspawn carries the world */

            fputs("{\n", f);

            /*
             * Real faces first, then the sides with no winding of their own -
             * so that when one of those turns out to restate a face's plane,
             * it is the bevel that goes and never the wall.
             */
            uint32_t order[MAPGEN_GEOMETRY_MAX_SIDES_PER_BRUSH];
            uint32_t ordered = 0;
            if (brush->num_sides <= MAPGEN_GEOMETRY_MAX_SIDES_PER_BRUSH) {
                for (int pass = 0; pass < 2; pass++) {
                    for (uint32_t s = 0; s < brush->num_sides; s++) {
                        if (g->sides[brush->first_side + s].bevel != (pass == 1))
                            continue;
                        order[ordered++] = s;
                    }
                }
            }

            for (uint32_t k = 0; k < ordered; k++) {
                const uint32_t s = order[k];
                const mapgen_geometry_side_t *side =
                    &g->sides[brush->first_side + s];
#if !MAPGEN_WRITE_BEVELS
                if (side->bevel)
                    continue;
#endif

                /*
                 * A plane this brush has already stated.
                 *
                 * The compiler treats two planes as one within a hundredth of a
                 * unit, and it generates its own bevels for the collision hull
                 * that come back as near-duplicates of real faces - one arc
                 * segment carried its inner plane at d=-317.2622 and, three
                 * rows later, a bevel at -317.2637. Handing back both re-clips
                 * the brush by that hair, and the tighter copy takes the face
                 * off the map: that segment's whole inner wall vanished.
                 *
                 * So a brush states each of its planes once. Rounding that was
                 * allowed to become geometry is not geometry.
                 */
                bool duplicate = false;
                for (uint32_t p = 0; p < k && !duplicate; p++) {
                    const mapgen_geometry_side_t *earlier =
                        &g->sides[brush->first_side + order[p]];
                    const float dot = earlier->normal[0] * side->normal[0]
                                    + earlier->normal[1] * side->normal[1]
                                    + earlier->normal[2] * side->normal[2];
                    duplicate = dot > 0.9999f
                             && fabsf(earlier->dist - side->dist) < 0.05f;
                }
                if (duplicate)
                    continue;

                double pts[3][3];
                plane_points(side, pts);

                for (int k = 0; k < 3; k++) {
                    fputs("( ", f);
                    for (int a = 0; a < 3; a++) {
                        write_f(f, pts[k][a], PLANE_POINT_DECIMALS);
                        fputc(' ', f);
                    }
                    fputs(") ", f);
                }

                fputs(side->texture[0] ? side->texture : "e1u1/clip", f);

                /*
                 * Valve 220 keeps the axis and the scale apart, while the BSP
                 * folds the scale into the axis. Split them back out - a unit
                 * axis and the reciprocal of its old length - and the mapping
                 * survives the round trip exactly.
                 */
                for (int a = 0; a < 2; a++) {
                    const double len = sqrt((double)side->axis[a][0] * side->axis[a][0]
                                          + (double)side->axis[a][1] * side->axis[a][1]
                                          + (double)side->axis[a][2] * side->axis[a][2]);
                    fputs(" [ ", f);
                    for (int i = 0; i < 3; i++) {
                        write_f(f, len > 0 ? side->axis[a][i] / len : (i == a ? 1 : 0), 6);
                        fputc(' ', f);
                    }
                    write_f(f, side->axis[a][3], 4);
                    fputs(" ]", f);
                }
                fputs(" 0", f);
                for (int a = 0; a < 2; a++) {
                    const double len = sqrt((double)side->axis[a][0] * side->axis[a][0]
                                          + (double)side->axis[a][1] * side->axis[a][1]
                                          + (double)side->axis[a][2] * side->axis[a][2]);
                    fputc(' ', f);
                    write_f(f, len > 0 ? 1.0 / len : 1.0, 6);
                }

                /*
                 * Contents, flags and value, on every side.
                 *
                 * Without them the compiler re-derives what a surface IS from
                 * its texture name, and a donor's water, lava, sky, detail and
                 * light surfaces all become ordinary walls that happen to look
                 * wet. The donor already answered this question; the answer is
                 * carried, not asked again.
                 */
                fputc(' ', f);
                write_f(f, brush->contents, 0);
                fputc(' ', f);
                write_f(f, side->flags, 0);
                fputc(' ', f);
                write_f(f, side->value, 0);
                fputc('\n', f);
            }
            fputs("}\n", f);
        }
        fputs("}\n", f);
    }

    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok ? MAPGEN_GEOMETRY_OK : MAPGEN_GEOMETRY_ERR_ARGS;
}
