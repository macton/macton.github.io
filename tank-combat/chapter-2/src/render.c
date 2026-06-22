#include "render.h"
#include "edge_paths.h"
#include "dirtab.h"

/* Pack a color as conventional RGBA8888 (little-endian wasm stores r,g,b,a in
 * memory == the shader's unorm8x4 attribute). */
#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const uint32_t COL_WALL = RGBA( 82,  87, 102, 255);
/* tank colours by index: 0 = manual (amber), 1..3 = pathed */
static const uint32_t COL_BODY[N_TANKS] = {
  RGBA(242, 158,  41, 255), RGBA( 77, 179, 230, 255), RGBA(120, 205, 120, 255), RGBA(196, 140, 235, 255) };
static const uint32_t COL_BARR[N_TANKS] = {
  RGBA(145,  95,  24, 255), RGBA( 46, 107, 138, 255), RGBA( 72, 123,  72, 255), RGBA(118,  84, 141, 255) };
static const uint32_t COL_PATH = RGBA(120, 230, 130, 255);  /* path highlight dots  */
static const uint32_t COL_SEL  = RGBA(255, 255, 255, 255);  /* selection ring       */
static const uint32_t COL_EDGE = RGBA( 70, 150, 165, 255);  /* edge-point (gap) marks */

static uint32_t push_quad(Inst* out, uint32_t k, int32_t cx, int32_t cy,
                          int32_t hx, int32_t hy, int32_t co, int32_t si, uint32_t rgba) {
  Inst* o = &out[k];
  o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->hx = (int16_t)hx; o->hy = (int16_t)hy;
  o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}
/* an axis-aligned marker centred on local subcell (lx,ly) */
static uint32_t mark(Inst* out, uint32_t k, int32_t lx, int32_t ly, int32_t h, uint32_t rgba) {
  return push_quad(out, k, lx, ly, h, h, 16384, 0, rgba);
}

/* Is world cell (wcx,wcy) in the camera screen? If so, give its screen-local
 * subcell centre offset (the cell's local column/row * SUB). */
static int in_camera(const World* w, int wcx, int wcy, int* lcx, int* lcy) {
  if ((uint32_t)(wcx / GRID_W) != w->cam_sx || (uint32_t)(wcy / GRID_H) != w->cam_sy) return 0;
  *lcx = (wcx % GRID_W); *lcy = (wcy % GRID_H);
  return 1;
}

uint32_t build_walls(const World* w, Inst* out) {
  uint32_t s = (uint32_t)w->cam_sy * SCREENS_X + w->cam_sx, k = 0;
  for (int cy = 0; cy < GRID_H; cy++)
    for (int cx = 0; cx < GRID_W; cx++)
      if ((w->grid[s * GRID_H + cy] >> cx) & 1u)
        k = push_quad(out, k, cx * SUB + SUB / 2, cy * SUB + SUB / 2, 123, 123, 16384, 0, COL_WALL);
  return k;
}

static uint16_t g_trace[BIG_W * BIG_H];   /* scratch for the highlight path (static, no alloc) */

uint32_t build_dynamic(const World* w, Inst* out, int show_path) {
  uint32_t k = 0;

  /* edge-point (gap) markers for the current screen — show the connectivity */
  uint32_t s = (uint32_t)w->cam_sy * SCREENS_X + w->cam_sx;
  for (uint32_t i = 0; i < w->screen_ep_count[s]; i++) {
    uint32_t e = w->screen_ep[s * EP_PER_SCREEN_MAX + i];
    int lcx = w->ep_cell[e] % GRID_W, lcy = w->ep_cell[e] / GRID_W;
    k = mark(out, k, lcx * SUB + SUB / 2, lcy * SUB + SUB / 2, 30, COL_EDGE);
  }

  /* the selected pathed tank's planned route, read straight out of the tables.
   * Reserve the tail of the dynamic region for the tanks + selection ring so a
   * long highlight can never crowd them out (or overflow into the wall region). */
  uint32_t hl_cap = INST_DYN_MAX - (N_TANKS * 2 + 1);
  if (show_path && is_pathed(w->selected)) {
    uint32_t p = w->selected - 1;
    uint32_t n = path_trace(w, p, g_trace, BIG_W * BIG_H);
    for (uint32_t i = 0; i < n && k < hl_cap; i++) {
      int wcx = g_trace[i] % BIG_W, wcy = g_trace[i] / BIG_W, lcx, lcy;
      if (in_camera(w, wcx, wcy, &lcx, &lcy))
        k = mark(out, k, lcx * SUB + SUB / 2, lcy * SUB + SUB / 2, 40, COL_PATH);
    }
  }

  /* selection ring (a larger marker behind the tank), then the tanks on top */
  int ox = (int)w->cam_sx * GRID_W * SUB, oy = (int)w->cam_sy * GRID_H * SUB;
  if (is_pathed(w->selected)) {
    int wcx = wrap_wcx(xy_lo(w->tank_xy[w->selected]) >> SUB_SHIFT);
    int wcy = wrap_wcy(xy_hi(w->tank_xy[w->selected]) >> SUB_SHIFT);
    int lcx, lcy;
    if (in_camera(w, wcx, wcy, &lcx, &lcy)) {
      int lx = xy_lo(w->tank_xy[w->selected]) - ox, ly = xy_hi(w->tank_xy[w->selected]) - oy;
      k = mark(out, k, lx, ly, 110, COL_SEL);
    }
  }
  for (uint32_t t = 0; t < N_TANKS; t++) {
    int wcx = wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT);
    int wcy = wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT);
    int lcx, lcy;
    if (!in_camera(w, wcx, wcy, &lcx, &lcy)) continue;
    uint32_t di = w->tank_ang[t] >> ANGLE_SHIFT;
    int32_t co = dir_cos(di), si = dir_sin(di);
    int32_t cx = xy_lo(w->tank_xy[t]) - ox, cy = xy_hi(w->tank_xy[t]) - oy;
    k = push_quad(out, k, cx, cy, 87, 67, co, si, COL_BODY[t]);
    int32_t bx = cx + ((co * 87) >> TRIG_SHIFT);
    int32_t by = cy + ((si * 87) >> TRIG_SHIFT);
    k = push_quad(out, k, bx, by, 56, 18, co, si, COL_BARR[t]);
  }

  while (k < INST_DYN_MAX) k = push_quad(out, k, 0, 0, 0, 0, 0, 0, 0);  /* zero-size pad */
  return k;
}
