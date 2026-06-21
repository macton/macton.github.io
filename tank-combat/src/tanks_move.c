#include "tanks_move.h"
#include "dirtab.h"

/* Is the tank's square at (px,py) subcells blocked by a wall or the edge?
 * Out-of-range policy: the edge counts as blocked (reject), so the tank stays
 * inside [TANK_R, ARENA-TANK_R] and grid row indices are always valid. */
static int32_t blocked(const uint32_t* grid, int32_t px, int32_t py) {
  if (px < TANK_R || py < TANK_R ||
      px > ARENA_W_SUB - TANK_R || py > ARENA_H_SUB - TANK_R) return 1;
  int32_t c0 = (px - TANK_R) >> SUB_SHIFT, c1 = (px + TANK_R) >> SUB_SHIFT;
  int32_t r0 = (py - TANK_R) >> SUB_SHIFT, r1 = (py + TANK_R) >> SUB_SHIFT;
  uint32_t span = ((1u << (c1 - c0 + 1)) - 1u) << c0;   /* the columns the tank covers */
  for (int32_t r = r0; r <= r1; r++)
    if (grid[r] & span) return 1;                        /* any wall in those columns?  */
  return 0;
}

void tanks_move(int16_t* x, int16_t* y, int16_t* vx, int16_t* vy, uint8_t* hit,
                const uint16_t* ang, const uint8_t* in, uint32_t n,
                int32_t speed, const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t thr = (int32_t)((in[i] & IN_FWD) != 0) - (int32_t)((in[i] & IN_BACK) != 0);
    uint32_t di = ang[i] >> ANGLE_SHIFT;
    int32_t mx = (dir_cos(di) * speed * thr) >> TRIG_SHIFT;   /* subcells/tick */
    int32_t my = (dir_sin(di) * speed * thr) >> TRIG_SHIFT;
    int32_t h = 0;
    int32_t nx = x[i] + mx;
    if (mx && blocked(grid, nx, y[i])) { mx = 0; h |= 1; } else x[i] = (int16_t)nx;  /* bit0: x blocked */
    int32_t ny = y[i] + my;
    if (my && blocked(grid, x[i], ny)) { my = 0; h |= 2; } else y[i] = (int16_t)ny;  /* bit1: y blocked */
    vx[i] = (int16_t)mx; vy[i] = (int16_t)my; hit[i] = (uint8_t)h;
  }
}
