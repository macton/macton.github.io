/* dirtab.h — one baked Q14 trig table for the discrete headings, shared by the
 * move and render transforms. cos of each of the N_DIRS directions (value ==
 * cos * 16384). No libm.
 *
 * sin needs no second table: sin x == cos(x - 90deg), so it is the same curve
 * a quarter-turn earlier. Index the cos table with a quarter-turn offset. */
#ifndef TANK_DIRTAB_H
#define TANK_DIRTAB_H

#include "defs.h"

#define DIR_QUARTER (N_DIRS / 4)

extern const int16_t DIR_COS[N_DIRS];

static inline int16_t dir_cos(uint32_t d) { return DIR_COS[d & (N_DIRS - 1)]; }
static inline int16_t dir_sin(uint32_t d) { return DIR_COS[(d - DIR_QUARTER) & (N_DIRS - 1)]; }

#endif
