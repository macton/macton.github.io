#include "collide.h"

int blocked(const uint32_t* grid, int32_t px, int32_t py) {
  if (px < TANK_R || py < TANK_R ||
      px > ARENA_W_SUB - TANK_R || py > ARENA_H_SUB - TANK_R) return 1;
  int32_t c0 = (px - TANK_R) >> SUB_SHIFT, c1 = (px + TANK_R) >> SUB_SHIFT;
  int32_t r0 = (py - TANK_R) >> SUB_SHIFT, r1 = (py + TANK_R) >> SUB_SHIFT;
  uint32_t span = ((1u << (c1 - c0 + 1)) - 1u) << c0;   /* the columns the tank covers */
  for (int32_t r = r0; r <= r1; r++)
    if (grid[r] & span) return 1;                        /* any wall in those columns?  */
  return 0;
}
