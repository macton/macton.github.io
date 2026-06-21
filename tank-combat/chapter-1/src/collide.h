/* collide.h — per-axis grid collision for incremental tank moves.
 *
 * A tank steps less than one cell per tick, and its previous cell was already
 * clear, so on each axis only the LEADING edge (in the move direction) can newly
 * hit a wall. We therefore test just the one column (x-move) or row (y-move) the
 * leading edge enters, at the rows/columns the tank spans on the other axis —
 * not the whole footprint. The arena is toroidal: coordinates wrap. */
#ifndef TANK_COLLIDE_H
#define TANK_COLLIDE_H

#include "defs.h"

/* Tank centre moving to (nx,y) by step mx (sign gives the leading edge): does
 * the leading x edge enter a wall, at the rows the tank spans at y? */
int blocked_x(const uint32_t* grid, int32_t nx, int32_t y, int32_t mx);
/* Symmetric for a y step. */
int blocked_y(const uint32_t* grid, int32_t x, int32_t ny, int32_t my);

/* A cell's local pattern: the 4 orthogonal neighbours' wall bits, N E S W in
 * bits 0..3 (toroidal). This 4-bit value is ALL the steer needs to know about a
 * cell — see patterns_build_open. */
uint32_t cell_pattern(const uint32_t* grid, int32_t cx, int32_t cy);

/* Build the open-direction mask for every one of the 16 local patterns:
 * pattern_open[p] gets bit d set if a tank centred in a cell with neighbour
 * pattern p can step one cell along direction d without hitting a wall. The
 * centre-origin probe only ever reads the 4 orthogonal neighbours, so the mask
 * is a pure function of the 4-bit pattern — grid-independent and only 16 rows
 * (it was once one row per cell). Built once; the escape table derives from it. */
void patterns_build_open(uint32_t pattern_open[N_PATTERNS]);

#endif
