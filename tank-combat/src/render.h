/* render.h — the data contract with the WebGPU renderer, isolated here so the
 * boundary is a file, not a convention. The sim produces this buffer; JS
 * uploads it verbatim and the vertex shader reads exactly this layout.
 *
 * One instance per quad, 16 bytes, all integer:
 *   int16  cx, cy   (subcells)   off 0   center
 *   int16  hx, hy   (subcells)   off 4   half-extent
 *   int16  co, si   (Q14)        off 8   rotation (cos, sin)
 *   uint32 rgba                  off 12  color, packed RGBA8888
 *
 * The color is conventional packed RGBA: on little-endian wasm its bytes land
 * in memory as r,g,b,a, which is exactly what the shader's unorm8x4 reads. */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "defs.h"

typedef struct { int16_t cx, cy, hx, hy, co, si; uint32_t rgba; } Inst;

/* world state -> instance buffer. Pure transform: reads the grid bitset and
 * the tank arrays, writes up to N_CELLS + n_tanks*2 instances into out,
 * returns the count. n_tanks must be <= N_TANKS (the color palette is sized
 * to N_TANKS). */
uint32_t build_instances(Inst* out, const uint32_t* grid,
                         const int16_t* tank_x, const int16_t* tank_y,
                         const uint16_t* tank_ang, uint32_t n_tanks);

#endif
