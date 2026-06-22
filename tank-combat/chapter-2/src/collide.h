/* collide.h — per-axis grid collision for incremental tank moves, plus the
 * world-cell wall query the whole module shares.
 *
 * A tank steps less than one cell per tick, and its previous cell was already
 * clear, so on each axis only the LEADING edge (in the move direction) can newly
 * hit a wall. We therefore test just the one column (x-move) or row (y-move) the
 * leading edge enters, at the rows/columns the tank spans on the other axis.
 *
 * Chapter 2: the world is one big BIG_W x BIG_H toroidal grid, organised as a
 * 4x4 block of 20x15 screen-grids. `grid` is the flat screen-major bitset
 * (one uint32_t per screen-row, bit c == column c); `cell_is_wall` resolves a
 * world cell to its screen and wraps at the big-grid edge — so a tank's
 * footprint and the steer's neighbour probes cross inner screen borders and the
 * outer wrap by the same far-side rule, with no special cases. */
#ifndef TANK_COLLIDE_H
#define TANK_COLLIDE_H

#include "defs.h"

/* Is world cell (wcx,wcy) a wall? Wraps toroidally and resolves the screen.
 * grid is laid out grid[screen*GRID_H + cy], one 20-bit word per screen-row. */
static inline int cell_is_wall(const uint32_t* grid, int32_t wcx, int32_t wcy) {
  wcx = wrap_wcx(wcx); wcy = wrap_wcy(wcy);
  uint32_t s  = ((uint32_t)wcy / GRID_H) * SCREENS_X + ((uint32_t)wcx / GRID_W);
  uint32_t cx = (uint32_t)wcx % GRID_W,  cy = (uint32_t)wcy % GRID_H;
  return (int)((grid[s * GRID_H + cy] >> cx) & 1u);
}

/* Tank centre moving to (nx,y) by step mx (sign gives the leading edge): does
 * the leading x edge enter a wall, at the rows the tank spans at y? */
int blocked_x(const uint32_t* grid, int32_t nx, int32_t y, int32_t mx);
/* Symmetric for a y step. */
int blocked_y(const uint32_t* grid, int32_t x, int32_t ny, int32_t my);

/* A cell's local pattern: the 4 orthogonal neighbours' wall bits, N E S W in
 * bits 0..3 (toroidal). This 4-bit value is ALL the steer needs to know about a
 * cell — it indexes the precomputed escape table (see src/escape_table.h). */
uint32_t cell_pattern(const uint32_t* grid, int32_t cx, int32_t cy);

#endif
