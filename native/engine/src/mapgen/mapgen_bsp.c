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
 * MAPGEN-1 - BspDocument.
 *
 * Strict, self-contained, one arena per load, no globals, no `USE_REF`.
 * Contract sections 6.2 and 2.
 *
 * Reading order is deliberate: validate the header, then every lump's bounds
 * and element size, then every COUNT against a ceiling - and only then
 * allocate. A file is not permitted to make this reader allocate anything on
 * the strength of a number it supplied.
 */

#include "common/mapgen_bsp.h"

#include <stdlib.h>
#include <string.h>

/* On-disk lump indices. */
enum {
    LUMP_ENTITIES = 0, LUMP_PLANES, LUMP_VERTEXES, LUMP_VISIBILITY,
    LUMP_NODES, LUMP_TEXINFO, LUMP_FACES, LUMP_LIGHTING,
    LUMP_LEAFS, LUMP_LEAFFACES, LUMP_LEAFBRUSHES, LUMP_EDGES,
    LUMP_SURFEDGES, LUMP_MODELS, LUMP_BRUSHES, LUMP_BRUSHSIDES,
    LUMP_POP, LUMP_AREAS, LUMP_AREAPORTALS
};

#define CONTENTS_SOLID  1

struct mapgen_bsp_s {
    bool     extended;
    size_t   file_bytes;

    uint32_t num_planes;      mapgen_bsp_plane_t     *planes;
    uint32_t num_nodes;       mapgen_bsp_node_t      *nodes;
    uint32_t num_leafs;       mapgen_bsp_leaf_t      *leafs;
    uint32_t num_leafbrushes; uint32_t               *leafbrushes;
    uint32_t num_leaffaces;   uint32_t               *leaffaces;
    uint32_t num_brushes;     mapgen_bsp_brush_t     *brushes;
    uint32_t num_brushsides;  mapgen_bsp_brushside_t *brushsides;
    uint32_t num_texinfo;     mapgen_bsp_texinfo_t   *texinfo;
    uint32_t num_models;      mapgen_bsp_model_t     *models;
    uint32_t num_vertices;    mapgen_bsp_vertex_t    *vertices;
    uint32_t num_edges;       mapgen_bsp_edge_t      *edges;
    uint32_t num_surfedges;   int32_t                *surfedges;
    uint32_t num_faces;       mapgen_bsp_face_t      *faces;

    uint32_t num_areas;
    uint32_t num_areaportals;
    uint32_t visibility_bytes;
    uint32_t lighting_bytes;

    uint32_t entity_length;   char *entities;

    /* One arena. Every array above points inside it, so a load either
       succeeds wholly or frees exactly one block. */
    uint8_t *arena;
    size_t   arena_size;
    size_t   arena_used;
};

/* ------------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static int16_t rd_i16(const uint8_t *p) { return (int16_t)rd_u16(p); }

static float rd_f32(const uint8_t *p)
{
    uint32_t bits = rd_u32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static void *arena_alloc(mapgen_bsp_t *bsp, size_t bytes)
{
    /* 8-byte alignment is enough for every type here and keeps the arithmetic
       obvious. */
    size_t aligned = (bsp->arena_used + 7u) & ~(size_t)7u;
    if (aligned + bytes < aligned || aligned + bytes > bsp->arena_size)
        return NULL;
    void *p = bsp->arena + aligned;
    bsp->arena_used = aligned + bytes;
    return p;
}

/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t offset;
    uint32_t length;
} lump_span_t;

mapgen_bsp_result_t MapGenBsp_Load(const uint8_t *data, size_t size, mapgen_bsp_t **out)
{
    if (!data || !out)
        return MAPGEN_BSP_ERR_ARGUMENT;
    *out = NULL;

    const size_t header_bytes = 8u + (size_t)MAPGEN_BSP_LUMPS * 8u;
    if (size < header_bytes)
        return MAPGEN_BSP_ERR_TOO_SMALL;
    if (size > MAPGEN_BSP_MAX_FILE_BYTES)
        return MAPGEN_BSP_ERR_TOO_LARGE;

    uint32_t ident = rd_u32(data);
    bool extended;
    if (ident == MAPGEN_BSP_IDENT_IBSP)
        extended = false;
    else if (ident == MAPGEN_BSP_IDENT_QBSP)
        extended = true;
    else
        return MAPGEN_BSP_ERR_BAD_IDENT;

    if (rd_u32(data + 4) != MAPGEN_BSP_VERSION)
        return MAPGEN_BSP_ERR_BAD_VERSION;

    lump_span_t lumps[MAPGEN_BSP_LUMPS];
    for (int i = 0; i < MAPGEN_BSP_LUMPS; i++) {
        uint32_t ofs = rd_u32(data + 8 + i * 8);
        uint32_t len = rd_u32(data + 12 + i * 8);
        /* 64-bit sum so a length near UINT32_MAX cannot wrap into range. */
        if ((uint64_t)ofs + (uint64_t)len > (uint64_t)size)
            return MAPGEN_BSP_ERR_LUMP_OUT_OF_BOUNDS;
        lumps[i].offset = ofs;
        lumps[i].length = len;
    }

    /* Record sizes differ between the standard and extended formats. */
    const uint32_t sz_plane      = 20;
    const uint32_t sz_node       = extended ? 44 : 28;
    const uint32_t sz_leaf       = extended ? 52 : 28;
    const uint32_t sz_leafbrush  = extended ? 4 : 2;
    const uint32_t sz_leafface   = extended ? 4 : 2;
    const uint32_t sz_brush      = 12;
    const uint32_t sz_brushside  = extended ? 8 : 4;
    const uint32_t sz_texinfo    = 76;
    const uint32_t sz_model      = 48;
    const uint32_t sz_vertex     = 12;
    const uint32_t sz_edge       = extended ? 8 : 4;
    const uint32_t sz_surfedge   = 4;
    const uint32_t sz_face       = extended ? 28 : 20;
    const uint32_t sz_area       = 8;
    const uint32_t sz_areaportal = 8;

    struct { int lump; uint32_t size; uint32_t limit; uint32_t *out; } table[] = {
        { LUMP_PLANES,      sz_plane,      MAPGEN_BSP_MAX_PLANES,      NULL },
        { LUMP_NODES,       sz_node,       MAPGEN_BSP_MAX_NODES,       NULL },
        { LUMP_LEAFS,       sz_leaf,       MAPGEN_BSP_MAX_LEAFS,       NULL },
        { LUMP_LEAFBRUSHES, sz_leafbrush,  MAPGEN_BSP_MAX_LEAFBRUSHES, NULL },
        { LUMP_LEAFFACES,   sz_leafface,   MAPGEN_BSP_MAX_LEAFFACES,   NULL },
        { LUMP_BRUSHES,     sz_brush,      MAPGEN_BSP_MAX_BRUSHES,     NULL },
        { LUMP_BRUSHSIDES,  sz_brushside,  MAPGEN_BSP_MAX_BRUSHSIDES,  NULL },
        { LUMP_TEXINFO,     sz_texinfo,    MAPGEN_BSP_MAX_TEXINFO,     NULL },
        { LUMP_MODELS,      sz_model,      MAPGEN_BSP_MAX_MODELS,      NULL },
        { LUMP_VERTEXES,    sz_vertex,     MAPGEN_BSP_MAX_VERTICES,    NULL },
        { LUMP_EDGES,       sz_edge,       MAPGEN_BSP_MAX_EDGES,       NULL },
        { LUMP_SURFEDGES,   sz_surfedge,   MAPGEN_BSP_MAX_SURFEDGES,   NULL },
        { LUMP_FACES,       sz_face,       MAPGEN_BSP_MAX_FACES,       NULL },
        { LUMP_AREAS,       sz_area,       MAPGEN_BSP_MAX_AREAS,       NULL },
        { LUMP_AREAPORTALS, sz_areaportal, MAPGEN_BSP_MAX_AREAPORTALS, NULL },
    };
    const size_t table_count = sizeof(table) / sizeof(table[0]);

    uint32_t counts[MAPGEN_BSP_LUMPS];
    memset(counts, 0, sizeof(counts));

    for (size_t i = 0; i < table_count; i++) {
        uint32_t len = lumps[table[i].lump].length;
        if (len % table[i].size)
            return MAPGEN_BSP_ERR_LUMP_ODD_SIZE;
        uint32_t count = len / table[i].size;
        if (count > table[i].limit)
            return MAPGEN_BSP_ERR_LIMIT_EXCEEDED;
        counts[table[i].lump] = count;
    }

    uint32_t ent_len = lumps[LUMP_ENTITIES].length;
    if (ent_len > MAPGEN_BSP_MAX_ENTCHARS)
        return MAPGEN_BSP_ERR_LIMIT_EXCEEDED;

    /* Size the arena from validated counts only. */
    size_t need = 0;
    need += (size_t)counts[LUMP_PLANES]      * sizeof(mapgen_bsp_plane_t)     + 8;
    need += (size_t)counts[LUMP_NODES]       * sizeof(mapgen_bsp_node_t)      + 8;
    need += (size_t)counts[LUMP_LEAFS]       * sizeof(mapgen_bsp_leaf_t)      + 8;
    need += (size_t)counts[LUMP_LEAFBRUSHES] * sizeof(uint32_t)               + 8;
    need += (size_t)counts[LUMP_LEAFFACES]   * sizeof(uint32_t)               + 8;
    need += (size_t)counts[LUMP_BRUSHES]     * sizeof(mapgen_bsp_brush_t)     + 8;
    need += (size_t)counts[LUMP_BRUSHSIDES]  * sizeof(mapgen_bsp_brushside_t) + 8;
    need += (size_t)counts[LUMP_TEXINFO]     * sizeof(mapgen_bsp_texinfo_t)   + 8;
    need += (size_t)counts[LUMP_MODELS]      * sizeof(mapgen_bsp_model_t)     + 8;
    need += (size_t)counts[LUMP_VERTEXES]    * sizeof(mapgen_bsp_vertex_t)    + 8;
    need += (size_t)counts[LUMP_EDGES]       * sizeof(mapgen_bsp_edge_t)      + 8;
    need += (size_t)counts[LUMP_SURFEDGES]   * sizeof(int32_t)                + 8;
    need += (size_t)counts[LUMP_FACES]       * sizeof(mapgen_bsp_face_t)      + 8;
    need += (size_t)ent_len + 1 + 8;

    mapgen_bsp_t *bsp = calloc(1, sizeof(*bsp));
    if (!bsp)
        return MAPGEN_BSP_ERR_OUT_OF_MEMORY;
    bsp->arena = calloc(1, need ? need : 1);
    if (!bsp->arena) {
        free(bsp);
        return MAPGEN_BSP_ERR_OUT_OF_MEMORY;
    }
    bsp->arena_size = need ? need : 1;
    bsp->extended = extended;
    bsp->file_bytes = size;

    bsp->num_planes      = counts[LUMP_PLANES];
    bsp->num_nodes       = counts[LUMP_NODES];
    bsp->num_leafs       = counts[LUMP_LEAFS];
    bsp->num_leafbrushes = counts[LUMP_LEAFBRUSHES];
    bsp->num_leaffaces   = counts[LUMP_LEAFFACES];
    bsp->num_brushes     = counts[LUMP_BRUSHES];
    bsp->num_brushsides  = counts[LUMP_BRUSHSIDES];
    bsp->num_texinfo     = counts[LUMP_TEXINFO];
    bsp->num_models      = counts[LUMP_MODELS];
    bsp->num_vertices    = counts[LUMP_VERTEXES];
    bsp->num_edges       = counts[LUMP_EDGES];
    bsp->num_surfedges   = counts[LUMP_SURFEDGES];
    bsp->num_faces       = counts[LUMP_FACES];
    bsp->num_areas       = counts[LUMP_AREAS];
    bsp->num_areaportals = counts[LUMP_AREAPORTALS];
    bsp->visibility_bytes = lumps[LUMP_VISIBILITY].length;
    bsp->lighting_bytes   = lumps[LUMP_LIGHTING].length;

#define ALLOC(field, type, count)                                  \
    do {                                                           \
        if (count) {                                               \
            bsp->field = arena_alloc(bsp, (size_t)(count) * sizeof(type)); \
            if (!bsp->field) { MapGenBsp_Free(bsp); return MAPGEN_BSP_ERR_OUT_OF_MEMORY; } \
        }                                                          \
    } while (0)

    ALLOC(planes,      mapgen_bsp_plane_t,     bsp->num_planes);
    ALLOC(nodes,       mapgen_bsp_node_t,      bsp->num_nodes);
    ALLOC(leafs,       mapgen_bsp_leaf_t,      bsp->num_leafs);
    ALLOC(leafbrushes, uint32_t,               bsp->num_leafbrushes);
    ALLOC(leaffaces,   uint32_t,               bsp->num_leaffaces);
    ALLOC(brushes,     mapgen_bsp_brush_t,     bsp->num_brushes);
    ALLOC(brushsides,  mapgen_bsp_brushside_t, bsp->num_brushsides);
    ALLOC(texinfo,     mapgen_bsp_texinfo_t,   bsp->num_texinfo);
    ALLOC(models,      mapgen_bsp_model_t,     bsp->num_models);
    ALLOC(vertices,    mapgen_bsp_vertex_t,    bsp->num_vertices);
    ALLOC(edges,       mapgen_bsp_edge_t,      bsp->num_edges);
    ALLOC(surfedges,   int32_t,                bsp->num_surfedges);
    ALLOC(faces,       mapgen_bsp_face_t,      bsp->num_faces);
#undef ALLOC

    bsp->entities = arena_alloc(bsp, (size_t)ent_len + 1);
    if (!bsp->entities) {
        MapGenBsp_Free(bsp);
        return MAPGEN_BSP_ERR_OUT_OF_MEMORY;
    }

    const uint8_t *p;

    p = data + lumps[LUMP_PLANES].offset;
    for (uint32_t i = 0; i < bsp->num_planes; i++, p += sz_plane) {
        bsp->planes[i].normal[0] = rd_f32(p);
        bsp->planes[i].normal[1] = rd_f32(p + 4);
        bsp->planes[i].normal[2] = rd_f32(p + 8);
        bsp->planes[i].dist = rd_f32(p + 12);
        bsp->planes[i].type = rd_i32(p + 16);
    }

    p = data + lumps[LUMP_NODES].offset;
    for (uint32_t i = 0; i < bsp->num_nodes; i++, p += sz_node) {
        bsp->nodes[i].planenum = rd_u32(p);
        bsp->nodes[i].children[0] = rd_i32(p + 4);
        bsp->nodes[i].children[1] = rd_i32(p + 8);
        for (int k = 0; k < 3; k++) {
            bsp->nodes[i].mins[k] = extended ? rd_i32(p + 12 + k * 4) : rd_i16(p + 12 + k * 2);
            bsp->nodes[i].maxs[k] = extended ? rd_i32(p + 24 + k * 4) : rd_i16(p + 18 + k * 2);
        }
    }

    p = data + lumps[LUMP_LEAFS].offset;
    for (uint32_t i = 0; i < bsp->num_leafs; i++, p += sz_leaf) {
        mapgen_bsp_leaf_t *lf = &bsp->leafs[i];
        lf->contents = rd_i32(p);
        if (extended) {
            lf->cluster = rd_i32(p + 4);
            lf->area = rd_i32(p + 8);
            for (int k = 0; k < 3; k++) {
                lf->mins[k] = rd_i32(p + 12 + k * 4);
                lf->maxs[k] = rd_i32(p + 24 + k * 4);
            }
            lf->firstleafface  = rd_u32(p + 36);
            lf->numleaffaces   = rd_u32(p + 40);
            lf->firstleafbrush = rd_u32(p + 44);
            lf->numleafbrushes = rd_u32(p + 48);
        } else {
            lf->cluster = rd_i16(p + 4);
            lf->area = rd_i16(p + 6);
            for (int k = 0; k < 3; k++) {
                lf->mins[k] = rd_i16(p + 8 + k * 2);
                lf->maxs[k] = rd_i16(p + 14 + k * 2);
            }
            lf->firstleafface  = rd_u16(p + 20);
            lf->numleaffaces   = rd_u16(p + 22);
            lf->firstleafbrush = rd_u16(p + 24);
            lf->numleafbrushes = rd_u16(p + 26);
        }
    }

    p = data + lumps[LUMP_LEAFBRUSHES].offset;
    for (uint32_t i = 0; i < bsp->num_leafbrushes; i++, p += sz_leafbrush)
        bsp->leafbrushes[i] = extended ? rd_u32(p) : rd_u16(p);

    p = data + lumps[LUMP_LEAFFACES].offset;
    for (uint32_t i = 0; i < bsp->num_leaffaces; i++, p += sz_leafface)
        bsp->leaffaces[i] = extended ? rd_u32(p) : rd_u16(p);

    p = data + lumps[LUMP_BRUSHES].offset;
    for (uint32_t i = 0; i < bsp->num_brushes; i++, p += sz_brush) {
        bsp->brushes[i].firstside = rd_i32(p);
        bsp->brushes[i].numsides = rd_i32(p + 4);
        bsp->brushes[i].contents = rd_i32(p + 8);
    }

    p = data + lumps[LUMP_BRUSHSIDES].offset;
    for (uint32_t i = 0; i < bsp->num_brushsides; i++, p += sz_brushside) {
        if (extended) {
            bsp->brushsides[i].planenum = rd_u32(p);
            bsp->brushsides[i].texinfo = rd_i32(p + 4);
        } else {
            bsp->brushsides[i].planenum = rd_u16(p);
            bsp->brushsides[i].texinfo = rd_i16(p + 2);
        }
    }

    p = data + lumps[LUMP_TEXINFO].offset;
    for (uint32_t i = 0; i < bsp->num_texinfo; i++, p += sz_texinfo) {
        mapgen_bsp_texinfo_t *ti = &bsp->texinfo[i];
        for (int a = 0; a < 2; a++)
            for (int c = 0; c < 4; c++)
                ti->axis[a][c] = rd_f32(p + (a * 4 + c) * 4);
        ti->flags = rd_i32(p + 32);
        ti->value = rd_i32(p + 36);
        /* A texture name is UNTRUSTED: it may fill the field with no
           terminator, so it is copied bounded and terminated here. */
        memcpy(ti->texture, p + 40, MAPGEN_BSP_TEXNAME);
        ti->texture[MAPGEN_BSP_TEXNAME] = '\0';
        for (int c = 0; c < MAPGEN_BSP_TEXNAME; c++) {
            if (ti->texture[c] == '\0')
                break;
            /* Control bytes never belong in a texture reference and would go
               on to reach a log, a UI and a compiler command line. */
            if ((unsigned char)ti->texture[c] < 0x20)
                ti->texture[c] = '?';
        }
        ti->nexttexinfo = rd_i32(p + 72);
    }

    p = data + lumps[LUMP_MODELS].offset;
    for (uint32_t i = 0; i < bsp->num_models; i++, p += sz_model) {
        for (int k = 0; k < 3; k++) {
            bsp->models[i].mins[k] = rd_f32(p + k * 4);
            bsp->models[i].maxs[k] = rd_f32(p + 12 + k * 4);
            bsp->models[i].origin[k] = rd_f32(p + 24 + k * 4);
        }
        bsp->models[i].headnode = rd_i32(p + 36);
        bsp->models[i].firstface = rd_i32(p + 40);
        bsp->models[i].numfaces = rd_i32(p + 44);
    }

    p = data + lumps[LUMP_VERTEXES].offset;
    for (uint32_t i = 0; i < bsp->num_vertices; i++, p += sz_vertex) {
        bsp->vertices[i].point[0] = rd_f32(p);
        bsp->vertices[i].point[1] = rd_f32(p + 4);
        bsp->vertices[i].point[2] = rd_f32(p + 8);
    }

    p = data + lumps[LUMP_EDGES].offset;
    for (uint32_t i = 0; i < bsp->num_edges; i++, p += sz_edge) {
        bsp->edges[i].v[0] = extended ? rd_u32(p) : rd_u16(p);
        bsp->edges[i].v[1] = extended ? rd_u32(p + 4) : rd_u16(p + 2);
    }

    p = data + lumps[LUMP_SURFEDGES].offset;
    for (uint32_t i = 0; i < bsp->num_surfedges; i++, p += sz_surfedge)
        bsp->surfedges[i] = rd_i32(p);

    p = data + lumps[LUMP_FACES].offset;
    for (uint32_t i = 0; i < bsp->num_faces; i++, p += sz_face) {
        mapgen_bsp_face_t *f = &bsp->faces[i];
        if (extended) {
            f->planenum = rd_u32(p);
            f->side = rd_i32(p + 4);
            f->firstedge = rd_i32(p + 8);
            f->numedges = rd_i32(p + 12);
            f->texinfo = rd_i32(p + 16);
            memcpy(f->styles, p + 20, 4);
            f->lightofs = rd_i32(p + 24);
        } else {
            f->planenum = rd_u16(p);
            f->side = rd_i16(p + 2);
            f->firstedge = rd_i32(p + 4);
            f->numedges = rd_i16(p + 8);
            f->texinfo = rd_i16(p + 10);
            memcpy(f->styles, p + 12, 4);
            f->lightofs = rd_i32(p + 16);
        }
    }

    /* The entity string is untrusted text. An embedded NUL would truncate it
       for one consumer and not another, so the document keeps exactly the
       bytes up to the first NUL and terminates them itself. */
    if (ent_len) {
        const uint8_t *ents = data + lumps[LUMP_ENTITIES].offset;
        uint32_t used = ent_len;
        for (uint32_t i = 0; i < ent_len; i++) {
            if (ents[i] == '\0') {
                used = i;
                break;
            }
        }
        memcpy(bsp->entities, ents, used);
        bsp->entities[used] = '\0';
        bsp->entity_length = used;
    } else {
        bsp->entities[0] = '\0';
        bsp->entity_length = 0;
    }

    /* Index validation, once, over everything - AFTER parsing so the checks
       run on decoded values rather than on re-derived offsets. A file that
       points a leaf at leafbrushes it does not have must be REJECTED here,
       not discovered later by a tree walk crashing. */
    if (!bsp->num_models) {
        MapGenBsp_Free(bsp);
        return MAPGEN_BSP_ERR_NO_MODELS;
    }
    for (uint32_t i = 0; i < bsp->num_leafs; i++) {
        const mapgen_bsp_leaf_t *lf = &bsp->leafs[i];
        if ((uint64_t)lf->firstleafbrush + lf->numleafbrushes > bsp->num_leafbrushes ||
            (uint64_t)lf->firstleafface + lf->numleaffaces > bsp->num_leaffaces) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_leafbrushes; i++) {
        if (bsp->leafbrushes[i] >= bsp->num_brushes) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_leaffaces; i++) {
        if (bsp->leaffaces[i] >= bsp->num_faces) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_brushes; i++) {
        const mapgen_bsp_brush_t *b = &bsp->brushes[i];
        if (b->firstside < 0 || b->numsides < 0 ||
            (uint64_t)b->firstside + (uint64_t)b->numsides > bsp->num_brushsides) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_brushsides; i++) {
        if (bsp->brushsides[i].planenum >= bsp->num_planes ||
            bsp->brushsides[i].texinfo >= (int32_t)bsp->num_texinfo) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_nodes; i++) {
        const mapgen_bsp_node_t *n = &bsp->nodes[i];
        if (n->planenum >= bsp->num_planes) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
        for (int c = 0; c < 2; c++) {
            int32_t child = n->children[c];
            if (child >= 0) {
                if ((uint32_t)child >= bsp->num_nodes) {
                    MapGenBsp_Free(bsp);
                    return MAPGEN_BSP_ERR_BAD_INDEX;
                }
            } else if ((uint32_t)(-1 - child) >= bsp->num_leafs) {
                MapGenBsp_Free(bsp);
                return MAPGEN_BSP_ERR_BAD_INDEX;
            }
        }
    }
    for (uint32_t i = 0; i < bsp->num_faces; i++) {
        const mapgen_bsp_face_t *f = &bsp->faces[i];
        if (f->planenum >= bsp->num_planes ||
            f->texinfo >= (int32_t)bsp->num_texinfo ||
            f->numedges < 0 || f->firstedge < 0 ||
            (uint64_t)f->firstedge + (uint64_t)f->numedges > bsp->num_surfedges) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_surfedges; i++) {
        int32_t e = bsp->surfedges[i];
        uint32_t idx = (uint32_t)(e < 0 ? -e : e);
        if (idx >= bsp->num_edges) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_edges; i++) {
        if (bsp->edges[i].v[0] >= bsp->num_vertices || bsp->edges[i].v[1] >= bsp->num_vertices) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }
    for (uint32_t i = 0; i < bsp->num_models; i++) {
        int32_t hn = bsp->models[i].headnode;
        /* A submodel whose whole volume is one leaf encodes its headnode as
           -(leaf + 1), exactly like a node child. Real shipped maps do this:
           q2rdm2 has four such submodels. Rejecting a negative headnode was
           the first version of this check, and it refused four playable maps
           from the PO's own Release tree - which is why parity runs against
           132 real maps and not only against synthesized ones. */
        if (hn >= 0) {
            if (bsp->num_nodes && (uint32_t)hn >= bsp->num_nodes) {
                MapGenBsp_Free(bsp);
                return MAPGEN_BSP_ERR_BAD_INDEX;
            }
        } else if ((uint32_t)(-1 - hn) >= bsp->num_leafs) {
            MapGenBsp_Free(bsp);
            return MAPGEN_BSP_ERR_BAD_INDEX;
        }
    }

    *out = bsp;
    return MAPGEN_BSP_OK;
}

void MapGenBsp_Free(mapgen_bsp_t *bsp)
{
    if (!bsp)
        return;
    free(bsp->arena);
    free(bsp);
}

/* ------------------------------------------------------------------------ */

bool MapGenBsp_IsExtended(const mapgen_bsp_t *b) { return b && b->extended; }

#define COUNT_FN(name, field) \
    uint32_t name(const mapgen_bsp_t *b) { return b ? b->field : 0; }

COUNT_FN(MapGenBsp_NumPlanes,      num_planes)
COUNT_FN(MapGenBsp_NumNodes,       num_nodes)
COUNT_FN(MapGenBsp_NumLeafs,       num_leafs)
COUNT_FN(MapGenBsp_NumLeafBrushes, num_leafbrushes)
COUNT_FN(MapGenBsp_NumLeafFaces,   num_leaffaces)
COUNT_FN(MapGenBsp_NumBrushes,     num_brushes)
COUNT_FN(MapGenBsp_NumBrushSides,  num_brushsides)
COUNT_FN(MapGenBsp_NumTexInfo,     num_texinfo)
COUNT_FN(MapGenBsp_NumModels,      num_models)
COUNT_FN(MapGenBsp_NumVertices,    num_vertices)
COUNT_FN(MapGenBsp_NumEdges,       num_edges)
COUNT_FN(MapGenBsp_NumSurfEdges,   num_surfedges)
COUNT_FN(MapGenBsp_NumFaces,       num_faces)
COUNT_FN(MapGenBsp_NumAreas,       num_areas)
COUNT_FN(MapGenBsp_NumAreaPortals, num_areaportals)
COUNT_FN(MapGenBsp_VisibilityBytes, visibility_bytes)
COUNT_FN(MapGenBsp_LightingBytes,   lighting_bytes)
#undef COUNT_FN

#define ITEM_FN(name, type, array, count) \
    const type *name(const mapgen_bsp_t *b, uint32_t i) \
    { return (b && i < b->count) ? &b->array[i] : NULL; }

ITEM_FN(MapGenBsp_Plane,     mapgen_bsp_plane_t,     planes,     num_planes)
ITEM_FN(MapGenBsp_Node,      mapgen_bsp_node_t,      nodes,      num_nodes)
ITEM_FN(MapGenBsp_Leaf,      mapgen_bsp_leaf_t,      leafs,      num_leafs)
ITEM_FN(MapGenBsp_Brush,     mapgen_bsp_brush_t,     brushes,    num_brushes)
ITEM_FN(MapGenBsp_BrushSide, mapgen_bsp_brushside_t, brushsides, num_brushsides)
ITEM_FN(MapGenBsp_TexInfo,   mapgen_bsp_texinfo_t,   texinfo,    num_texinfo)
ITEM_FN(MapGenBsp_Model,     mapgen_bsp_model_t,     models,     num_models)
ITEM_FN(MapGenBsp_Face,      mapgen_bsp_face_t,      faces,      num_faces)
ITEM_FN(MapGenBsp_Vertex,    mapgen_bsp_vertex_t,    vertices,   num_vertices)
ITEM_FN(MapGenBsp_Edge,      mapgen_bsp_edge_t,      edges,      num_edges)
#undef ITEM_FN

uint32_t MapGenBsp_LeafBrush(const mapgen_bsp_t *b, uint32_t i)
{
    return (b && i < b->num_leafbrushes) ? b->leafbrushes[i] : 0;
}

uint32_t MapGenBsp_LeafFace(const mapgen_bsp_t *b, uint32_t i)
{
    return (b && i < b->num_leaffaces) ? b->leaffaces[i] : 0;
}

int32_t MapGenBsp_SurfEdge(const mapgen_bsp_t *b, uint32_t i)
{
    return (b && i < b->num_surfedges) ? b->surfedges[i] : 0;
}

const char *MapGenBsp_Entities(const mapgen_bsp_t *b, uint32_t *out_length)
{
    if (out_length)
        *out_length = b ? b->entity_length : 0;
    return b ? b->entities : NULL;
}

/* ------------------------------------------------------------------------ */

static const mapgen_bsp_leaf_t *point_leaf_from(const mapgen_bsp_t *b,
                                                int32_t num,
                                                const float point[3]);

const mapgen_bsp_leaf_t *MapGenBsp_PointLeaf(const mapgen_bsp_t *b, const float point[3])
{
    if (!b || !point || !b->num_leafs)
        return NULL;
    if (!b->num_nodes)
        return &b->leafs[0];

    int32_t num = b->models[0].headnode;
    return point_leaf_from(b, num, point);
}

/* The same walk, from any subtree - a door and a lift each have their own. */
static const mapgen_bsp_leaf_t *point_leaf_from(const mapgen_bsp_t *b,
                                                int32_t num,
                                                const float point[3])
{
    /* A headnode may itself be a leaf reference; the walk below already
       handles that shape, so nothing special is needed here beyond not
       assuming it is a node. */
    /* Bounded by the node count: the tree was validated at load, but a walk
       that could not terminate would still be a hang rather than an error. */
    for (uint32_t guard = 0; num >= 0 && guard <= b->num_nodes; guard++) {
        const mapgen_bsp_node_t *n = &b->nodes[num];
        const mapgen_bsp_plane_t *pl = &b->planes[n->planenum];
        float d = point[0] * pl->normal[0] + point[1] * pl->normal[1] +
                  point[2] * pl->normal[2] - pl->dist;
        /* The engine's tie-break, not a plausible one: `BSP_PointLeaf`
           (src/common/bsp.c:1127-1136) does `children[d < 0]`, so a point
           exactly ON a node plane goes to the FRONT. 4.5% of the stances
           MAPGEN queries land exactly on a plane, and every one of them
           resolved to a different leaf under `d > 0`. */
        num = n->children[d < 0];
    }
    if (num >= 0)
        return NULL;
    uint32_t leaf = (uint32_t)(-1 - num);
    return leaf < b->num_leafs ? &b->leafs[leaf] : NULL;
}

int32_t MapGenBsp_PointContents(const mapgen_bsp_t *b, const float point[3])
{
    return b ? MapGenBsp_PointContentsAt(b, b->models[0].headnode, point) : 0;
}

int32_t MapGenBsp_PointContentsAt(const mapgen_bsp_t *b, int32_t headnode,
                                  const float point[3])
{
    if (!b || !point)
        return 0;
    const mapgen_bsp_leaf_t *lf = point_leaf_from(b, headnode, point);
    if (!lf)
        return 0;

    int32_t contents = lf->contents;
    for (uint32_t i = 0; i < lf->numleafbrushes; i++) {
        const mapgen_bsp_brush_t *br = &b->brushes[b->leafbrushes[lf->firstleafbrush + i]];
        bool inside = true;
        for (int32_t s = 0; s < br->numsides; s++) {
            const mapgen_bsp_brushside_t *side = &b->brushsides[br->firstside + s];
            const mapgen_bsp_plane_t *pl = &b->planes[side->planenum];
            float d = point[0] * pl->normal[0] + point[1] * pl->normal[1] +
                      point[2] * pl->normal[2] - pl->dist;
            if (d > 0) {
                inside = false;
                break;
            }
        }
        if (inside)
            contents |= br->contents;
    }
    return contents;
}

/* ------------------------------------------------------------------------ */

/* Canonical float text: fixed precision, no negative zero, locale-free.
   Written by hand rather than with printf("%f") so a locale that uses a comma
   for the decimal separator cannot change a digest (contract section 10). */
static size_t fmt_float(char *out, size_t cap, float value)
{
    double v = (double)value;
    bool negative = v < 0.0;
    if (negative)
        v = -v;

    /* Six decimals, matching the Python oracle. */
    double scaled = v * 1000000.0 + 0.5;
    if (!(scaled < 9.0e18))
        scaled = 0.0;                       /* NaN or absurd: canonicalize */
    uint64_t units = (uint64_t)scaled;
    uint64_t whole = units / 1000000u;
    uint64_t frac = units % 1000000u;

    if (whole == 0 && frac == 0)
        negative = false;                   /* kills -0.000000 */

    char buf[64];
    size_t n = 0;
    if (negative)
        buf[n++] = '-';

    char digits[24];
    size_t d = 0;
    if (whole == 0) {
        digits[d++] = '0';
    } else {
        while (whole && d < sizeof(digits)) {
            digits[d++] = (char)('0' + (whole % 10u));
            whole /= 10u;
        }
    }
    while (d)
        buf[n++] = digits[--d];

    buf[n++] = '.';
    for (int place = 100000; place >= 1; place /= 10) {
        buf[n++] = (char)('0' + (frac / (uint64_t)place) % 10u);
    }
    buf[n] = '\0';

    if (out && cap) {
        size_t copy = n < cap - 1 ? n : cap - 1;
        memcpy(out, buf, copy);
        out[copy] = '\0';
    }
    return n;
}

typedef struct {
    char  *out;
    size_t capacity;
    size_t needed;
} sink_t;

static void sink_str(sink_t *s, const char *text)
{
    size_t n = strlen(text);
    if (s->out && s->needed < s->capacity) {
        size_t room = s->capacity - 1 - s->needed;
        size_t copy = n < room ? n : room;
        memcpy(s->out + s->needed, text, copy);
    }
    s->needed += n;
}

static void sink_u64(sink_t *s, uint64_t v)
{
    char buf[24];
    size_t n = 0;
    if (!v) {
        buf[n++] = '0';
    } else {
        char tmp[24];
        size_t t = 0;
        while (v) {
            tmp[t++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (t)
            buf[n++] = tmp[--t];
    }
    buf[n] = '\0';
    sink_str(s, buf);
}

static void sink_i64(sink_t *s, int64_t v)
{
    if (v < 0) {
        sink_str(s, "-");
        sink_u64(s, (uint64_t)(-(v + 1)) + 1u);
    } else {
        sink_u64(s, (uint64_t)v);
    }
}

static void sink_f(sink_t *s, float v)
{
    char buf[64];
    fmt_float(buf, sizeof(buf), v);
    sink_str(s, buf);
}

size_t MapGenBsp_CanonicalText(const mapgen_bsp_t *b, char *out, size_t capacity)
{
    sink_t s = { out, capacity, 0 };
    if (!b) {
        if (out && capacity)
            out[0] = '\0';
        return 0;
    }

    sink_str(&s, "format="); sink_str(&s, b->extended ? "QBSP" : "IBSP"); sink_str(&s, "\n");
    sink_str(&s, "planes="); sink_u64(&s, b->num_planes); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_planes; i++) {
        const mapgen_bsp_plane_t *pl = &b->planes[i];
        sink_str(&s, "plane=");
        sink_f(&s, pl->normal[0]); sink_str(&s, ",");
        sink_f(&s, pl->normal[1]); sink_str(&s, ",");
        sink_f(&s, pl->normal[2]); sink_str(&s, ",");
        sink_f(&s, pl->dist); sink_str(&s, ",");
        sink_i64(&s, pl->type); sink_str(&s, "\n");
    }

    sink_str(&s, "brushes="); sink_u64(&s, b->num_brushes); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_brushes; i++) {
        sink_str(&s, "brush=");
        sink_i64(&s, b->brushes[i].firstside); sink_str(&s, ",");
        sink_i64(&s, b->brushes[i].numsides); sink_str(&s, ",");
        sink_i64(&s, b->brushes[i].contents); sink_str(&s, "\n");
    }

    sink_str(&s, "brushsides="); sink_u64(&s, b->num_brushsides); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_brushsides; i++) {
        sink_str(&s, "side=");
        sink_u64(&s, b->brushsides[i].planenum); sink_str(&s, ",");
        sink_i64(&s, b->brushsides[i].texinfo); sink_str(&s, "\n");
    }

    sink_str(&s, "texinfo="); sink_u64(&s, b->num_texinfo); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_texinfo; i++) {
        sink_str(&s, "tex=");
        sink_str(&s, b->texinfo[i].texture); sink_str(&s, ",");
        sink_i64(&s, b->texinfo[i].flags); sink_str(&s, ",");
        sink_i64(&s, b->texinfo[i].value); sink_str(&s, ",");
        sink_i64(&s, b->texinfo[i].nexttexinfo); sink_str(&s, "\n");
    }

    sink_str(&s, "nodes="); sink_u64(&s, b->num_nodes); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_nodes; i++) {
        sink_str(&s, "node=");
        sink_u64(&s, b->nodes[i].planenum); sink_str(&s, ",");
        sink_i64(&s, b->nodes[i].children[0]); sink_str(&s, ",");
        sink_i64(&s, b->nodes[i].children[1]); sink_str(&s, "\n");
    }

    sink_str(&s, "leafs="); sink_u64(&s, b->num_leafs); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_leafs; i++) {
        const mapgen_bsp_leaf_t *lf = &b->leafs[i];
        sink_str(&s, "leaf=");
        sink_i64(&s, lf->contents); sink_str(&s, ",");
        sink_i64(&s, lf->cluster); sink_str(&s, ",");
        sink_i64(&s, lf->area); sink_str(&s, ",");
        sink_u64(&s, lf->firstleafbrush); sink_str(&s, ",");
        sink_u64(&s, lf->numleafbrushes); sink_str(&s, "\n");
    }

    sink_str(&s, "leafbrushes="); sink_u64(&s, b->num_leafbrushes); sink_str(&s, "\n");
    sink_str(&s, "lb=");
    for (uint32_t i = 0; i < b->num_leafbrushes; i++) {
        if (i)
            sink_str(&s, ",");
        sink_u64(&s, b->leafbrushes[i]);
    }
    sink_str(&s, "\n");

    sink_str(&s, "models="); sink_u64(&s, b->num_models); sink_str(&s, "\n");
    for (uint32_t i = 0; i < b->num_models; i++) {
        const mapgen_bsp_model_t *m = &b->models[i];
        sink_str(&s, "model=");
        sink_f(&s, m->mins[0]); sink_str(&s, ",");
        sink_f(&s, m->mins[1]); sink_str(&s, ",");
        sink_f(&s, m->mins[2]); sink_str(&s, ",");
        sink_f(&s, m->maxs[0]); sink_str(&s, ",");
        sink_f(&s, m->maxs[1]); sink_str(&s, ",");
        sink_f(&s, m->maxs[2]); sink_str(&s, ",");
        sink_i64(&s, m->headnode); sink_str(&s, "\n");
    }

    sink_str(&s, "faces="); sink_u64(&s, b->num_faces); sink_str(&s, "\n");
    sink_str(&s, "entitychars="); sink_u64(&s, b->entity_length); sink_str(&s, "\n");

    if (out && capacity) {
        size_t end = s.needed < capacity - 1 ? s.needed : capacity - 1;
        out[end] = '\0';
    }
    return s.needed;
}

uint64_t MapGenBsp_CanonicalDigest(const mapgen_bsp_t *b)
{
    size_t needed = MapGenBsp_CanonicalText(b, NULL, 0);
    char *text = malloc(needed + 1);
    if (!text)
        return 0;
    MapGenBsp_CanonicalText(b, text, needed + 1);

    uint64_t hash = 1469598103934665603ull;      /* FNV-1a 64 offset basis */
    for (size_t i = 0; i < needed; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 1099511628211ull;
    }
    free(text);
    return hash;
}

const char *MapGenBsp_ResultName(mapgen_bsp_result_t r)
{
    switch (r) {
    case MAPGEN_BSP_OK:                     return "OK";
    case MAPGEN_BSP_ERR_ARGUMENT:           return "ARGUMENT";
    case MAPGEN_BSP_ERR_TOO_SMALL:          return "TOO_SMALL";
    case MAPGEN_BSP_ERR_TOO_LARGE:          return "TOO_LARGE";
    case MAPGEN_BSP_ERR_BAD_IDENT:          return "BAD_IDENT";
    case MAPGEN_BSP_ERR_BAD_VERSION:        return "BAD_VERSION";
    case MAPGEN_BSP_ERR_LUMP_OUT_OF_BOUNDS: return "LUMP_OUT_OF_BOUNDS";
    case MAPGEN_BSP_ERR_LUMP_ODD_SIZE:      return "LUMP_ODD_SIZE";
    case MAPGEN_BSP_ERR_LIMIT_EXCEEDED:     return "LIMIT_EXCEEDED";
    case MAPGEN_BSP_ERR_BAD_INDEX:          return "BAD_INDEX";
    case MAPGEN_BSP_ERR_NO_MODELS:          return "NO_MODELS";
    case MAPGEN_BSP_ERR_ENTSTRING:          return "ENTSTRING";
    case MAPGEN_BSP_ERR_OUT_OF_MEMORY:      return "OUT_OF_MEMORY";
    default:                                return "UNKNOWN";
    }
}
