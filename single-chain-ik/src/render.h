/* render.h — the data contract with the WebGPU renderer, isolated to a file so
 * the boundary is explicit. Reads the simulation's World and produces a packed
 * instance buffer; the simulation never depends on this.
 *
 * One instance per quad, 16 bytes, all integer (identical layout to the other
 * projects here so the host shader is the same shape):
 *   int16  cx, cy   (subcells)  off 0   centre, RELATIVE to the follow camera
 *   int16  hx, hy   (subcells)  off 4   half-extent
 *   int16  co, si   (Q14)       off 8   rotation (cos, sin) of the quad
 *   uint32 rgba                 off 12  colour, packed RGBA8888
 *
 * Centres are emitted relative to the camera (the body, horizontally; a fixed
 * ground line, vertically) so the walker stays framed and the values stay inside
 * int16 even though world x grows without bound as it walks. The whole view is
 * rebuilt every frame (the ground scrolls and every leg re-solves), so there is
 * no static/dynamic split to keep. */
#ifndef SCIK_RENDER_H
#define SCIK_RENDER_H

#include "sim.h"

typedef struct { int16_t cx, cy, hx, hy, co, si; uint32_t rgba; } Inst;

/* The visible window, in subcells, that the host maps onto the canvas. Wide and
 * short: the walker is ~3 cells tall and the interest is the ground scrolling
 * past, so a 2:1 banner frames it with a little sky above and ground below. */
#define VIEW_W (16 * SUB)
#define VIEW_H (8 * SUB)

/* The follow camera: horizontally it tracks the body; vertically it holds this
 * steady line (a touch above the mean ground), so the walker sits upper-middle.
 * Instances are emitted relative to (body_x, CAM_Y). */
#define CAM_Y (TERRAIN_MID - SUB / 2)

#define INST_MAX 512

/* Build the whole view into out[0..) and return the quad count. When show_stages
 * is set, the leg `focus` also gets the length/direction teaching overlay. */
uint32_t build_view(const World* w, Inst* out, int show_stages, int focus);

#endif
