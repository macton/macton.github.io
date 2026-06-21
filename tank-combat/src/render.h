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

/* world state -> instance buffer. Writes up to N_CELLS + N_TANKS*2 instances
 * into out and returns the count. */
uint32_t build_instances(const World* w, Inst* out);

#endif
