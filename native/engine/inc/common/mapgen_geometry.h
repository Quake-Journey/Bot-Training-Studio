/*
 * MapGenGeometry - the canonical, editable equivalent of a map's compiled
 * geometry, and the carrier of architecture fidelity.
 *
 * This exists because the thing it replaces could not do the job. The previous
 * fidelity path learned a segmentation of the donor - where the rooms are, how
 * big, what connects to what - and rebuilt each room as a fresh axis-aligned
 * box. At fidelity 100 that reproduced q2dm1's volume graph exactly and looked
 * nothing like q2dm1: 904 of the donor's planes are oblique and its average
 * brush has seven sides, while every solid the old path emitted was a box with
 * six. No work downstream could recover geometry the learning stage never
 * read.
 *
 * So the geometry itself is what is carried. A donor's convex solids, their
 * per-side texture mapping with the real S/T axes, which submodel owns what,
 * and the entity records bound to those submodels - all of it survives
 * Training, and a candidate at fidelity 100 IS the donor's geometry rather
 * than a reconstruction of it.
 *
 * What this is NOT: a decompiler. A compiled BSP has lost the original CSG and
 * the editor's grouping, and nothing here pretends otherwise. What it recovers
 * is an EQUIVALENT set of convex half-spaces and surface constraints, which is
 * what a compiler needs and what the eye reads as architecture.
 *
 * Contract sections 3, 6.3 and 14.0.
 */

#ifndef MAPGEN_GEOMETRY_H
#define MAPGEN_GEOMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/mapgen_bsp.h"

typedef enum {
    MAPGEN_GEOMETRY_OK = 0,
    MAPGEN_GEOMETRY_ERR_ARGS,
    MAPGEN_GEOMETRY_ERR_MEMORY,
    MAPGEN_GEOMETRY_ERR_LIMIT,          /* a bound in this header was exceeded */
    MAPGEN_GEOMETRY_ERR_DEGENERATE,     /* a brush encloses no space */
    MAPGEN_GEOMETRY_ERR_NON_FINITE,     /* a plane or vertex is NaN/inf */
    MAPGEN_GEOMETRY_ERR_OWNERSHIP,      /* two models claim one brush */
    MAPGEN_GEOMETRY_ERR_UNSUPPORTED     /* DONOR_GEOMETRY_UNSUPPORTED */
} mapgen_geometry_result_t;

const char *MapGenGeometry_ResultName(mapgen_geometry_result_t r);

/*
 * Bounds, checked BEFORE any allocation. A donor is data from outside this
 * program and is sized before it is trusted.
 */
#define MAPGEN_GEOMETRY_MAX_BRUSHES     262144u
#define MAPGEN_GEOMETRY_MAX_SIDES       1048576u
#define MAPGEN_GEOMETRY_MAX_MODELS      8192u
#define MAPGEN_GEOMETRY_MAX_ENTITIES    32768u
#define MAPGEN_GEOMETRY_MAX_KEYVALUES   (2u * 1024u * 1024u)
#define MAPGEN_GEOMETRY_MAX_SIDES_PER_BRUSH 256u

#define MAPGEN_GEOMETRY_FACE_SAMPLES 9

/* A winding this far out is treated as the compiler's own unbounded scratch,
   not as architecture. q2dm1 fits inside 4096 in every axis. */
#define MAPGEN_GEOMETRY_WORLD_EXTENT    65536.0f

/*
 * One side of one convex solid.
 *
 * `normal`/`dist` is the outward half-space. `axis` is the texinfo mapping
 * exactly as the donor recorded it - vecs[0] and vecs[1] with their offsets in
 * [3] - because a re-derived "dominant axis" mapping is a different map with
 * the same brushes.
 */
typedef struct {
    float    normal[3];
    float    dist;
    float    axis[2][4];
    int32_t  flags;
    int32_t  value;
    char     texture[MAPGEN_BSP_TEXNAME + 1];
    bool     bevel;             /* no winding of its own; retained, not dropped */

    /* Where the face actually is and how much of it there is. Derived once
       from the winding, because an edit that asks "is there room beyond this
       surface" needs a point to ask from, and a coverage axis weighted by
       visible area needs the area. */
    float    center[3];
    float    area;

    /*
     * A handful of points spread across the face, always strictly inside it.
     *
     * The centre alone is not enough to ask "is there room beyond this
     * surface": a long wall whose middle faces an open hall can have an end
     * that borders a doorway, and a grow proven safe at the centre closes the
     * doorway. Each sample is halfway from the centroid to one of the face's
     * own corners, so the set covers the face and never leaves it.
     */
    float    sample[MAPGEN_GEOMETRY_FACE_SAMPLES][3];
    uint8_t  num_samples;

    /*
     * Three points of the side's own winding, spread as widely as the face
     * allows, kept so the writer can state the plane the way the donor's
     * geometry states it instead of reconstructing it from a normal and a
     * distance. Absent - `has_anchor` false - on a side with no winding.
     */
    float    anchor[3][3];
    bool     has_anchor;
} mapgen_geometry_side_t;

typedef struct {
    uint32_t first_side;
    uint32_t num_sides;
    int32_t  contents;
    uint32_t model;             /* 0 world, n the n-th brush model */
    float    mins[3];
    float    maxs[3];
} mapgen_geometry_brush_t;

/*
 * One entity, with its key/values kept verbatim in canonical order.
 *
 * `model` is the brush model it owns, or 0 for a point entity - resolved from
 * its own "*n" value, never from proximity. The compiler assigns new ordinals
 * on the way out, so identity here is the binding, not the number.
 */
typedef struct {
    uint32_t first_pair;
    uint32_t num_pairs;
    uint32_t model;
    bool     has_origin;
    float    origin[3];
} mapgen_geometry_entity_t;

/*
 * One visible surface as the compiler actually emitted it, with the winding it
 * is drawn from.
 *
 * Kept separately from the brush sides, and not derived from them, because the
 * two answer different questions. A side is a half-space that stops a player; a
 * face is a polygon he looks at. A rebuild can reproduce every half-space to a
 * ten-thousandth of a unit and still leave a seam in the mesh, and only this
 * table can see that.
 */
typedef struct {
    uint32_t first_point;
    uint32_t num_points;
    float    normal[3];         /* already flipped by the face's own side bit */
    float    dist;
    int32_t  flags;
    int32_t  value;
    uint32_t model;             /* 0 world, n the n-th brush model           */
    float    area;
    char     texture[MAPGEN_BSP_TEXNAME + 1];
} mapgen_geometry_face_t;

#define MAPGEN_GEOMETRY_MAX_FACES   1048576u
#define MAPGEN_GEOMETRY_MAX_POINTS  8388608u

typedef struct mapgen_geometry_s mapgen_geometry_t;

/*
 * Extract a donor's canonical geometry from its compiled BSP.
 *
 * Every brush's winding is derived by clipping against all of its own planes;
 * a brush that encloses nothing is a failure, not a brush that is skipped.
 * Ownership comes from each model's own collision subtree, so a door's solids
 * belong to the door because the door's tree reaches them - never because they
 * are near it.
 */
mapgen_geometry_result_t MapGenGeometry_FromBsp(const mapgen_bsp_t *bsp,
                                                mapgen_geometry_t **out);

/* A candidate begins life as its donor's geometry. */
mapgen_geometry_result_t MapGenGeometry_Clone(const mapgen_geometry_t *src,
                                              mapgen_geometry_t **out);

void MapGenGeometry_Free(mapgen_geometry_t *g);

uint32_t MapGenGeometry_NumBrushes(const mapgen_geometry_t *g);
uint32_t MapGenGeometry_NumSides(const mapgen_geometry_t *g);
uint32_t MapGenGeometry_NumModels(const mapgen_geometry_t *g);
uint32_t MapGenGeometry_NumEntities(const mapgen_geometry_t *g);
uint32_t MapGenGeometry_NumFaces(const mapgen_geometry_t *g);

/*
 * How many surfaces in here a compiler cannot build cleanly.
 *
 * Three things are counted, and they are all one question - is what the
 * brushes describe still a surface somebody can look at:
 *
 *   a brush whose planes enclose nothing at all;
 *   a side that was meant to be a face and has no area left;
 *   a T-junction, where a vertex of one face lands in the middle of a
 *   coplanar neighbour's edge and leaves a crack that sparkles.
 *
 * `space` is the compiled map the geometry stands in, and it decides which
 * seams count: a seam between two faces that meet in the void, on the outside
 * of the shell, is one no player can stand in front of. Passing NULL counts
 * every one of them, which is only useful to something asking about the
 * brushes rather than about the map.
 *
 * It is a count and not a verdict on purpose. A donor is full of them - q2dm1's
 * brushes come out of a compiled tree that split them - so the contract an edit
 * is held to is that it does not ADD one, which is a comparison the caller
 * makes between what it started with and what it produced.
 */
uint32_t MapGenGeometry_SurfaceFaults(const mapgen_geometry_t *g,
                                     const mapgen_bsp_t *space);

const mapgen_geometry_face_t *MapGenGeometry_Face(const mapgen_geometry_t *g,
                                                  uint32_t i);
/* The i-th point of the whole winding table; a face names its own range. */
const float *MapGenGeometry_FacePoint(const mapgen_geometry_t *g, uint32_t i);

/*
 * The render truth, canonicalised so that subdivision is not a difference.
 *
 * Per PLANE rather than per face: a compiler that splits one surface into three
 * equivalent pieces must read identically to one that does not, and a per-face
 * digest would reject exactly the subdivision contract 18.0 says to allow. What
 * survives the aggregation is what a viewer can actually tell apart - which
 * material, with which flags, covering how much area, over what extent.
 */
size_t   MapGenGeometry_RenderCanonicalText(const mapgen_geometry_t *g,
                                            char *out, size_t capacity);
uint64_t MapGenGeometry_RenderDigest(const mapgen_geometry_t *g);

const mapgen_geometry_brush_t  *MapGenGeometry_Brush(const mapgen_geometry_t *g,
                                                     uint32_t i);
const mapgen_geometry_side_t   *MapGenGeometry_Side(const mapgen_geometry_t *g,
                                                    uint32_t i);
const mapgen_geometry_entity_t *MapGenGeometry_Entity(const mapgen_geometry_t *g,
                                                      uint32_t i);

/* The i-th key/value of an entity, as two NUL-terminated strings. */
void MapGenGeometry_Pair(const mapgen_geometry_t *g, uint32_t pair,
                         const char **key, const char **value);

/* The value for a key on one entity, or NULL. */
const char *MapGenGeometry_EntityValue(const mapgen_geometry_t *g,
                                       uint32_t entity, const char *key);

/*
 * The one transform fidelity 100 permits: a rigid D4 move applied COHERENTLY
 * to solids, texture axes, entity origins and angles, and mover vectors.
 *
 * `quarter_turns` rotates about Z, `mirror_x` reflects across X afterwards,
 * `offset` translates. Applied to anything less than all of those at once it
 * would be a structural edit wearing a transform's name, so this is the only
 * entry point and it moves everything.
 */
mapgen_geometry_result_t MapGenGeometry_Transform(mapgen_geometry_t *g,
                                                  uint32_t quarter_turns,
                                                  bool mirror_x,
                                                  const float offset[3]);

/*
 * The same rigid move, about a PIVOT, over a named part of the map.
 *
 * What a bundle transform needs and what the whole-map one cannot give: the
 * brushes and entities named by the two masks turn about `pivot` and then
 * shift by `offset`; everything else stays exactly where it is.
 *
 * `brush_mask` has one byte per brush and `entity_mask` one per entity; either
 * may be NULL to mean all of them. A brush that moves takes its own sides with
 * it - a mask that named a side and not its brush would make a solid that is
 * not convex - so the unit of movement is the brush.
 *
 * Angles move too. The whole-map transform carries entity origins and NOT
 * their angles, which turns a map and leaves every door opening the way it
 * used to; here `angle`, `angles` and a mover's `movedir` are turned with the
 * geometry, because half a rotation is a broken map rather than a partial one.
 */
mapgen_geometry_result_t MapGenGeometry_TransformSubset(
    mapgen_geometry_t *g, const uint8_t *brush_mask,
    const uint8_t *entity_mask, const float pivot[3],
    uint32_t quarter_turns, bool mirror_x, const float offset[3]);

/*
 * The three ways a candidate may differ from its donor.
 *
 * Each is deliberately small and total: displacing a plane along its own
 * normal cannot make a convex solid non-convex, retexturing cannot move
 * anything, and dropping a brush is refused unless the caller has already
 * established that the brush seals nothing. Anything an edit operator wants
 * beyond these has to be argued for, not reached for.
 */
mapgen_geometry_result_t MapGenGeometry_DisplaceSide(mapgen_geometry_t *g,
                                                     uint32_t side, float delta);
mapgen_geometry_result_t MapGenGeometry_RetextureSide(mapgen_geometry_t *g,
                                                      uint32_t side,
                                                      const char *texture,
                                                      int32_t flags,
                                                      int32_t value);
mapgen_geometry_result_t MapGenGeometry_DropBrush(mapgen_geometry_t *g,
                                                  uint32_t brush);

/*
 * Add one convex solid, built from planes the caller has already decided on.
 *
 * The bounds and each face's centroid, area and samples are derived here
 * rather than supplied, because a caller that could state them would be a
 * caller that had already done the clipping - and then two places would know
 * how a brush gets its shape.
 *
 * Refused if the planes enclose nothing: a ramp that came out as an empty
 * intersection must not reach the compiler as a brush with no faces.
 */
mapgen_geometry_result_t MapGenGeometry_AddBrush(mapgen_geometry_t *g,
                                                 const mapgen_geometry_side_t *sides,
                                                 uint32_t num_sides,
                                                 int32_t contents,
                                                 uint32_t model);

/* Add one entity with no key/values yet, and then its pairs. Used where an
   edit introduces a construction the donor did not have. */
mapgen_geometry_result_t MapGenGeometry_AddEntity(mapgen_geometry_t *g,
                                                  uint32_t model,
                                                  uint32_t *out_entity);
mapgen_geometry_result_t MapGenGeometry_AddEntityPair(mapgen_geometry_t *g,
                                                      uint32_t entity,
                                                      const char *key,
                                                      const char *value);

/*
 * Give an existing key a new value.
 *
 * Replaces only - a key the entity does not already carry is refused. The
 * key/values of every entity live in one block in order, and inserting into
 * the middle of it would renumber every entity after this one; a caller that
 * needs a key an entity lacks has to say so rather than have it appear.
 */
mapgen_geometry_result_t MapGenGeometry_SetEntityValue(mapgen_geometry_t *g,
                                                       uint32_t entity,
                                                       const char *key,
                                                       const char *value);

/*
 * The canonical text and its digest: what this geometry MEANS, independent of
 * the order a reader happened to walk the lumps in and of any float
 * formatting a second implementation might choose. Two independent extractors
 * of one donor agree here or one of them is wrong.
 */
size_t   MapGenGeometry_CanonicalText(const mapgen_geometry_t *g, char *out,
                                      size_t capacity);
uint64_t MapGenGeometry_CanonicalDigest(const mapgen_geometry_t *g);

/*
 * Serialize as a Valve 220 `.map` the pinned compiler accepts.
 *
 * Arbitrary planes and real texture axes, written as locale-independent finite
 * decimals: the integer-only axis writer that serves the fidelity-0 backend
 * cannot express an oblique plane, and quantizing one into it is a fidelity
 * failure rather than a rounding choice.
 */
mapgen_geometry_result_t MapGenGeometry_WriteValve220(const mapgen_geometry_t *g,
                                                      const char *path);

#endif /* MAPGEN_GEOMETRY_H */
