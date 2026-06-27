/* mesh_data.h — the late-bound art, host-baked. The asset pass swaps the
 * placeholder cube for a small set of low-poly meshes WITHOUT touching the
 * projection, the depth, the shading, or the sim. The meshes are pure data: built
 * on the host by tools/gen_meshes.c and committed as src/mesh_data.c (the same
 * bake-it-once discipline as the escape table), then exposed through wasm so the
 * page uploads them ONCE at startup and instances them every frame.
 *
 * A mesh is a NON-INDEXED triangle list — flat per-face shading needs a per-face
 * normal, so faces don't share vertices. Each vertex is 12 bytes:
 *   int8  px,py,pz,_   position in a unit box [-1,1] (snorm8x4 on the GPU)
 *   int8  nx,ny,nz,_   face normal           (snorm8x4)
 *   uint8 r,g,b,a      per-vertex colour     (unorm8x4)
 * The shader computes meshColour * instanceTint: the PROCEDURAL meshes are white, so
 * the instance tint shows through (chapter-3 behaviour); the baked KENNEY map meshes
 * carry their real palette colour with a white tint — except the nest, a neutral mesh
 * the 63 per-nest tints colour. The instance's half-extents scale the unit box to
 * subcells, its facing rotates it, the shader projects + shades it. */
#ifndef TANK_MESH_DATA_H
#define TANK_MESH_DATA_H

#include <stdint.h>

#define MESH_VSTRIDE 12  /* bytes per baked mesh vertex (pos snorm8x4 + normal snorm8x4 + colour unorm8x4) */

/* The baked meshes, in ONE index space across two sources: the PROCEDURAL meshes
 * [0, M_PROC_COUNT) are host-baked by tools/gen_meshes.c -> src/mesh_data.c; the
 * KENNEY map meshes [M_PROC_COUNT, M_COUNT) are baked from CC0 art by
 * tools/bake_map_assets.mjs -> src/map_mesh_data.c. The runtime concatenates the two
 * vertex buffers (procedural first), so a mesh id indexes the combined buffer. */
enum { M_CUBE = 0, M_PYLON, M_PYRAMID, M_HULL, M_TURRET, M_PROC_COUNT,
       M_MAP_FLOOR = M_PROC_COUNT, M_MAP_WALL, M_MAP_NEST, M_COUNT };

extern const int8_t   MESH_VERT[];                   /* procedural meshes, MESH_VSTRIDE bytes/vertex */
extern const uint32_t MESH_VOFF[M_PROC_COUNT];       /* first vertex of each procedural mesh */
extern const uint32_t MESH_VCNT[M_PROC_COUNT];       /* vertex count of each procedural mesh */
extern const uint32_t MESH_VERT_TOTAL;               /* procedural vertices (size of MESH_VERT) */
extern const uint8_t  MESH_FOR_KIND[];               /* asset mesh id per opaque kind (K_OPAQUE_COUNT entries) */

#define M_MAP_COUNT (M_COUNT - M_PROC_COUNT)
extern const int8_t   MAP_MESH_VERT[];               /* Kenney map meshes, MESH_VSTRIDE bytes/vertex */
extern const uint32_t MAP_MESH_VOFF[M_MAP_COUNT];    /* first vertex of each map mesh (within MAP_MESH_VERT) */
extern const uint32_t MAP_MESH_VCNT[M_MAP_COUNT];    /* vertex count of each map mesh */
extern const uint32_t MAP_MESH_VERT_TOTAL;           /* map vertices (size of MAP_MESH_VERT) */

#endif
