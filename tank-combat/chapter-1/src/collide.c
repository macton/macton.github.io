#include "collide.h"

/* TANK_R < SUB, so the footprint spans at most two columns and two rows; wrap
 * each into the grid (GRID_W/GRID_H are not powers of two, so add/sub-wrap). */
#define WRAPC(c) ((c) < 0 ? (c) + GRID_W : ((c) >= GRID_W ? (c) - GRID_W : (c)))
#define WRAPR(r) ((r) < 0 ? (r) + GRID_H : ((r) >= GRID_H ? (r) - GRID_H : (r)))

int blocked(const uint32_t* grid, int32_t px, int32_t py) {
  px = wrap_x(px); py = wrap_y(py);
  int32_t c0 = WRAPC((px - TANK_R) >> SUB_SHIFT);   /* >> floors (incl. to -1) */
  int32_t c1 = WRAPC((px + TANK_R) >> SUB_SHIFT);
  int32_t r0 = WRAPR((py - TANK_R) >> SUB_SHIFT);
  int32_t r1 = WRAPR((py + TANK_R) >> SUB_SHIFT);
  uint32_t cmask = (1u << c0) | (1u << c1);          /* the (1 or 2) columns    */
  if (grid[r0] & cmask) return 1;
  if (r1 != r0 && (grid[r1] & cmask)) return 1;
  return 0;
}
