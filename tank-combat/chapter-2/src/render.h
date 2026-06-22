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
 * CHAPTER 2 viewport: the world is 4x4 screens but the page shows ONE screen at
 * a time, so the renderer builds quads in SCREEN-LOCAL subcells for the camera
 * screen (cam_sx,cam_sy). The canvas is therefore still a single GRID_W x GRID_H
 * arena, exactly like chapter 1 — only the camera changes what is built. Walls,
 * tanks, the path highlight, and edge-point markers are all clipped to the
 * camera screen. */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "sim.h"

typedef struct { int16_t cx, cy, hx, hy, co, si; uint32_t rgba; } Inst;

/* The instance buffer is split by frequency of change (as in chapter 1):
 *
 *   [0 .. INST_DYN_MAX)            dynamic: tanks in view, the selected tank's
 *                                  path highlight, the selection ring, the
 *                                  current screen's edge-point markers — rebuilt
 *                                  every tick (unused slots are zero-size quads)
 *   [INST_DYN_MAX .. +wall_count)  the camera screen's walls — rebuilt only when
 *                                  the camera moves or a wall is edited
 *
 * Per screen there are at most N_CELLS wall quads and N_CELLS highlight cells. */
#define INST_DYN_MAX (N_TANKS * 2 + N_CELLS + EP_PER_SCREEN_MAX + 1)

/* Camera screen's walls, into out[0..]. Returns the wall count. Call only when
 * the camera moves or the grid changes. */
uint32_t build_walls(const World* w, Inst* out);

/* Dynamic quads (tanks in view + path highlight + selection + edge markers),
 * rewritten every tick into out[0..INST_DYN_MAX). Pads the tail with zero-size
 * quads. Returns INST_DYN_MAX. */
uint32_t build_dynamic(const World* w, Inst* out, int show_path);

#endif
