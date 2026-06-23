#include "render.h"
#include "edge_paths.h"
#include "dirtab.h"

#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const uint32_t COL_WALL = RGBA(82, 87, 102, 255);
/* per-tank identity colours: body, barrel, and the lighter path shade */
static const uint32_t COL_BODY[N_TANKS] = {
  RGBA(242, 158, 41, 255), RGBA(77, 179, 230, 255), RGBA(120, 205, 120, 255), RGBA(196, 140, 235, 255) };
static const uint32_t COL_BARR[N_TANKS] = {
  RGBA(150, 98, 26, 255), RGBA(48, 110, 142, 255), RGBA(74, 127, 74, 255), RGBA(121, 86, 145, 255) };
static const uint32_t COL_PATH[N_TANKS] = {   /* a lighter variation of the body colour */
  RGBA(255, 205, 120, 255), RGBA(150, 215, 250, 255), RGBA(180, 240, 180, 255), RGBA(225, 190, 250, 255) };
/* state outline colours (UNSELECTED draws no outline) */
static const uint32_t COL_AUTOPATH = RGBA(120, 235, 140, 255);
static const uint32_t COL_MANUAL   = RGBA(245, 225, 90, 255);

/* the belief field, drawn right on the mites: idle teal (no record) -> relaying
 * amber (carrying a record while wandering) -> seeking red, fading from hot to
 * cold as the recorded sighting ages. Watch a sighting diffuse out and dissolve. */
static const uint32_t COL_MITE_IDLE      = RGBA( 96, 150, 138, 255);
static const uint32_t COL_MITE_RELAY     = RGBA(222, 182,  74, 255);
static const uint32_t COL_MITE_SEEK_HOT  = RGBA(240,  92,  60, 255);
static const uint32_t COL_MITE_SEEK_COLD = RGBA(150,  64,  74, 255);
#define MITE_AGE_HOT 150   /* frames a sighting stays "hot" in the belief tint */

static uint32_t mite_colour(const World* w, uint32_t m) {
  if (w->mite_rec_cell[w->rec_buf][m] == REC_EMPTY) return COL_MITE_IDLE;   /* no belief */
  if (w->mite_mode[m] == MM_SEEK)
    return (w->frame - w->mite_rec_time[w->rec_buf][m]) < MITE_AGE_HOT ? COL_MITE_SEEK_HOT : COL_MITE_SEEK_COLD;
  return COL_MITE_RELAY;                                                    /* holds a record, wandering */
}

static uint32_t push(Inst* out, uint32_t k, int32_t cx, int32_t cy, int32_t hx, int32_t hy,
                     int32_t co, int32_t si, uint32_t rgba) {
  if (k >= INST_MAX) return k;
  Inst* o = &out[k];
  o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->hx = (int16_t)hx; o->hy = (int16_t)hy;
  o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}

static uint8_t  g_pmask[N_CELLS];          /* per visible cell: which tanks' paths cross it */
static uint16_t g_trace[BIG_W * BIG_H];    /* path-trace scratch */

/* build one screen's quads at pixel offset (ox,oy): walls, path strips, then
 * tanks (state outline behind, body+barrel on top). */
static uint32_t build_screen(const World* w, Inst* out, uint32_t k,
                             uint32_t screen, int ox, int oy) {
  /* walls */
  for (int cy = 0; cy < GRID_H; cy++)
    for (int cx = 0; cx < GRID_W; cx++)
      if ((w->grid[screen * GRID_H + cy] >> cx) & 1u)
        k = push(out, k, ox + cx * SUB + SUB / 2, oy + cy * SUB + SUB / 2, 123, 123, 16384, 0, COL_WALL);

  /* paths: mark which tanks route through each cell of this screen */
  for (uint32_t i = 0; i < N_CELLS; i++) g_pmask[i] = 0;
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (!w->phas[t] || w->pstatus[t] != PS_ROUTING) continue;
    uint32_t n = path_trace(w, t, g_trace, BIG_W * BIG_H);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t wcx = g_trace[i] % BIG_W, wcy = g_trace[i] / BIG_W;
      if ((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W) == screen)
        g_pmask[(wcy % GRID_H) * GRID_W + (wcx % GRID_W)] |= (uint8_t)(1u << t);
    }
  }
  for (uint32_t cell = 0; cell < N_CELLS; cell++) {
    uint8_t mask = g_pmask[cell]; if (!mask) continue;
    int count = 0; for (uint32_t t = 0; t < N_TANKS; t++) if (mask & (1u << t)) count++;
    int cx0 = ox + (int)(cell % GRID_W) * SUB + SUB / 2;
    int cy0 = oy + (int)(cell / GRID_W) * SUB + SUB / 2;
    int j = 0, span = 140;                                  /* split a ~0.55-cell marker into strips */
    for (uint32_t t = 0; t < N_TANKS; t++) {
      if (!(mask & (1u << t))) continue;
      int sw = span / count, sx = cx0 - span / 2 + j * sw + sw / 2;
      k = push(out, k, sx, cy0, sw / 2 - 3, 70, 16384, 0, COL_PATH[t]);
      j++;
    }
  }

  int sox = (int)(screen % SCREENS_X) * GRID_W * SUB, soy = (int)(screen / SCREENS_X) * GRID_H * SUB;

  /* the mites on THIS screen only — build instances for what the camera shows, so
   * the cost scales with the visible swarm, not the 1000-strong pool. Each is one
   * small quad rotated to its heading, tinted by its belief (mite_colour). */
  for (uint32_t m = 0; m < N_MITES; m++) {
    int wcx = wrap_wcx(xy_lo(w->mite_xy[m]) >> SUB_SHIFT), wcy = wrap_wcy(xy_hi(w->mite_xy[m]) >> SUB_SHIFT);
    if ((uint32_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W)) != screen) continue;
    int lx = ox + xy_lo(w->mite_xy[m]) - sox, ly = oy + xy_hi(w->mite_xy[m]) - soy;
    uint32_t di = w->mite_ang[m] >> ANGLE_SHIFT;
    k = push(out, k, lx, ly, 40, 40, dir_cos(di), dir_sin(di), mite_colour(w, m));
  }

  /* tank state outlines (behind the tanks, above the mites) */
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (w->tstate[t] == TS_UNSELECTED) continue;
    int wcx = wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT), wcy = wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT);
    if ((uint32_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W)) != screen) continue;
    int lx = ox + xy_lo(w->tank_xy[t]) - sox, ly = oy + xy_hi(w->tank_xy[t]) - soy;
    k = push(out, k, lx, ly, 118, 100, 16384, 0, w->tstate[t] == TS_MANUAL ? COL_MANUAL : COL_AUTOPATH);
  }
  /* tank bodies + barrels */
  for (uint32_t t = 0; t < N_TANKS; t++) {
    int wcx = wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT), wcy = wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT);
    if ((uint32_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W)) != screen) continue;
    int lx = ox + xy_lo(w->tank_xy[t]) - sox, ly = oy + xy_hi(w->tank_xy[t]) - soy;
    uint32_t di = w->tank_ang[t] >> ANGLE_SHIFT; int32_t co = dir_cos(di), si = dir_sin(di);
    k = push(out, k, lx, ly, 87, 67, co, si, COL_BODY[t]);
    k = push(out, k, lx + ((co * 87) >> TRIG_SHIFT), ly + ((si * 87) >> TRIG_SHIFT), 56, 18, co, si, COL_BARR[t]);
  }
  return k;
}

uint32_t build_view(const World* w, Inst* out,
                    uint32_t cam_sx, uint32_t cam_sy,
                    int sliding, uint32_t to_sx, uint32_t to_sy, int dx, int dy) {
  uint32_t k = build_screen(w, out, 0, cam_sy * SCREENS_X + cam_sx, 0, 0);
  if (sliding)
    k = build_screen(w, out, k, to_sy * SCREENS_X + to_sx, dx * GRID_W * SUB, dy * GRID_H * SUB);
  return k;
}
