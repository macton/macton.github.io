#include "tanks_move.h"
#include "dirtab.h"
#include "collide.h"

void tanks_move(int16_t* x, int16_t* y, int16_t* vx, int16_t* vy, uint8_t* hit,
                const uint16_t* ang, const uint8_t* in, uint32_t n,
                int32_t speed, const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t thr = (int32_t)((in[i] & IN_FWD) != 0) - (int32_t)((in[i] & IN_BACK) != 0);
    uint32_t di = ang[i] >> ANGLE_SHIFT;
    int32_t mx = (dir_cos(di) * speed * thr) >> TRIG_SHIFT;   /* subcells/tick */
    int32_t my = (dir_sin(di) * speed * thr) >> TRIG_SHIFT;
    int32_t h = 0;
    int32_t nx = wrap_x(x[i] + mx);   /* toroidal arena: crossing an edge wraps */
    if (mx && blocked(grid, nx, y[i])) { mx = 0; h |= 1; } else x[i] = (int16_t)nx;  /* bit0: x blocked */
    int32_t ny = wrap_y(y[i] + my);
    if (my && blocked(grid, x[i], ny)) { my = 0; h |= 2; } else y[i] = (int16_t)ny;  /* bit1: y blocked */
    vx[i] = (int16_t)mx; vy[i] = (int16_t)my; hit[i] = (uint8_t)h;
  }
}
