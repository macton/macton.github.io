/* mesh_data.h — the PROCEDURAL low-poly meshes for the DYNAMIC things (tanks, the
 * swarm, FX, overlays). Host-baked by tools/gen_meshes.c and committed as
 * src/mesh_data.c (the same bake-it-once discipline as the escape table), then
 * exposed through wasm so the page uploads them ONCE at startup and instances them
 * every frame. The STATIC TOWN meshes (roads/buildings/grass/landmark) are a
 * SEPARATE table — src/map_mesh_data.{c,h}, baked from Kenney CC0 art — because the
 * town is static map data placed by the offline static bake (src/staticmap.c), not a
 * per-frame entity. The two mesh tables share one GPU vertex buffer (procedural
 * first, town second), so a draw addresses a town mesh at M_PROC_COUNT + its town id.
 *
 * A mesh is a NON-INDEXED triangle list — flat per-face shading needs a per-face
 * normal, so faces don't share vertices. Each vertex is 12 bytes:
 *   int8  px,py,pz,_   position in a unit box [-1,1] (snorm8x4 on the GPU)
 *   int8  nx,ny,nz,_   face normal           (snorm8x4)
 *   uint8 r,g,b,a      per-vertex colour     (unorm8x4)
 * The shader computes meshColour * instanceTint: the PROCEDURAL meshes are white, so
 * the instance tint shows through (chapter-3 behaviour); the baked town meshes carry
 * their real palette colour with a white tint — except the landmark, which the
 * 63 per-nest tints colour. The instance's half-extents scale the unit box to
 * subcells, its facing rotates it, the shader projects + shades it. */
#ifndef TANK_MESH_DATA_H
#define TANK_MESH_DATA_H

#include <stdint.h>

#define MESH_VSTRIDE 12  /* bytes per baked mesh vertex (pos snorm8x4 + normal snorm8x4 + colour unorm8x4) */

/* The procedural meshes (the dynamic things). The town meshes are NOT in this enum —
 * they are indexed by their own group bases in map_mesh_data.h and offset by
 * M_PROC_COUNT into the shared GPU vertex buffer at draw time. */
/* The PLACEHOLDER dynamic meshes. M_HULL / M_TURRET are the simple procedural tank (a tapered
 * box body + a tapered dome turret), authored in ONE shared unit-box frame so the renderer spins
 * the hull by the chassis heading and the turret by the aim while they stay registered (same
 * centre + scale, differing only in rotation); M_PYRAMID is the swarm mite (a spike). These are
 * what MESH_FOR_KIND binds. Externally-sourced art (see ASSETS.md) would drop into this table as
 * additional meshes re-bound to K_HULL / K_TURRET / K_MITE. */
enum { M_CUBE = 0, M_PYLON, M_PYRAMID, M_HULL, M_TURRET, M_TREE, M_ARROW, M_PROC_COUNT };

extern const int8_t   MESH_VERT[];                   /* procedural meshes, MESH_VSTRIDE bytes/vertex */
extern const uint32_t MESH_VOFF[M_PROC_COUNT];       /* first vertex of each procedural mesh */
extern const uint32_t MESH_VCNT[M_PROC_COUNT];       /* vertex count of each procedural mesh */
extern const uint32_t MESH_VERT_TOTAL;               /* procedural vertices (size of MESH_VERT) */
extern const uint8_t  MESH_FOR_KIND[];               /* procedural mesh id per opaque kind (K_OPAQUE_COUNT entries) */

#endif
