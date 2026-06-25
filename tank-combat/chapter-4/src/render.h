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
 * all move), but only for what the camera SHOWS — the viewport screen's cells plus
 * a one-cell margin, and the tanks/mites/nests/FX on those cells — so the cost
 * scales with VISIBILITY, not with the 4800-cell world or the 1000-strong pool. */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "sim.h"

typedef struct { int16_t wx, wy, wz, hz, hx, hy, co, si; uint32_t rgba; } Inst;

/* mesh kinds — the slot a draw call binds its mesh by. The opaque kinds emit
 * first, in this order, each as a contiguous instance range (z-buffered, flat
 * face-shaded). The translucent FX kinds emit last, into ONE range drawn after the
 * opaque pass (depth-test on, depth-write off, alpha-blended, painter-sorted). */
enum {
  K_FLOOR = 0, K_WALL, K_NEST, K_MITE, K_HULL, K_TURRET, K_BARREL, /* opaque */
  K_OPAQUE_COUNT,            /* opaque kinds are [0, K_OPAQUE_COUNT) */
  K_LASER = K_OPAQUE_COUNT, K_BURST,   /* translucent (both drawn as cubes) */
  K_COUNT
};

/* what the host needs to issue the draws: per-opaque-kind instance counts (the
 * ranges are contiguous in emit order, so kind i starts at the sum of the earlier
 * counts) and the translucent instance count (one sorted range after the opaque). */
typedef struct {
  uint32_t opaque[K_OPAQUE_COUNT];
  uint32_t translucent;
} DrawList;

/* Capacity per built view: up to two screens (viewport + the one sliding in) of
 * terrain (one block per visible cell, with a one-cell margin) + nests, plus the
 * mites/tanks/FX the camera shows. A mite/burst is on exactly one screen, so at
 * most N_MITES + N_FX of those are ever visible across the (<=2) built screens, and
 * each tank contributes a hull + turret + barrel (+ a 2-box laser). Render scales
 * with what's shown, capped by the pools. */
#define VIS_CELLS ((GRID_W + 2) * (GRID_H + 2))   /* a screen's cells + a one-cell margin */
#define INST_MAX  (2 * (VIS_CELLS + NEST_COUNT) + N_MITES + N_TANKS * 3 + N_FX + N_TANKS * 2)

/* Build the whole view into out[0..) (grouped by kind) and fill `dl`. Emits the
 * viewport screen (cam_sx,cam_sy) in a camera-local subcell frame; if `sliding`,
 * also the `to` screen offset by (dx,dy) screen-widths (dx,dy in {-1,0,1}), so the
 * two screens sit adjacent across the toroidal seam and the host slides the camera
 * uniform between them. Returns the total instance count. */
uint32_t build_view(const World* w, Inst* out, DrawList* dl,
                    uint32_t cam_sx, uint32_t cam_sy,
                    int sliding, uint32_t to_sx, uint32_t to_sy, int dx, int dy);

#endif
