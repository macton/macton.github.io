/* render.h — the data contract with the WebGPU renderer, isolated here so the
 * boundary is a file, not a convention. Reads the simulation's World and produces
 * the instance buffer + a tiny draw list; the simulation never depends on this.
 *
 * CHAPTER 4 — the view becomes ISOMETRIC 3-D, and NOTHING in the sim changes. The
 * renderer keeps chapter 3's instance-per-thing model but now emits a 3-D WORLD
 * PLACEMENT per instance and lets the vertex shader do the dimetric projection
 * (see iso.h). render.c does NOT bake screen positions — it emits placements; the
 * shader projects them, so the camera stays a uniform, not a rebuild.
 *
 * One instance is a box: a centre, a half-extent, a facing (rotation about the
 * vertical axis), and a tint. 20 bytes, all integer, GPU-native layout:
 *   int16  wx, wy   (subcells)   off 0   box centre, camera-local x/y
 *   int16  wz, hz   (subcells)   off 4   centre height + half-height
 *   int16  hx, hy   (subcells)   off 8   half-extent x/y
 *   int16  co, si   (Q14)        off 12  facing (cos, sin)
 *   uint32 rgba                  off 16  tint, packed RGBA8888
 * Growth from chapter 3's 16-byte 2-D quad is exactly +wz +hz — the third
 * dimension — reusing co/si (facing) and rgba (tint). The "kind/mesh id" is NOT a
 * per-instance field: nothing in the shader reads it (the mesh is bound per draw),
 * so storing it per instance would be dead weight. It lives instead in the DRAW
 * GROUPING below — instances are emitted contiguously by kind and the host draws
 * one range per kind, binding that kind's mesh (placeholder cube, or a baked
 * low-poly mesh in the asset pass). Same instances, different mesh: late-bound art.
 *
 * The whole view is rebuilt every frame (the follow camera, the swarm, and the FX
 * all move), but only for what the camera SHOWS — the viewport screen AND its eight
 * toroidal neighbours (a connected 3x3 neighbourhood), and the tanks/mites/nests/FX
 * on those screens — so the cost scales with VISIBILITY (9 of the 16 screens), not
 * with the 4800-cell world or the 1000-strong pool. The neighbours sit adjacent
 * across the toroidal seam so the world reads as one connected space; the host's
 * follow camera pans within the neighbourhood and re-anchors as it crosses a screen.
 *
 * It also emits render-only INTERACTION overlays — a hover-cell highlight, the
 * selected tank's state ring, its destination beacon, and the routed path tiles —
 * all derived from the sim (or, for the cursor, from one host-set hover cell); none
 * of it feeds back into the model. */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "sim.h"

typedef struct { int16_t wx, wy, wz, hz, hx, hy, co, si; uint32_t rgba; } Inst;

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

/* Capacity per built view: the 3x3 neighbourhood's terrain (one block per cell) +
 * its nests, plus the mites/tanks/FX on those screens and the interaction overlays.
 * A mite/burst is on exactly one screen, so at most N_MITES + N_FX of those are
 * visible across the <=9 screens; each tank contributes a hull+turret+barrel (+ a
 * 2-box laser, a state ring, a destination beacon); the path is capped. Render
 * scales with what's shown, capped by the pools. */
#define VIS_SCREENS 9                    /* the camera screen + its 8 toroidal neighbours */
#define PATH_MAX    400                  /* path tiles across all routing tanks (capped) */
#define INST_MAX (VIS_SCREENS * N_CELLS + N_MITES + NEST_COUNT + N_TANKS * 3 \
                  + N_TANKS /*rings*/ + N_TANKS /*dest beacons*/ \
                  + N_FX + N_TANKS * 2 /*laser*/ + PATH_MAX + 1 /*hover*/)

/* Build the whole view into out[0..) (grouped by kind) and fill `dl`. Emits the 3x3
 * neighbourhood around the viewport screen (cam_sx,cam_sy) in a camera-local subcell
 * frame anchored at that screen, the neighbours offset by one screen-width each
 * (adjacent across the toroidal seam); the host pans the camera uniform within it.
 * `hover_cell` is the world cell under the cursor (REC_EMPTY for none). Returns the
 * total instance count. */
uint32_t build_view(const World* w, Inst* out, DrawList* dl,
                    uint32_t cam_sx, uint32_t cam_sy, uint32_t hover_cell);

#endif
