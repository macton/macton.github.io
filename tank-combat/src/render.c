#include "render.h"
#include "dirtab.h"

/* Pack a color as conventional RGBA8888. Little-endian wasm stores the bytes
 * as r,g,b,a in memory == the shader's unorm8x4 attribute. */
#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const uint32_t COL_WALL          = RGBA( 82,  87, 102, 255);
static const uint32_t COL_BODY[N_TANKS] = { RGBA(242, 158,  41, 255), RGBA( 77, 179, 230, 255) };
static const uint32_t COL_BARR[N_TANKS] = { RGBA(145,  95,  24, 255), RGBA( 46, 107, 138, 255) };

static uint32_t push_quad(Inst* out, uint32_t k, int32_t cx, int32_t cy,
                          int32_t hx, int32_t hy, int32_t co, int32_t si, uint32_t rgba) {
  Inst* o = &out[k];
  o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->hx = (int16_t)hx; o->hy = (int16_t)hy;
  o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}

uint32_t build_instances(Inst* out, const uint32_t* grid,
                         const int16_t* tank_x, const int16_t* tank_y,
                         const uint16_t* tank_ang, uint32_t n_tanks) {
  uint32_t k = 0;
  for (int32_t r = 0; r < GRID_H; r++)
    for (int32_t c = 0; c < GRID_W; c++)
      if ((grid[r] >> c) & 1u)
        k = push_quad(out, k, c * SUB + SUB / 2, r * SUB + SUB / 2,
                      123, 123, 16384, 0, COL_WALL);
  for (uint32_t i = 0; i < n_tanks; i++) {
    uint32_t di = tank_ang[i] >> ANGLE_SHIFT;
    int32_t co = dir_cos(di), si = dir_sin(di);
    int32_t cx = tank_x[i], cy = tank_y[i];
    k = push_quad(out, k, cx, cy, 87, 67, co, si, COL_BODY[i]);
    /* barrel: a short bar offset forward (0.34 cell == 87 subcells) */
    int32_t bx = cx + ((co * 87) >> TRIG_SHIFT);
    int32_t by = cy + ((si * 87) >> TRIG_SHIFT);
    k = push_quad(out, k, bx, by, 56, 18, co, si, COL_BARR[i]);
  }
  return k;
}
