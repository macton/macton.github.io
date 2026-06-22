/* map.h — the initial 4x4 world map, as committed row bit-words.
 *
 * Fully determined at authoring time, so (like the trig and escape tables) it is
 * generated on the host by tools/gen_map.c and baked into src/map_data.c, not
 * parsed or computed at runtime. Layout: GRID_INIT[screen*GRID_H + cy] is one
 * 20-bit word, bit c == column c (1 = wall), for screen `screen` (0..15, row
 * sy = screen/4, col sx = screen%4). Every screen border is wall except a
 * one-cell gap at the centre of each side; those gaps line up across inner
 * screen borders and across the outer toroidal wrap, so the whole 4x4 world is
 * one connected toroidal grid. */
#ifndef TANK_MAP_H
#define TANK_MAP_H

#include "defs.h"

extern const uint32_t GRID_INIT[N_SCREENS * GRID_H];

#endif
