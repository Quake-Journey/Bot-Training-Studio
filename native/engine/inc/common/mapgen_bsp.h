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

MAPGEN-1 - BspDocument, the headless BSP reader

A strict, self-contained IBSP/QBSP v38 reader that the worker owns. It shares
no state with the engine loader and is not conditioned on `USE_REF`.

--- Why not just use bsp_t ------------------------------------------------

Because `bsp_t` cannot carry what the worker needs in the build the worker
runs in. In `inc/common/bsp.h` the faces, leaffaces, vertices, edges,
surfedges, lightmap and lightgrid all live inside `#if USE_REF`, and
`mtexinfo_t` keeps its texture AXES there too - while the material name sits
behind `USE_CLIENT`. A headless worker built without the renderer would lose
exactly the geometry and surface mapping that Training reads.

The Codex activation review of 2026-08-31 put it plainly: the `USE_REF=0` form
of `bsp_t` is not a geometry oracle, and a renderer-conditioned struct can be
neither the worker's persistence schema nor its IPC schema. So this document
parses the on-disk lumps into its own types, and its correctness is a result
to be proven rather than inherited.

Also: `BSP_Load` uses a global cache, a reference count, Zone/Hunk allocation
and intrusive lists, with no concurrent API and no synchronization contract.
Training parses many maps at once. This document owns ONE arena per load and
touches nothing global.

--- Strictness ------------------------------------------------------------

BSPs are UNTRUSTED input (contract section 2). Every count, offset, size and
index is validated before it is used, limits are imposed BEFORE allocation,
and the reader returns a stable error code rather than trusting anything the
file claims about itself.

==============================================================================
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAPGEN_BSP_IDENT_IBSP   0x50534249u  /* 'IBSP' */
#define MAPGEN_BSP_IDENT_QBSP   0x50534251u  /* 'QBSP' */
#define MAPGEN_BSP_VERSION      38
#define MAPGEN_BSP_LUMPS        19
#define MAPGEN_BSP_TEXNAME      32

/* Ceilings checked BEFORE allocation. They are the extended-format limits, so
   a legitimate QBSP map fits and a file claiming more is refused rather than
   believed. A hostile header is only ever an error, never an allocation. */
#define MAPGEN_BSP_MAX_FILE_BYTES   (512u * 1024u * 1024u)
#define MAPGEN_BSP_MAX_PLANES       1048576u
#define MAPGEN_BSP_MAX_NODES        1048576u
#define MAPGEN_BSP_MAX_LEAFS        1048576u
#define MAPGEN_BSP_MAX_LEAFBRUSHES  1048576u
#define MAPGEN_BSP_MAX_LEAFFACES    1048576u
#define MAPGEN_BSP_MAX_BRUSHES      1048576u
#define MAPGEN_BSP_MAX_BRUSHSIDES   4194304u
#define MAPGEN_BSP_MAX_TEXINFO      1048576u
#define MAPGEN_BSP_MAX_MODELS       131072u
#define MAPGEN_BSP_MAX_VERTICES     4194304u
#define MAPGEN_BSP_MAX_EDGES        4194304u
#define MAPGEN_BSP_MAX_SURFEDGES    8388608u
#define MAPGEN_BSP_MAX_FACES        1048576u
#define MAPGEN_BSP_MAX_ENTCHARS     (8u * 1024u * 1024u)
/* Never raised, never bypassed, including for QBSP output: this one is
   network-visible (contract section 18.1). */
#define MAPGEN_BSP_MAX_AREAS        256u
#define MAPGEN_BSP_MAX_AREAPORTALS  4096u

typedef enum {
    MAPGEN_BSP_OK = 0,

    MAPGEN_BSP_ERR_ARGUMENT,
    MAPGEN_BSP_ERR_TOO_SMALL,
    MAPGEN_BSP_ERR_TOO_LARGE,
    MAPGEN_BSP_ERR_BAD_IDENT,
    MAPGEN_BSP_ERR_BAD_VERSION,
    MAPGEN_BSP_ERR_LUMP_OUT_OF_BOUNDS,
    MAPGEN_BSP_ERR_LUMP_ODD_SIZE,
    MAPGEN_BSP_ERR_LIMIT_EXCEEDED,
    MAPGEN_BSP_ERR_BAD_INDEX,
    MAPGEN_BSP_ERR_NO_MODELS,
    MAPGEN_BSP_ERR_ENTSTRING,
    MAPGEN_BSP_ERR_OUT_OF_MEMORY,

    MAPGEN_BSP_RESULT_COUNT
} mapgen_bsp_result_t;

typedef struct { float normal[3]; float dist; int32_t type; } mapgen_bsp_plane_t;
typedef struct { uint32_t planenum; int32_t children[2]; int32_t mins[3]; int32_t maxs[3]; } mapgen_bsp_node_t;

typedef struct {
    int32_t  contents;
    int32_t  cluster;
    int32_t  area;
    int32_t  mins[3];
    int32_t  maxs[3];
    uint32_t firstleafface;
    uint32_t numleaffaces;
    uint32_t firstleafbrush;
    uint32_t numleafbrushes;
} mapgen_bsp_leaf_t;

typedef struct { int32_t firstside; int32_t numsides; int32_t contents; } mapgen_bsp_brush_t;
typedef struct { uint32_t planenum; int32_t texinfo; } mapgen_bsp_brushside_t;

typedef struct {
    /* The texture AXES the renderer-gated struct would have hidden. Training
       reads surface orientation and scale, so they are not optional here. */
    float   axis[2][4];
    int32_t flags;
    int32_t value;
    char    texture[MAPGEN_BSP_TEXNAME + 1];
    int32_t nexttexinfo;
} mapgen_bsp_texinfo_t;

typedef struct {
    float    mins[3];
    float    maxs[3];
    float    origin[3];
    int32_t  headnode;
    int32_t  firstface;
    int32_t  numfaces;
} mapgen_bsp_model_t;

typedef struct {
    uint32_t planenum;
    int32_t  side;
    int32_t  firstedge;
    int32_t  numedges;
    int32_t  texinfo;
    uint8_t  styles[4];
    int32_t  lightofs;
} mapgen_bsp_face_t;

typedef struct { float point[3]; } mapgen_bsp_vertex_t;
typedef struct { uint32_t v[2]; } mapgen_bsp_edge_t;

typedef struct mapgen_bsp_s mapgen_bsp_t;

/* Load from a memory image the caller owns. The document copies what it needs
   into its own arena and never references `data` afterwards. */
mapgen_bsp_result_t MapGenBsp_Load(const uint8_t *data, size_t size, mapgen_bsp_t **out);
void MapGenBsp_Free(mapgen_bsp_t *bsp);

bool     MapGenBsp_IsExtended(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_Count(const mapgen_bsp_t *bsp, int lump);

uint32_t MapGenBsp_NumPlanes(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumNodes(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumLeafs(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumLeafBrushes(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumLeafFaces(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumBrushes(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumBrushSides(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumTexInfo(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumModels(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumVertices(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumEdges(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumSurfEdges(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumFaces(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumAreas(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_NumAreaPortals(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_VisibilityBytes(const mapgen_bsp_t *bsp);
uint32_t MapGenBsp_LightingBytes(const mapgen_bsp_t *bsp);

const mapgen_bsp_plane_t     *MapGenBsp_Plane(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_node_t      *MapGenBsp_Node(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_leaf_t      *MapGenBsp_Leaf(const mapgen_bsp_t *bsp, uint32_t i);
uint32_t                      MapGenBsp_LeafBrush(const mapgen_bsp_t *bsp, uint32_t i);
uint32_t                      MapGenBsp_LeafFace(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_brush_t     *MapGenBsp_Brush(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_brushside_t *MapGenBsp_BrushSide(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_texinfo_t   *MapGenBsp_TexInfo(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_model_t     *MapGenBsp_Model(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_face_t      *MapGenBsp_Face(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_vertex_t    *MapGenBsp_Vertex(const mapgen_bsp_t *bsp, uint32_t i);
const mapgen_bsp_edge_t      *MapGenBsp_Edge(const mapgen_bsp_t *bsp, uint32_t i);
int32_t                       MapGenBsp_SurfEdge(const mapgen_bsp_t *bsp, uint32_t i);

/* The entity string, NUL-terminated and guaranteed free of embedded NULs. */
const char *MapGenBsp_Entities(const mapgen_bsp_t *bsp, uint32_t *out_length);

/*
 * Walk the collision tree of model 0 exactly as the engine does, and return
 * the leaf a point falls in. Read-only and reentrant: no global scratch, no
 * checkcount, so independent calls may run concurrently (contract 18.3).
 */
const mapgen_bsp_leaf_t *MapGenBsp_PointLeaf(const mapgen_bsp_t *bsp, const float point[3]);

/* Leaf contents OR the contents of any brush in that leaf containing the
   point. Detail brushes keep their contents on the brush, which is why the
   leaf alone is not the answer. */
int32_t MapGenBsp_PointContents(const mapgen_bsp_t *bsp, const float point[3]);

/* The same, but in ONE subtree: model zero is the map, and every door, lift
   and train has a headnode of its own. */
int32_t MapGenBsp_PointContentsAt(const mapgen_bsp_t *bsp, int32_t headnode,
                                  const float point[3]);

/*
 * A canonical text describing the document's MEANING, for cross-implementation
 * comparison. It excludes lightmap bytes, vis bytes, padding and lump offsets,
 * exactly like the digest in contract section 10.
 *
 * Writes at most `capacity` bytes including the terminator and returns the
 * number of bytes the full text WOULD need, so a caller can size a buffer.
 */
size_t MapGenBsp_CanonicalText(const mapgen_bsp_t *bsp, char *out, size_t capacity);

/* FNV-1a 64 over the canonical text. Two independent readers of the same file
   must produce the same value. */
uint64_t MapGenBsp_CanonicalDigest(const mapgen_bsp_t *bsp);

const char *MapGenBsp_ResultName(mapgen_bsp_result_t result);
