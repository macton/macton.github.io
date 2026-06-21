/* defs.h — shared vocabulary: fixed sizes, input bits, exact-width types.
 * These are constraints the whole module exploits, not parameters to expose. */
#ifndef TANK_DEFS_H
#define TANK_DEFS_H

#include <stdint.h>

#define GRID_W      20
#define GRID_H      15
#define N_CELLS     (GRID_W * GRID_H)
#define N_TANKS     2
#define N_DIRS      32          /* discrete headings: angle >> ANGLE_SHIFT   */
#define ANGLE_SHIFT 11          /* 65536 >> 11 == 32                         */
#define SUB         256         /* subcells per grid cell (Q8.8)             */
#define SUB_SHIFT   8
#define ARENA_W_SUB (GRID_W * SUB)
#define ARENA_H_SUB (GRID_H * SUB)
#define TANK_R      92          /* collision half-extent, subcells (~0.36c)  */
#define TRIG_SHIFT  14          /* the DIR_COS table is Q14                   */

/* tank input bitfield */
#define IN_FWD   1u
#define IN_BACK  2u
#define IN_LEFT  4u
#define IN_RIGHT 8u

/* The arena is a torus: positions wrap. There is no hard edge — whether you can
 * cross an edge depends only on the wall on the far side. */
static inline int32_t wrap_x(int32_t x) { x %= ARENA_W_SUB; return x < 0 ? x + ARENA_W_SUB : x; }
static inline int32_t wrap_y(int32_t y) { y %= ARENA_H_SUB; return y < 0 ? y + ARENA_H_SUB : y; }

#endif
