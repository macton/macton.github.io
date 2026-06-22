#include "tanks_move.h"
#include "dirtab.h"
#include "collide.h"

void tanks_move(uint32_t* xy, uint32_t* vxy, uint8_t* hit,
                const uint16_t* ang, const uint8_t* in, uint32_t n,
                int32_t speed, int32_t coll_scale, const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    /* hit[i] still holds last tick's collision (turn only read it); if the tank
     * was colliding, move at a reduced speed this tick (coll_scale is Q0.8). */
    int32_t s = hit[i] ? (speed * coll_scale) >> 8 : speed;
    int32_t thr = (int32_t)((in[i] & IN_FWD) != 0) - (int32_t)((in[i] & IN_BACK) != 0);
    uint32_t di = ang[i] >> ANGLE_SHIFT;
    int32_t mx = (dir_cos(di) * s * thr) >> TRIG_SHIFT;   /* subcells/tick */
    int32_t my = (dir_sin(di) * s * thr) >> TRIG_SHIFT;
    int32_t x = xy_lo(xy[i]), y = xy_hi(xy[i]);              /* one load, both axes */
    int32_t h = 0;
    int32_t nx = wrap_x(x + mx);      /* toroidal arena: crossing an edge wraps */
    if (mx && blocked_x(grid, nx, y, mx)) { mx = 0; h |= 1; } else x = nx;  /* bit0: x blocked */
    int32_t ny = wrap_y(y + my);
    if (my && blocked_y(grid, x, ny, my)) { my = 0; h |= 2; } else y = ny;  /* bit1: y blocked */
    xy[i] = xy_pack(x, y); vxy[i] = xy_pack(mx, my); hit[i] = (uint8_t)h;
  }
}
