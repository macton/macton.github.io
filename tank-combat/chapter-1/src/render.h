/* render.h — the data contract with the WebGPU renderer, isolated here so the
 * boundary is a file, not a convention. Reads the simulation's World and
 * produces the instance buffer; the simulation never depends on this.
 *
 * One instance per quad, 16 bytes, all integer:
 *   int16  cx, cy   (subcells)   off 0   center
 *   int16  hx, hy   (subcells)   off 4   half-extent
 *   int16  co, si   (Q14)        off 8   rotation (cos, sin)
 *   uint32 rgba                  off 12  color, packed RGBA8888
 *
 * The color's bytes land in memory as r,g,b,a (little-endian), which is what
 * the shader's unorm8x4 reads. */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "sim.h"

typedef struct { int16_t cx, cy, hx, hy, co, si; uint32_t rgba; } Inst;

/* The instance buffer is split by frequency of change, because ~93% of the quads
 * (the walls) only change on a grid edit while the tanks change every tick:
 *
 *   [0 .. INST_DYN_MAX)              dynamic: tank quads + sample overlay,
 *                                    rewritten every tick (unused slots are
 *                                    zero-size quads, so a contiguous draw skips
 *                                    them)
 *   [INST_DYN_MAX .. +wall_count)    static walls, written once per grid change
 *
 * So the per-tick path writes only INST_DYN_MAX quads, and the host re-uploads
 * only that front region each frame (the walls are uploaded once, again only when
 * the grid changes). N_TANKS*2 tank quads + up to N_TANKS*8 overlay markers. */
#define INST_DYN_MAX (N_TANKS * 2 + N_TANKS * 8)

/* Static walls: one quad per wall cell, written into out[0..]. Returns the wall
 * count. Call only when the grid changes (init, edit). */
uint32_t build_walls(const World* w, Inst* out);

/* Dynamic quads (tank bodies + barrels, plus the debug sample overlay when
 * show_samples): rewritten every tick into out[0..INST_DYN_MAX). Pads the unused
 * tail with zero-size quads. Returns INST_DYN_MAX.
 *
 * The sample overlay marks exactly the grid cells the collision/steer code reads
 * this tick for each moving tank — the leading-edge cell(s) movement tests (one
 * colour) and the 4 orthogonal neighbours the steer reads (another). */
uint32_t build_dynamic(const World* w, Inst* out, int show_samples);

#endif
