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

/* a mite's tint shows its CURRENT role (re-derived every tick, so it is never stale):
 * idle teal (wander, no record) -> hunting red, fading hot to cold as the sighting ages
 * -> homing in its NEST's colour (the 20% carrying a record back to one of the four
 * nests). Homing is transient: on reaching its nest a mite delivers the record and
 * reverts to wander (teal), so nest-tint means "in transit", not a permanent state.
 * Watch a sighting diffuse out from a contact, swarm in, and dissolve when it goes stale. */
static const uint32_t COL_MITE_IDLE     = RGBA( 96, 150, 138, 255);
static const uint32_t COL_MITE_HUNT_HOT = RGBA(240,  92,  60, 255);
static const uint32_t COL_MITE_HUNT_COLD= RGBA(150,  64,  74, 255);
/* the nests (one per screen but (0,0)), drawn on the map and used to tint mites homing to
 * each — a hue wheel so adjacent screens read as different colours (match NESTC in app.js) */
static const uint32_t COL_NEST[NEST_COUNT] = {
  RGBA(230, 146, 110, 255), RGBA(230, 194, 110, 255), RGBA(218, 230, 110, 255), RGBA(170, 230, 110, 255),
  RGBA(122, 230, 110, 255), RGBA(110, 230, 146, 255), RGBA(110, 230, 194, 255), RGBA(110, 218, 230, 255),
  RGBA(110, 170, 230, 255), RGBA(110, 122, 230, 255), RGBA(146, 110, 230, 255), RGBA(194, 110, 230, 255),
  RGBA(230, 110, 218, 255), RGBA(230, 110, 170, 255), RGBA(230, 110, 122, 255) };
static const uint32_t COL_LASER_GLOW = RGBA(255,  96,  64, 255);   /* the beam's wide outer glow */
static const uint32_t COL_LASER_CORE = RGBA(255, 244, 224, 255);   /* its thin bright core */
#define MITE_AGE_HOT 150   /* frames a sighting stays "hot" in the hunt tint */

/* a destruction burst's colour by age: a bright flash darkening toward the arena
 * background over its lifetime (the renderer is opaque, so "fade" = darken, not alpha). */
static uint32_t fx_colour(int age) {
  int n = FX_DURATION - 1; if (n < 1) n = 1; if (age > n) age = n;
  int r = 255 + (34  - 255) * age / n;
  int g = 240 + (36  - 240) * age / n;
  int b = 190 + (46  - 190) * age / n;
  return RGBA(r, g, b, 255);
}

static uint32_t mite_colour(const World* w, uint32_t m) {
  uint8_t mode = w->mite_mode[m];
  if (mode == MM_HOME) return COL_NEST[nest_of(m)];                          /* carrying it home */
  if (mode == MM_HUNT)
    return (w->frame - w->mite_rec_time[w->rec_buf][m]) < MITE_AGE_HOT ? COL_MITE_HUNT_HOT : COL_MITE_HUNT_COLD;
  return COL_MITE_IDLE;                                                      /* wander (no belief) */
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

  /* the nests (home cells) on this screen: a large marker per nest, under the
   * mites, in the nest's colour — the homes the 20% carry sightings back to. */
  for (uint32_t n = 0; n < NEST_COUNT; n++) {
    int nwx = wc_x(w->nest_cell[n]), nwy = wc_y(w->nest_cell[n]);
    if ((uint32_t)((nwy / GRID_H) * SCREENS_X + (nwx / GRID_W)) != screen) continue;
    int lx = ox + (nwx % GRID_W) * SUB + SUB / 2, ly = oy + (nwy % GRID_H) * SUB + SUB / 2;
    k = push(out, k, lx, ly, 116, 116, 16384, 0, COL_NEST[n]);
  }

  /* the mites on THIS screen only — build instances for what the camera shows, so
   * the cost scales with the visible swarm, not the 1000-strong pool. Each is one
   * small quad rotated to its heading, tinted by its role (mite_colour). */
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;                          /* dead mites aren't drawn (or indexed) */
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
  /* tank bodies + barrels: the BODY faces its movement heading (tank_ang); the
   * BARREL faces the turret (tank_turret), which aims independently. A live shot is a
   * LASER — a glow + bright core from the muzzle to where the beam met a wall. */
  for (uint32_t t = 0; t < N_TANKS; t++) {
    int wcx = wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT), wcy = wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT);
    if ((uint32_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W)) != screen) continue;
    int lx = ox + xy_lo(w->tank_xy[t]) - sox, ly = oy + xy_hi(w->tank_xy[t]) - soy;
    uint32_t bi = w->tank_ang[t]    >> ANGLE_SHIFT; int32_t bco = dir_cos(bi), bsi = dir_sin(bi);
    uint32_t ti = w->tank_turret[t] >> ANGLE_SHIFT; int32_t tco = dir_cos(ti), tsi = dir_sin(ti);

    /* beam: project the (toroidal) muzzle->endpoint vector onto the turret dir for its
     * length, then a quad centred on the half-way point — a wide glow, a thin core. */
    if (w->tank_tracer[t]) {
      int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
      int scx = wc_x(w->tank_shot_cell[t]) * SUB + SUB / 2, scy = wc_y(w->tank_shot_cell[t]) * SUB + SUB / 2;
      int ddx = scx - tx; if (ddx >  ARENA_W_SUB / 2) ddx -= ARENA_W_SUB; if (ddx < -ARENA_W_SUB / 2) ddx += ARENA_W_SUB;
      int ddy = scy - ty; if (ddy >  ARENA_H_SUB / 2) ddy -= ARENA_H_SUB; if (ddy < -ARENA_H_SUB / 2) ddy += ARENA_H_SUB;
      int len = (ddx * tco + ddy * tsi) >> TRIG_SHIFT;     /* ~distance along the turret to the wall */
      if (len > 0) {
        int half = len / 2;
        int mx = lx + ((tco * half) >> TRIG_SHIFT), my = ly + ((tsi * half) >> TRIG_SHIFT);
        k = push(out, k, mx, my, half, 16, tco, tsi, COL_LASER_GLOW);   /* outer glow */
        k = push(out, k, mx, my, half,  5, tco, tsi, COL_LASER_CORE);   /* bright core */
      }
    }

    k = push(out, k, lx, ly, 87, 67, bco, bsi, COL_BODY[t]);
    k = push(out, k, lx + ((tco * 87) >> TRIG_SHIFT), ly + ((tsi * 87) >> TRIG_SHIFT), 56, 18, tco, tsi, COL_BARR[t]);
  }

  /* destruction bursts on top: expanding, darkening diamonds where the laser killed a
   * mite — only those whose position is on this screen (cost scales with what's shown). */
  for (uint32_t i = 0; i < N_FX; i++) {
    if (!w->fx_t[i]) continue;
    int fxx = xy_lo(w->fx_xy[i]), fxy = xy_hi(w->fx_xy[i]);
    int wcx = wrap_wcx(fxx >> SUB_SHIFT), wcy = wrap_wcy(fxy >> SUB_SHIFT);
    if ((uint32_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W)) != screen) continue;
    int lx = ox + fxx - sox, ly = oy + fxy - soy;
    int age = FX_DURATION - (int)w->fx_t[i];             /* 0 (fresh) .. FX_DURATION-1 */
    int half = 22 + age * 9;                              /* expands as it fades */
    k = push(out, k, lx, ly, half, half, 11585, 11585, fx_colour(age));   /* 45-degree diamond */
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
