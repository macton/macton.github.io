#include "collide.h"
#include "dirtab.h"

/* TANK_R < SUB, so the footprint spans at most two cells per axis; wrap each
 * index into the grid (GRID_W/GRID_H aren't powers of two, so add/sub-wrap). */
#define WRAPC(c) ((c) < 0 ? (c) + GRID_W : ((c) >= GRID_W ? (c) - GRID_W : (c)))
#define WRAPR(r) ((r) < 0 ? (r) + GRID_H : ((r) >= GRID_H ? (r) - GRID_H : (r)))

int blocked_x(const uint32_t* grid, int32_t nx, int32_t y, int32_t mx) {
  int32_t ex = nx + (mx > 0 ? TANK_R : -TANK_R);            /* leading x edge      */
  uint32_t m = 1u << (uint32_t)WRAPC(ex >> SUB_SHIFT);      /* the column it enters */
  int32_t r0 = WRAPR((y - TANK_R) >> SUB_SHIFT);
  int32_t r1 = WRAPR((y + TANK_R) >> SUB_SHIFT);            /* rows the tank spans */
  return (grid[r0] & m) || (grid[r1] & m);
}

int blocked_y(const uint32_t* grid, int32_t x, int32_t ny, int32_t my) {
  int32_t ey = ny + (my > 0 ? TANK_R : -TANK_R);            /* leading y edge      */
  int32_t row = WRAPR(ey >> SUB_SHIFT);                     /* the row it enters   */
  uint32_t c0 = (uint32_t)WRAPC((x - TANK_R) >> SUB_SHIFT);
  uint32_t c1 = (uint32_t)WRAPC((x + TANK_R) >> SUB_SHIFT); /* cols the tank spans */
  return (grid[row] & ((1u << c0) | (1u << c1))) != 0;
}

void cells_build_move(uint32_t* cell_move, const uint32_t* grid) {
  for (int32_t cy = 0; cy < GRID_H; cy++) {
    for (int32_t cx = 0; cx < GRID_W; cx++) {
      int32_t px = cx * SUB + SUB / 2, py = cy * SUB + SUB / 2;   /* cell centre */
      uint32_t mask = 0;
      for (uint32_t d = 0; d < N_DIRS; d++) {
        int32_t dx = (dir_cos(d) * SUB) >> TRIG_SHIFT;            /* one cell over */
        int32_t dy = (dir_sin(d) * SUB) >> TRIG_SHIFT;
        if (dx && blocked_x(grid, wrap_x(px + dx), py, dx)) continue;
        if (dy && blocked_y(grid, px, wrap_y(py + dy), dy)) continue;
        mask |= (1u << d);
      }
      cell_move[cy * GRID_W + cx] = mask;
    }
  }
}
