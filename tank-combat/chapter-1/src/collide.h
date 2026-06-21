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

/* Build the per-cell open-direction masks: cell_move[cy*GRID_W+cx] gets bit d
 * set if a tank centred in that cell can step one cell along direction d without
 * hitting a wall (wrapping included). This is the intermediate that the escape
 * table (tanks_build_escape) is built from, recomputed only on grid change. */
void cells_build_move(uint32_t* cell_move, const uint32_t* grid);

#endif
