/* mesh_data.h — the late-bound art, host-baked. The asset pass swaps the
 * placeholder cube for a small set of low-poly meshes WITHOUT touching the
 * projection, the depth, the shading, or the sim. The meshes are pure data: built
 * on the host by tools/gen_meshes.c and committed as src/mesh_data.c (the same
 * bake-it-once discipline as the escape table), then exposed through wasm so the
 * page uploads them ONCE at startup and instances them every frame.
 *
 * A mesh is a NON-INDEXED triangle list — flat per-face shading needs a per-face
 * normal, so faces don't share vertices. Each vertex is 8 bytes:
 *   int8 px,py,pz,_   position in a unit box [-1,1] (snorm8x4 on the GPU)
 *   int8 nx,ny,nz,_   face normal           (snorm8x4)
 * The instance's half-extents scale the unit box to real subcells, its facing
 * rotates it, the shader projects + shades it. Swapping K_MITE's mesh from the cube
 * to M_PYRAMID changes the silhouette and nothing else: art is data you replace. */
#ifndef TANK_MESH_DATA_H
#define TANK_MESH_DATA_H

#include <stdint.h>

#define MESH_VSTRIDE 8   /* bytes per baked mesh vertex (pos snorm8x4 + normal snorm8x4) */

/* the baked meshes. M_CUBE is the placeholder for every kind (a thin slab, a wall
 * block, a body box — the instance's half-extents shape it); the asset pass binds
 * the richer meshes per kind (see MESH_FOR_KIND). */
enum { M_CUBE = 0, M_PYLON, M_PYRAMID, M_HULL, M_TURRET, M_COUNT };

extern const int8_t   MESH_VERT[];               /* all meshes concatenated, MESH_VSTRIDE bytes/vertex */
extern const uint32_t MESH_VOFF[M_COUNT];        /* first vertex of each mesh */
extern const uint32_t MESH_VCNT[M_COUNT];        /* vertex count of each mesh */
extern const uint32_t MESH_VERT_TOTAL;           /* total vertices (size of MESH_VERT in vertices) */
extern const uint8_t  MESH_FOR_KIND[];           /* asset mesh id per opaque kind (K_OPAQUE_COUNT entries) */

#endif
