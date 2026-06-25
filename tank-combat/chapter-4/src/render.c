/* render.c — chapter 4: read the frozen World, emit 3-D world placements for an
 * ISOMETRIC view. Every instance is a box (centre + half-extent + facing + tint) in
 * a CAMERA-LOCAL subcell frame; the vertex shader projects it (iso.h) and resolves
 * occlusion with a z-buffer. This file emits PLACEMENTS only — it never bakes a
 * screen position and never writes the sim. Instances are grouped by kind so the
 * host draws one range per kind (binding the placeholder cube, or a baked mesh).
 *
 * It reads exactly chapter 3's inputs — tank_xy/ang/turret, mite_xy/ang/mode/resp,
 * the wall grid, the nests, the tracer/FX ring — and gains only the third
 * dimension (a height per kind) and the kind grouping. No new sim state. */
#include "render.h"
#include "iso.h"
#include "dirtab.h"
#include "collide.h"

#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const uint32_t COL_FLOOR = RGBA(44, 48, 58, 255);
static const uint32_t COL_WALL  = RGBA(96, 102, 120, 255);
/* per-tank identity colours: hull/turret, and the darker barrel */
static const uint32_t COL_BODY[N_TANKS] = {
  RGBA(242, 158, 41, 255), RGBA(77, 179, 230, 255), RGBA(120, 205, 120, 255), RGBA(196, 140, 235, 255) };
static const uint32_t COL_BARR[N_TANKS] = {
  RGBA(150, 98, 26, 255), RGBA(48, 110, 142, 255), RGBA(74, 127, 74, 255), RGBA(121, 86, 145, 255) };

/* a mite's tint shows its CURRENT role (re-derived every tick, never stale):
 * idle teal (wander) -> hunting red (hot, fading cold as the sighting ages) ->
 * homing in its NEST's colour (the 20% relaying a record back to a nest). */
static const uint32_t COL_MITE_IDLE      = RGBA( 96, 150, 138, 255);
static const uint32_t COL_MITE_HUNT_HOT  = RGBA(240,  92,  60, 255);
static const uint32_t COL_MITE_HUNT_COLD = RGBA(150,  64,  74, 255);
/* the nests (one per screen but the tank start), a hue wheel so adjacent screens
 * read as different colours (matches NESTC in app.js) */
static const uint32_t COL_NEST[NEST_COUNT] = {
  RGBA(230, 146, 110, 255), RGBA(230, 194, 110, 255), RGBA(218, 230, 110, 255), RGBA(170, 230, 110, 255),
  RGBA(122, 230, 110, 255), RGBA(110, 230, 146, 255), RGBA(110, 230, 194, 255), RGBA(110, 218, 230, 255),
  RGBA(110, 170, 230, 255), RGBA(110, 122, 230, 255), RGBA(146, 110, 230, 255), RGBA(194, 110, 230, 255),
  RGBA(230, 110, 218, 255), RGBA(230, 110, 170, 255), RGBA(230, 110, 122, 255) };
static const uint32_t COL_LASER_GLOW = RGBA(255,  96,  64, 120);   /* wide outer glow (translucent) */
static const uint32_t COL_LASER_CORE = RGBA(255, 244, 224, 235);   /* thin bright core */
#define MITE_AGE_HOT 150   /* frames a sighting stays "hot" in the hunt tint */

/* per-kind heights + footprints, in subcells (render-only — z lives nowhere in the
 * sim). A floor is a thin slab on the ground; a wall is a full-height cube; tanks
 * and mites sit on the floor; a nest is a tall pylon. */
#define FLOOR_H    (SUB / 8)        /* 32: a thin slab */
#define WALL_H     SUB              /* 256: a full-height cube */
#define MITE_H     (SUB / 3)        /* ~85 */
#define HULL_H     (SUB / 2)        /* 128 */
#define TURRET_H   (SUB / 4)        /* 64, on top of the hull */
#define BARREL_H   (SUB / 6)        /* ~42 */
#define NEST_H     (SUB + SUB / 2)  /* 384: a tall pylon */
#define LASER_H    (SUB / 8)        /* 32: a thin beam */
#define FLOOR_HALF (SUB / 2 - 4)    /* inset a hair so cell edges read */
#define WALL_HALF  (SUB / 2)        /* fills the cell footprint */
#define MITE_HALF  40
#define NEST_HALF  54
#define HULL_HX    87
#define HULL_HY    67
#define TURRET_HALF 42
#define BARREL_HW  16               /* barrel half-width */

static uint32_t burst_colour(int age) {     /* bright flash -> fully transparent over its life */
  int n = FX_DURATION - 1; if (n < 1) n = 1; if (age > n) age = n;
  int a = 230 + (0 - 230) * age / n;
  return RGBA(255, 232, 150, a);
}
static uint32_t mite_colour(const World* w, uint32_t m) {
  uint8_t mode = w->mite_mode[m];
  if (mode == MM_HOME) return COL_NEST[nest_of(m)];
  if (mode == MM_HUNT)
    return (w->frame - w->mite_rec_time[w->rec_buf][m]) < MITE_AGE_HOT ? COL_MITE_HUNT_HOT : COL_MITE_HUNT_COLD;
  return COL_MITE_IDLE;
}

/* emit one box (centre, half-extent, facing, tint) into the kind-grouped buffer */
static uint32_t push(Inst* out, uint32_t k, int cx, int cy, int cz,
                     int hx, int hy, int hz, int co, int si, uint32_t rgba) {
  if (k >= INST_MAX) return k;
  Inst* o = &out[k];
  o->wx = (int16_t)cx; o->wy = (int16_t)cy; o->wz = (int16_t)cz; o->hz = (int16_t)hz;
  o->hx = (int16_t)hx; o->hy = (int16_t)hy; o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}

/* the toroidal world cell at local cell (lcx,lcy) of `screen` */
static int screen_wall(const World* w, uint32_t screen, int lcx, int lcy) {
  int ssx = (int)(screen % SCREENS_X), ssy = (int)(screen / SCREENS_X);
  return cell_is_wall(w->grid, ssx * GRID_W + lcx, ssy * GRID_H + lcy);
}

/* is world subcell position (px,py) on `screen`? if so, give its camera-local
 * centre (ox/oy is the screen's local origin). visible things scale with the view. */
static int local_of(uint32_t screen, int ox, int oy, int px, int py, int* lx, int* ly) {
  int wcx = wrap_wcx(px >> SUB_SHIFT), wcy = wrap_wcy(py >> SUB_SHIFT);
  if ((uint32_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W)) != screen) return 0;
  int sox = (int)(screen % SCREENS_X) * GRID_W * SUB, soy = (int)(screen / SCREENS_X) * GRID_H * SUB;
  *lx = ox + px - sox; *ly = oy + py - soy;
  return 1;
}

/* ---- per-kind emitters: each appends its kind's instances for one screen ----- */

static uint32_t emit_terrain(const World* w, Inst* out, uint32_t k, int want_wall,
                             uint32_t screen, int ox, int oy) {
  for (int lcy = -1; lcy <= GRID_H; lcy++)            /* a one-cell margin so far-edge */
    for (int lcx = -1; lcx <= GRID_W; lcx++) {        /* walls don't pop in late */
      int wall = screen_wall(w, screen, lcx, lcy);
      if (wall != want_wall) continue;
      int cx = ox + lcx * SUB + SUB / 2, cy = oy + lcy * SUB + SUB / 2;
      if (want_wall) k = push(out, k, cx, cy, WALL_H / 2, WALL_HALF, WALL_HALF, WALL_H / 2, 16384, 0, COL_WALL);
      else           k = push(out, k, cx, cy, FLOOR_H / 2, FLOOR_HALF, FLOOR_HALF, FLOOR_H / 2, 16384, 0, COL_FLOOR);
    }
  return k;
}

static uint32_t emit_nests(const World* w, Inst* out, uint32_t k, uint32_t screen, int ox, int oy) {
  for (uint32_t n = 0; n < NEST_COUNT; n++) {
    int nwx = wc_x(w->nest_cell[n]), nwy = wc_y(w->nest_cell[n]);
    int lx, ly;
    if (!local_of(screen, ox, oy, nwx * SUB + SUB / 2, nwy * SUB + SUB / 2, &lx, &ly)) continue;
    k = push(out, k, lx, ly, NEST_H / 2, NEST_HALF, NEST_HALF, NEST_H / 2, 16384, 0, COL_NEST[n]);
  }
  return k;
}

static uint32_t emit_mites(const World* w, Inst* out, uint32_t k, uint32_t screen, int ox, int oy) {
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;                     /* dead mites aren't drawn (or indexed) */
    int lx, ly;
    if (!local_of(screen, ox, oy, xy_lo(w->mite_xy[m]), xy_hi(w->mite_xy[m]), &lx, &ly)) continue;
    uint32_t di = w->mite_ang[m] >> ANGLE_SHIFT;
    k = push(out, k, lx, ly, FLOOR_H + MITE_H / 2, MITE_HALF, MITE_HALF, MITE_H / 2,
             dir_cos(di), dir_sin(di), mite_colour(w, m));
  }
  return k;
}

static uint32_t emit_tank_part(const World* w, Inst* out, uint32_t k, int part,
                               uint32_t screen, int ox, int oy) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    int lx, ly;
    if (!local_of(screen, ox, oy, xy_lo(w->tank_xy[t]), xy_hi(w->tank_xy[t]), &lx, &ly)) continue;
    uint32_t bi = w->tank_ang[t]    >> ANGLE_SHIFT;
    uint32_t ti = w->tank_turret[t] >> ANGLE_SHIFT;
    int tco = dir_cos(ti), tsi = dir_sin(ti);
    if (part == K_HULL)
      k = push(out, k, lx, ly, FLOOR_H + HULL_H / 2, HULL_HX, HULL_HY, HULL_H / 2,
               dir_cos(bi), dir_sin(bi), COL_BODY[t]);
    else if (part == K_TURRET)
      k = push(out, k, lx, ly, FLOOR_H + HULL_H + TURRET_H / 2, TURRET_HALF, TURRET_HALF, TURRET_H / 2,
               tco, tsi, COL_BODY[t]);
    else { /* K_BARREL: a thin box reaching out from the turret along its aim */
      int off = HULL_HX;   /* push the barrel's centre out past the hull front */
      int bx = lx + ((tco * off) >> TRIG_SHIFT), by = ly + ((tsi * off) >> TRIG_SHIFT);
      k = push(out, k, bx, by, FLOOR_H + HULL_H, 56, BARREL_HW, BARREL_H / 2, tco, tsi, COL_BARR[t]);
    }
  }
  return k;
}

/* the translucent FX: laser beams (glow + core) and destruction bursts, all drawn
 * as cubes in the second pass — depth-tested, not depth-written, painter-sorted. */
static uint32_t emit_fx(const World* w, Inst* out, uint32_t k, uint32_t screen, int ox, int oy) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (!w->tank_tracer[t]) continue;
    int lx, ly;
    int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
    if (!local_of(screen, ox, oy, tx, ty, &lx, &ly)) continue;
    uint32_t ti = w->tank_turret[t] >> ANGLE_SHIFT; int tco = dir_cos(ti), tsi = dir_sin(ti);
    int scx = wc_x(w->tank_shot_cell[t]) * SUB + SUB / 2, scy = wc_y(w->tank_shot_cell[t]) * SUB + SUB / 2;
    int ddx = scx - tx; if (ddx >  ARENA_W_SUB / 2) ddx -= ARENA_W_SUB; if (ddx < -ARENA_W_SUB / 2) ddx += ARENA_W_SUB;
    int ddy = scy - ty; if (ddy >  ARENA_H_SUB / 2) ddy -= ARENA_H_SUB; if (ddy < -ARENA_H_SUB / 2) ddy += ARENA_H_SUB;
    int len = (ddx * tco + ddy * tsi) >> TRIG_SHIFT;   /* beam length along the turret to the wall */
    if (len <= 0) continue;
    int half = len / 2;
    int mx = lx + ((tco * half) >> TRIG_SHIFT), my = ly + ((tsi * half) >> TRIG_SHIFT);
    int cz = FLOOR_H + HULL_H;                          /* at turret height */
    k = push(out, k, mx, my, cz, half, 16, LASER_H / 2, tco, tsi, COL_LASER_GLOW);
    k = push(out, k, mx, my, cz, half,  5, LASER_H / 4, tco, tsi, COL_LASER_CORE);
  }
  for (uint32_t i = 0; i < N_FX; i++) {
    if (!w->fx_t[i]) continue;
    int lx, ly;
    if (!local_of(screen, ox, oy, xy_lo(w->fx_xy[i]), xy_hi(w->fx_xy[i]), &lx, &ly)) continue;
    int age = FX_DURATION - (int)w->fx_t[i];            /* 0 (fresh) .. FX_DURATION-1 */
    int half = 22 + age * 9;                            /* expands as it fades */
    k = push(out, k, lx, ly, FLOOR_H + half, half, half, half, 16384, 0, burst_colour(age));
  }
  return k;
}

/* painter-sort the translucent range back-to-front (ascending iso_depth, so the
 * farthest blends first). Few FX are ever visible, so a simple insertion sort over
 * the small range is cheaper than any structure. */
static void sort_translucent(Inst* a, uint32_t n) {
  for (uint32_t i = 1; i < n; i++) {
    Inst v = a[i]; int32_t vk = iso_depth(v.wx, v.wy, v.wz);
    uint32_t j = i;
    while (j > 0 && iso_depth(a[j - 1].wx, a[j - 1].wy, a[j - 1].wz) > vk) { a[j] = a[j - 1]; j--; }
    a[j] = v;
  }
}

uint32_t build_view(const World* w, Inst* out, DrawList* dl,
                    uint32_t cam_sx, uint32_t cam_sy,
                    int sliding, uint32_t to_sx, uint32_t to_sy, int dx, int dy) {
  struct { uint32_t screen; int ox, oy; } S[2];
  int ns = 1;
  S[0].screen = cam_sy * SCREENS_X + cam_sx; S[0].ox = 0; S[0].oy = 0;
  if (sliding) {
    S[1].screen = to_sy * SCREENS_X + to_sx;
    S[1].ox = dx * GRID_W * SUB; S[1].oy = dy * GRID_H * SUB; ns = 2;
  }

  uint32_t k = 0, base;
  base = k; for (int s = 0; s < ns; s++) k = emit_terrain(w, out, k, 0, S[s].screen, S[s].ox, S[s].oy); dl->opaque[K_FLOOR]  = k - base;
  base = k; for (int s = 0; s < ns; s++) k = emit_terrain(w, out, k, 1, S[s].screen, S[s].ox, S[s].oy); dl->opaque[K_WALL]   = k - base;
  base = k; for (int s = 0; s < ns; s++) k = emit_nests(w, out, k, S[s].screen, S[s].ox, S[s].oy);       dl->opaque[K_NEST]   = k - base;
  base = k; for (int s = 0; s < ns; s++) k = emit_mites(w, out, k, S[s].screen, S[s].ox, S[s].oy);       dl->opaque[K_MITE]   = k - base;
  base = k; for (int s = 0; s < ns; s++) k = emit_tank_part(w, out, k, K_HULL,   S[s].screen, S[s].ox, S[s].oy); dl->opaque[K_HULL]   = k - base;
  base = k; for (int s = 0; s < ns; s++) k = emit_tank_part(w, out, k, K_TURRET, S[s].screen, S[s].ox, S[s].oy); dl->opaque[K_TURRET] = k - base;
  base = k; for (int s = 0; s < ns; s++) k = emit_tank_part(w, out, k, K_BARREL, S[s].screen, S[s].ox, S[s].oy); dl->opaque[K_BARREL] = k - base;

  uint32_t opaque_end = k;
  for (int s = 0; s < ns; s++) k = emit_fx(w, out, k, S[s].screen, S[s].ox, S[s].oy);
  dl->translucent = k - opaque_end;
  sort_translucent(out + opaque_end, dl->translucent);
  return k;
}
