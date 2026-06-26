/* render.h — the data contract with the WebGPU renderer, isolated here so the
 * boundary is a file, not a convention. Reads the simulation's World and produces
 * the instance buffer + a tiny draw list; the simulation never depends on this.
 *
 * CHAPTER 4 — the view becomes TOP-DOWN PERSPECTIVE 3-D, and NOTHING in the sim changes. The
 * renderer keeps chapter 3's instance-per-thing model but now emits a 3-D WORLD
 * PLACEMENT per instance and lets the vertex shader do the projection
 * (a perspective MVP). render.c does NOT bake screen positions — it emits placements; the
 * shader projects them, so the camera stays a uniform, not a rebuild.
 *
 * One instance is a box: a centre, a half-extent, a facing (rotation about the
 * vertical axis), and a tint. 24 bytes, all integer, GPU-native layout:
 *   int32  wx, wy   (subcells)   off 0   box centre x/y — int32 because the 8x8
 *                                        arena reaches 40960 subcells, past int16
 *   int16  wz, hz   (subcells)   off 8   centre height + half-height
 *   int16  hx, hy   (subcells)   off 12  half-extent x/y
 *   int16  co, si   (Q14)        off 16  facing (cos, sin)
 *   uint32 rgba                  off 20  tint, packed RGBA8888
 * Growth from chapter 3's 16-byte 2-D quad is the third dimension (+wz +hz) plus the
 * int32 world coordinates, reusing co/si (facing) and rgba (tint). The "kind/mesh id" is NOT a
 * per-instance field: nothing in the shader reads it (the mesh is bound per draw),
 * so storing it per instance would be dead weight. It lives instead in the DRAW
 * GROUPING below — instances are emitted contiguously by kind and the host draws
 * one range per kind, binding that kind's mesh (placeholder cube, or a baked
 * low-poly mesh in the asset pass). Same instances, different mesh: late-bound art.
 *
 * The whole 8x8 world is one connected map: placements are WORLD positions (anchor =
 * the world origin), so all sixty-four screens sit in their natural layout and the host
 * camera (free pan + zoom) shows whatever part. The view is rebuilt every frame (the
 * camera, the swarm, and the FX all move), but only for what the camera SHOWS — the
 * cells and the tanks/mites/nests/FX inside the visible world-space box the host
 * passes — so the cost still scales with VISIBILITY: the whole world when zoomed out,
 * a handful of cells when zoomed in, never the 4800-cell world or the 1000-strong
 * pool wholesale once you zoom.
 *
 * It also emits render-only INTERACTION overlays — a hover-cell highlight, the
 * selected tank's state ring, its destination beacon, and the routed path tiles —
 * all derived from the sim (or, for the cursor, from one host-set hover cell); none
 * of it feeds back into the model. */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "sim.h"

typedef struct { int32_t wx, wy; int16_t wz, hz, hx, hy, co, si; uint32_t rgba; } Inst;

/* mesh kinds — the slot a draw call binds its mesh by. The opaque kinds emit
 * first, in this order, each as a contiguous instance range (z-buffered, flat
 * face-shaded). The translucent kinds emit last, into ONE range drawn after the
 * opaque pass (depth-test on, depth-write off, alpha-blended, painter-sorted). */
enum {
  K_FLOOR = 0, K_WALL, K_NEST, K_RING, K_MITE, K_HULL, K_TURRET, K_BARREL, K_DEST, /* opaque */
  K_OPAQUE_COUNT,                    /* opaque kinds are [0, K_OPAQUE_COUNT) */
  K_LASER = K_OPAQUE_COUNT, K_BURST, K_PATH, K_HOVER,   /* translucent (all drawn as cubes) */
  K_COUNT
};

/* what the host needs to issue the draws: per-opaque-kind instance counts (the
 * ranges are contiguous in emit order, so kind i starts at the sum of the earlier
 * counts) and the translucent instance count (one sorted range after the opaque). */
typedef struct {
  uint32_t opaque[K_OPAQUE_COUNT];
  uint32_t translucent;
} DrawList;

/* Capacity per built view: the worst case is the whole world in view — every cell
 * (one block each), the whole live swarm, the nests, every tank's hull+turret+barrel
 * (3) + up to PROJ_MAX 2-box bolts in flight + a 2-piece selection mark + a destination
 * beacon, the FX ring, and the routed path (capped). Render scales with what's shown. */
#define PATH_MAX  400                    /* path tiles across all routing tanks (capped) */
#define INST_MAX (N_WORLD_CELLS + N_MITES + NEST_COUNT + N_TANKS * (6 + PROJ_MAX * 2) \
                  + N_FX + PATH_MAX + 1 /*hover*/)

/* Build the whole view into out[0..) (grouped by kind) and fill `dl`. Emits at WORLD
 * positions everything inside the visible box [wx0,wx1] x [wy0,wy1] (subcells) the
 * host passes — terrain cells, nests, tanks, the swarm, FX, and the overlays — so the
 * cost scales with the visible region. `hover_cell` is the world cell under the cursor
 * (REC_EMPTY for none). Returns the total instance count. */
uint32_t build_view(const World* w, Inst* out, DrawList* dl,
                    int32_t wx0, int32_t wy0, int32_t wx1, int32_t wy1, uint32_t hover_cell);

#endif
