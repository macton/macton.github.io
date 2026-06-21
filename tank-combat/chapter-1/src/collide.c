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

uint32_t cell_pattern(const uint32_t* grid, int32_t cx, int32_t cy) {
  return (((grid[WRAPR(cy - 1)] >> WRAPC(cx)) & 1u) << 0)   /* N */
       | (((grid[WRAPR(cy)] >> WRAPC(cx + 1)) & 1u) << 1)   /* E */
       | (((grid[WRAPR(cy + 1)] >> WRAPC(cx)) & 1u) << 2)   /* S */
       | (((grid[WRAPR(cy)] >> WRAPC(cx - 1)) & 1u) << 3);  /* W */
}

void patterns_build_open(uint32_t pattern_open[N_PATTERNS]) {
  /* The probe only reads the 4 orthogonal neighbours, so we synthesise a tiny
   * grid that holds exactly the pattern's neighbours around one interior cell
   * and run the same centre-origin probe collide uses everywhere. One source of
   * truth for the geometry; 16 patterns instead of 300 cells. */
  int32_t cx = GRID_W / 2, cy = GRID_H / 2;        /* interior: no wrap overlap */
  int32_t px = cx * SUB + SUB / 2, py = cy * SUB + SUB / 2;
  for (uint32_t p = 0; p < N_PATTERNS; p++) {
    uint32_t g[GRID_H] = {0};
    if (p & 1u) g[cy - 1] |= 1u << cx;             /* N */
    if (p & 2u) g[cy]     |= 1u << (cx + 1);       /* E */
    if (p & 4u) g[cy + 1] |= 1u << cx;             /* S */
    if (p & 8u) g[cy]     |= 1u << (cx - 1);       /* W */
    uint32_t mask = 0;
    for (uint32_t d = 0; d < N_DIRS; d++) {
      int32_t dx = (dir_cos(d) * SUB) >> TRIG_SHIFT;             /* one cell over */
      int32_t dy = (dir_sin(d) * SUB) >> TRIG_SHIFT;
      if (dx && blocked_x(g, wrap_x(px + dx), py, dx)) continue;
      if (dy && blocked_y(g, px, wrap_y(py + dy), dy)) continue;
      mask |= (1u << d);
    }
    pattern_open[p] = mask;
  }
}
