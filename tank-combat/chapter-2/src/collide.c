#include "collide.h"

/* TANK_R < SUB, so the footprint spans at most two cells per axis. Those two
 * cells may fall in different screens at a border, so we test each via
 * cell_is_wall (which resolves the screen and wraps) rather than a single
 * row-word mask — the whole-world bitset is screen-major, not one word per
 * world row. The leading-edge cell may sit one cell outside [0,BIG_W)/[0,BIG_H)
 * before wrapping; cell_is_wall handles that. */
int blocked_x(const uint32_t* grid, int32_t nx, int32_t y, int32_t mx) {
  int32_t ex  = nx + (mx > 0 ? TANK_R : -TANK_R);   /* leading x edge      */
  int32_t col = ex >> SUB_SHIFT;                     /* world column it enters */
  int32_t r0  = (y - TANK_R) >> SUB_SHIFT;
  int32_t r1  = (y + TANK_R) >> SUB_SHIFT;           /* rows the tank spans */
  return cell_is_wall(grid, col, r0) || cell_is_wall(grid, col, r1);
}

int blocked_y(const uint32_t* grid, int32_t x, int32_t ny, int32_t my) {
  int32_t ey  = ny + (my > 0 ? TANK_R : -TANK_R);   /* leading y edge      */
  int32_t row = ey >> SUB_SHIFT;                      /* the row it enters   */
  int32_t c0  = (x - TANK_R) >> SUB_SHIFT;
  int32_t c1  = (x + TANK_R) >> SUB_SHIFT;            /* cols the tank spans */
  return cell_is_wall(grid, c0, row) || cell_is_wall(grid, c1, row);
}

uint32_t cell_pattern(const uint32_t* grid, int32_t cx, int32_t cy) {
  return ((uint32_t)cell_is_wall(grid, cx,     cy - 1) << 0)   /* N */
       | ((uint32_t)cell_is_wall(grid, cx + 1, cy    ) << 1)   /* E */
       | ((uint32_t)cell_is_wall(grid, cx,     cy + 1) << 2)   /* S */
       | ((uint32_t)cell_is_wall(grid, cx - 1, cy    ) << 3);  /* W */
}
