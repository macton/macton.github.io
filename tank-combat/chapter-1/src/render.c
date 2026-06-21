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

static const uint32_t COL_SMP_MOVE = RGBA(120, 230, 130, 255);  /* movement edge */
static const uint32_t COL_SMP_TURN = RGBA(228, 110, 220, 255);  /* steer neighbour */

/* a small centred marker on cell (cx,cy), axis-aligned, ~half a cell */
static uint32_t mark(Inst* out, uint32_t k, int32_t cx, int32_t cy, uint32_t rgba) {
  return push_quad(out, k, cx * SUB + SUB / 2, cy * SUB + SUB / 2,
                   64, 64, 16384, 0, rgba);
}
#define WC(c) ((c) < 0 ? (c) + GRID_W : ((c) >= GRID_W ? (c) - GRID_W : (c)))
#define WR(r) ((r) < 0 ? (r) + GRID_H : ((r) >= GRID_H ? (r) - GRID_H : (r)))

uint32_t build_sample_overlay(const World* w, Inst* out, uint32_t k) {
  for (uint32_t i = 0; i < N_TANKS; i++) {
    int32_t thr = (int32_t)((w->tank_in[i] & IN_FWD) != 0)
                - (int32_t)((w->tank_in[i] & IN_BACK) != 0);
    if (!thr) continue;                               /* only when moving */
    int32_t x = xy_lo(w->tank_xy[i]), y = xy_hi(w->tank_xy[i]);
    uint32_t di = w->tank_ang[i] >> ANGLE_SHIFT;
    int32_t mx = (dir_cos(di) * (int32_t)w->move_speed * thr) >> TRIG_SHIFT;
    int32_t my = (dir_sin(di) * (int32_t)w->move_speed * thr) >> TRIG_SHIFT;

    /* movement: the column / row the leading edge enters (collide.c) */
    if (mx) {
      int32_t col = WC(((x + mx) + (mx > 0 ? TANK_R : -TANK_R)) >> SUB_SHIFT);
      k = mark(out, k, col, WR((y - TANK_R) >> SUB_SHIFT), COL_SMP_MOVE);
      k = mark(out, k, col, WR((y + TANK_R) >> SUB_SHIFT), COL_SMP_MOVE);
    }
    if (my) {
      int32_t row = WR(((y + my) + (my > 0 ? TANK_R : -TANK_R)) >> SUB_SHIFT);
      k = mark(out, k, WC((x - TANK_R) >> SUB_SHIFT), row, COL_SMP_MOVE);
      k = mark(out, k, WC((x + TANK_R) >> SUB_SHIFT), row, COL_SMP_MOVE);
    }
    /* steering: the 4 orthogonal neighbours read when auto-steering this tick */
    if (w->tank_hit[i]) {
      int32_t cx = x >> SUB_SHIFT, cy = y >> SUB_SHIFT;
      k = mark(out, k, WC(cx), WR(cy - 1), COL_SMP_TURN);
      k = mark(out, k, WC(cx + 1), WR(cy), COL_SMP_TURN);
      k = mark(out, k, WC(cx), WR(cy + 1), COL_SMP_TURN);
      k = mark(out, k, WC(cx - 1), WR(cy), COL_SMP_TURN);
    }
  }
  return k;
}

uint32_t build_instances(const World* w, Inst* out) {
  uint32_t k = 0;
  for (int32_t r = 0; r < GRID_H; r++)
    for (int32_t c = 0; c < GRID_W; c++)
      if ((w->grid[r] >> c) & 1u)
        k = push_quad(out, k, c * SUB + SUB / 2, r * SUB + SUB / 2,
                      123, 123, 16384, 0, COL_WALL);
  for (uint32_t i = 0; i < N_TANKS; i++) {
    uint32_t di = w->tank_ang[i] >> ANGLE_SHIFT;
    int32_t co = dir_cos(di), si = dir_sin(di);
    int32_t cx = xy_lo(w->tank_xy[i]), cy = xy_hi(w->tank_xy[i]);
    k = push_quad(out, k, cx, cy, 87, 67, co, si, COL_BODY[i]);
    /* barrel: a short bar offset forward (0.34 cell == 87 subcells) */
    int32_t bx = cx + ((co * 87) >> TRIG_SHIFT);
    int32_t by = cy + ((si * 87) >> TRIG_SHIFT);
    k = push_quad(out, k, bx, by, 56, 18, co, si, COL_BARR[i]);
  }
  return k;
}
