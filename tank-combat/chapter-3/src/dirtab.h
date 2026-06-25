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

/* A finer 256-step cosine for the TURRET only (its aim, shot, and drawn barrel/bolt):
 * the full Q5.11 heading is bucketed to 1 of 256 (1.4-degree) steps via `ang >> 8`,
 * eight times the 32-direction movement resolution. fine_sin trails a quarter-turn
 * (64 of the 256 steps). The body + mites keep using dir_cos/dir_sin above. */
#define N_FINE 256
#define FINE_QUARTER (N_FINE / 4)
extern const int16_t FINE_COS[N_FINE];

static inline int16_t fine_cos(uint16_t ang) { return FINE_COS[(ang >> 8) & (N_FINE - 1)]; }
static inline int16_t fine_sin(uint16_t ang) { return FINE_COS[((ang >> 8) - FINE_QUARTER) & (N_FINE - 1)]; }

#endif
