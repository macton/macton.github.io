/* sim.h — the simulation core: all world data + the per-tick update.
 *
 * This layer has NO dependency on rendering or on wasm. It is plain C over the
 * World struct and can be built and run natively (see test.c / test.sh). The
 * wasm boundary (wasm.c) and the renderer (render.c) sit on top of it.
 *
 * NUMBERS ARE INTEGER FIXED POINT (see defs.h / dirtab.h):
 *   positions/sizes  subcells, 256 == 1 cell (Q8.8), int16_t
 *   heading          uint16_t Q5.11 (top 5 bits = 1 of 32 directions)
 *   grid             one uint32_t per row, bit c == column c (1 = wall)
 * Fixed timestep: one sim_tick == one frame; per-tick deltas are the data. */
#ifndef TANK_SIM_H
#define TANK_SIM_H

#include "defs.h"

/* All simulation state, flat. Tanks are structure-of-arrays so each transform
 * streams the one field it needs — except fields always touched together, which
 * are merged into one word so a single load gets both (see xy_pack in defs.h):
 * x and y are never used apart (collision, pattern, render all read the pair),
 * so they are one `tank_xy`; likewise the applied move `tank_vxy`. Heading stays
 * separate: it has a different writer (turn writes it, move writes position). */
typedef struct {
  uint32_t tank_xy [N_TANKS];   /* packed position: x | y<<16, subcells (Q8.8) */
  uint16_t tank_ang[N_TANKS];   /* Q5.11 heading */
  uint8_t  tank_in [N_TANKS];   /* input bitfield (IN_*) */
  uint32_t tank_vxy[N_TANKS];   /* packed last-tick applied move: vx | vy<<16 */
  uint8_t  tank_hit[N_TANKS];   /* last tick's blocked-axis bitmask (bit0 x, bit1 y) */
  uint32_t grid    [GRID_H];    /* wall bitset, one row per word */
  uint8_t  pattern_escape[N_PATTERNS * N_DIRS];  /* steer lookup, indexed
                                 * [pattern*N_DIRS + travel_dir]: the precomputed
                                 * nearest-open escape (bit5 = handedness, bits0-4
                                 * = direction; 0xFF = none). `pattern` is a cell's
                                 * 4-neighbour wall code, read live. The escape is
                                 * a pure function of that pattern, so this table
                                 * is grid-independent — built once, never per
                                 * grid edit (16 rows, not one per cell). */
  uint32_t frame;
  uint16_t move_speed;          /* subcells per tick */
  uint16_t turn_rate;           /* angle units per tick */
} World;

_Static_assert(GRID_W <= 32, "a grid row must fit in one uint32_t");

void sim_init(World* w);         /* load the level, place tanks, build escape table */
void sim_tick(World* w);         /* advance one fixed step: turn then move */

#endif
