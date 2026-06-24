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
 * CHAPTER 2 viewport: the page shows ONE screen at a time, built in screen-local
 * subcells. The camera follows the selected tank, and when it changes screen the
 * page slides the new screen in — so the renderer can build TWO screens at once
 * (the from-screen at offset 0 and the to-screen at a one-screen offset) and the
 * host animates the view between them. The follow camera, the updating paths, and
 * the slide change the whole view every frame, so it is rebuilt each frame.
 *
 * Each tank shows its IDENTITY as a body colour and its STATE as an outline
 * (AUTOPATH / MANUAL / none). A pathing tank's route is drawn in a lighter shade
 * of its colour; where two tanks' routes share a cell the marker is split into
 * coloured strips so both are visible.
 *
 * CHAPTER 3: the swarm is also drawn, but only the mites the camera shows — the
 * renderer scales with VISIBILITY, not with the 1000-strong pool. Each visible
 * mite is one small quad, tinted by its belief (idle teal / relaying amber /
 * seeking red, fading with the record's age), so the last-known position is
 * visible diffusing out from a sighting on the live map. The whole swarm appears
 * only on the minimap (downsampled dots in app.js). */
#ifndef TANK_RENDER_H
#define TANK_RENDER_H

#include "sim.h"

typedef struct { int16_t cx, cy, hx, hy, co, si; uint32_t rgba; } Inst;

/* Capacity per built view: up to two screens of walls + path strips +
 * tanks/outlines, plus the mites the camera shows. A mite is on exactly one
 * screen, so at most N_MITES mite quads are ever visible across the (<=2) built
 * screens — render scales with the visible swarm, capped by the whole pool. */
#define INST_MAX (2 * (N_CELLS + 512 + N_TANKS * 4 + NEST_COUNT) + N_MITES)

/* Build the whole view into out[0..) and return the quad count. Draws the camera
 * screen at local offset 0; if `sliding`, also the `to` screen offset by
 * (dx,dy) screen-widths (dx,dy in {-1,0,1}). */
uint32_t build_view(const World* w, Inst* out,
                    uint32_t cam_sx, uint32_t cam_sy,
                    int sliding, uint32_t to_sx, uint32_t to_sy, int dx, int dy);

#endif
