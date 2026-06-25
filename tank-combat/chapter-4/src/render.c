/* render.c — chapter 4: read the frozen World, emit 3-D world placements for a
 * TOP-DOWN PERSPECTIVE view. Every instance is a box (centre + half-extent + facing + tint) at
 * its WORLD position (anchor = the world origin), so all sixteen screens form one
 * connected map; the vertex shader projects it (a perspective MVP) and resolves occlusion with a
 * z-buffer. This file emits PLACEMENTS only — it never bakes a screen position and
 * never writes the sim. Instances are grouped by kind so the host draws one range per
 * kind (binding the placeholder cube, or a baked mesh).
 *
 * The host passes the visible world-space box; render emits only the cells and the
 * tanks/mites/nests/FX inside it (+ a small margin), so the cost scales with what the
 * free pan/zoom camera SHOWS — the whole world when zoomed out, a few cells when
 * zoomed in. It also emits the render-only interaction overlays (hover cell, the
 * selected tank's state mark, its destination beacon, the routed path), all derived
 * from the sim or the host's one hover cell — none of it feeds back into the model. */
#include "render.h"
#include "view.h"
#include "dirtab.h"
#include "collide.h"
#include "edge_paths.h"

#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const uint32_t COL_FLOOR = RGBA(44, 48, 58, 255);
static const uint32_t COL_WALL  = RGBA(96, 102, 120, 255);
/* per-tank identity colours: hull/turret, the darker barrel, and the lighter path */
static const uint32_t COL_BODY[N_TANKS] = {
  RGBA(242, 158, 41, 255), RGBA(77, 179, 230, 255), RGBA(120, 205, 120, 255), RGBA(196, 140, 235, 255) };
static const uint32_t COL_BARR[N_TANKS] = {
  RGBA(150, 98, 26, 255), RGBA(48, 110, 142, 255), RGBA(74, 127, 74, 255), RGBA(121, 86, 145, 255) };
static const uint32_t COL_PATH[N_TANKS] = {   /* a lighter, translucent shade of the body colour */
  RGBA(255, 205, 120, 205), RGBA(150, 215, 250, 205), RGBA(180, 240, 180, 205), RGBA(225, 190, 250, 205) };
/* the selected tank's state highlight (UNSELECTED draws none), and the cursor highlight */
static const uint32_t COL_AUTOPATH = RGBA(120, 235, 140, 255);
static const uint32_t COL_MANUAL   = RGBA(245, 225,  90, 255);
static const uint32_t COL_HOVER    = RGBA(255, 255, 255, 140);   /* translucent cursor cell */

/* a mite's tint shows its CURRENT role (re-derived every tick, never stale):
 * idle teal (wander) -> hunting red (hot, fading cold as the sighting ages) ->
 * homing in its NEST's colour (the 20% relaying a record back to a nest). */
static const uint32_t COL_MITE_IDLE      = RGBA( 96, 150, 138, 255);
static const uint32_t COL_MITE_HUNT_HOT  = RGBA(240,  92,  60, 255);
static const uint32_t COL_MITE_HUNT_COLD = RGBA(150,  64,  74, 255);
/* the nests (one per screen but the tank start), a hue wheel (matches NESTC in app.js) */
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
 * and mites sit on the floor; a nest is a tall pylon; the overlays sit just above
 * the floor (a hair, to avoid z-fighting) or rise as a beacon/spike. */
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
/* interaction overlays */
#define TILE_LIFT  6                /* lift overlay tiles above the floor (no z-fight) */
#define RING_H     (SUB / 7)        /* the selected tank's state ring (a low bright base) */
#define RING_HALF  130              /* clearly larger than the hull footprint */
#define MARK_HALF  34               /* the state spike rising above the tank (diamond section) */
#define MARK_H     (SUB + SUB / 5)  /* ~300: tall enough to spot above the swarm */
#define DEST_H     (2 * SUB)        /* a tall destination beacon */
#define DEST_HALF  26
#define PATH_HALF  (SUB / 2 - 22)   /* a routed-path tile, inset from the cell */
#define HOVER_HALF (SUB / 2 - 6)    /* the cursor-cell highlight, near cell-sized */
#define VIS_MARGIN (2 * SUB)        /* grow the visible box so edge walls/units don't pop */

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

/* the visible world-space box (subcells), grown by the margin; what to emit */
typedef struct { int x0, y0, x1, y1; } Box;
static int in_box(const Box* b, int px, int py) { return px >= b->x0 && px <= b->x1 && py >= b->y0 && py <= b->y1; }
static int cell_in_box(const Box* b, uint32_t cell) {
  return in_box(b, wc_x(cell) * SUB + SUB / 2, wc_y(cell) * SUB + SUB / 2);
}

/* ---- per-kind emitters: each appends its kind's instances inside the box -------- */

static uint32_t emit_terrain(const World* w, Inst* out, uint32_t k, int want_wall, const Box* b) {
  int cx0 = b->x0 >> SUB_SHIFT, cx1 = b->x1 >> SUB_SHIFT;
  int cy0 = b->y0 >> SUB_SHIFT, cy1 = b->y1 >> SUB_SHIFT;
  if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
  if (cx1 > BIG_W - 1) cx1 = BIG_W - 1; if (cy1 > BIG_H - 1) cy1 = BIG_H - 1;
  for (int cy = cy0; cy <= cy1; cy++)
    for (int cx = cx0; cx <= cx1; cx++) {
      int wall = cell_is_wall(w->grid, cx, cy);
      if (wall != want_wall) continue;
      int px = cx * SUB + SUB / 2, py = cy * SUB + SUB / 2;
      if (want_wall) k = push(out, k, px, py, WALL_H / 2, WALL_HALF, WALL_HALF, WALL_H / 2, 16384, 0, COL_WALL);
      else           k = push(out, k, px, py, FLOOR_H / 2, FLOOR_HALF, FLOOR_HALF, FLOOR_H / 2, 16384, 0, COL_FLOOR);
    }
  return k;
}

static uint32_t emit_nests(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t n = 0; n < NEST_COUNT; n++) {
    if (!cell_in_box(b, w->nest_cell[n])) continue;
    int px = wc_x(w->nest_cell[n]) * SUB + SUB / 2, py = wc_y(w->nest_cell[n]) * SUB + SUB / 2;
    k = push(out, k, px, py, NEST_H / 2, NEST_HALF, NEST_HALF, NEST_H / 2, 16384, 0, COL_NEST[n]);
  }
  return k;
}

/* the selected tank's selection highlight — green for AUTOPATH, yellow for MANUAL.
 * UNSELECTED tanks draw none, so this IS the mode indicator: a bright base ring under
 * the tank PLUS a tall diamond spike above it, so the selection (and which mode) reads
 * clearly from anywhere in the connected world. */
static uint32_t emit_rings(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (w->tstate[t] == TS_UNSELECTED) continue;
    int px = xy_lo(w->tank_xy[t]), py = xy_hi(w->tank_xy[t]);
    if (!in_box(b, px, py)) continue;
    uint32_t col = w->tstate[t] == TS_MANUAL ? COL_MANUAL : COL_AUTOPATH;
    k = push(out, k, px, py, FLOOR_H + RING_H / 2, RING_HALF, RING_HALF, RING_H / 2, 16384, 0, col);
    int mz = FLOOR_H + HULL_H + TURRET_H + MARK_H / 2 + 20;               /* a tall diamond spike above */
    k = push(out, k, px, py, mz, MARK_HALF, MARK_HALF, MARK_H / 2, 11585, 11585, col);
  }
  return k;
}

static uint32_t emit_mites(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;                     /* dead mites aren't drawn (or indexed) */
    int px = xy_lo(w->mite_xy[m]), py = xy_hi(w->mite_xy[m]);
    if (!in_box(b, px, py)) continue;
    uint32_t di = w->mite_ang[m] >> ANGLE_SHIFT;
    k = push(out, k, px, py, FLOOR_H + MITE_H / 2, MITE_HALF, MITE_HALF, MITE_H / 2,
             dir_cos(di), dir_sin(di), mite_colour(w, m));
  }
  return k;
}

static uint32_t emit_tank_part(const World* w, Inst* out, uint32_t k, int part, const Box* b) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    int px = xy_lo(w->tank_xy[t]), py = xy_hi(w->tank_xy[t]);
    if (!in_box(b, px, py)) continue;
    uint32_t bi = w->tank_ang[t]    >> ANGLE_SHIFT;
    uint32_t ti = w->tank_turret[t] >> ANGLE_SHIFT;
    int tco = dir_cos(ti), tsi = dir_sin(ti);
    if (part == K_HULL)
      k = push(out, k, px, py, FLOOR_H + HULL_H / 2, HULL_HX, HULL_HY, HULL_H / 2,
               dir_cos(bi), dir_sin(bi), COL_BODY[t]);
    else if (part == K_TURRET)
      k = push(out, k, px, py, FLOOR_H + HULL_H + TURRET_H / 2, TURRET_HALF, TURRET_HALF, TURRET_H / 2,
               tco, tsi, COL_BODY[t]);
    else { /* K_BARREL: a thin box reaching out from the turret along its aim */
      int off = HULL_HX;
      int bx = px + ((tco * off) >> TRIG_SHIFT), by = py + ((tsi * off) >> TRIG_SHIFT);
      k = push(out, k, bx, by, FLOOR_H + HULL_H, 56, BARREL_HW, BARREL_H / 2, tco, tsi, COL_BARR[t]);
    }
  }
  return k;
}

/* a destination beacon — a tall bright box at a routing tank's destination cell, in
 * the tank's colour, so the place you sent it to is obvious. */
static uint32_t emit_dest(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (!w->phas[t]) continue;
    uint32_t cell = wc_pack((int32_t)(w->pdest_screen[t] % SCREENS_X) * GRID_W + (w->pdest_cell[t] % GRID_W),
                            (int32_t)(w->pdest_screen[t] / SCREENS_X) * GRID_H + (w->pdest_cell[t] / GRID_W));
    if (!cell_in_box(b, cell)) continue;
    int px = wc_x(cell) * SUB + SUB / 2, py = wc_y(cell) * SUB + SUB / 2;
    k = push(out, k, px, py, FLOOR_H + DEST_H / 2, DEST_HALF, DEST_HALF, DEST_H / 2, 16384, 0, COL_BODY[t]);
  }
  return k;
}

/* the translucent FX: laser beams (glow + core) and destruction bursts, all drawn
 * as cubes in the second pass — depth-tested, not depth-written, painter-sorted. */
static uint32_t emit_fx(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (!w->tank_tracer[t]) continue;
    int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
    if (!in_box(b, tx, ty)) continue;
    uint32_t ti = w->tank_turret[t] >> ANGLE_SHIFT; int tco = dir_cos(ti), tsi = dir_sin(ti);
    int scx = wc_x(w->tank_shot_cell[t]) * SUB + SUB / 2, scy = wc_y(w->tank_shot_cell[t]) * SUB + SUB / 2;
    int ddx = scx - tx; if (ddx >  ARENA_W_SUB / 2) ddx -= ARENA_W_SUB; if (ddx < -ARENA_W_SUB / 2) ddx += ARENA_W_SUB;
    int ddy = scy - ty; if (ddy >  ARENA_H_SUB / 2) ddy -= ARENA_H_SUB; if (ddy < -ARENA_H_SUB / 2) ddy += ARENA_H_SUB;
    int len = (ddx * tco + ddy * tsi) >> TRIG_SHIFT;   /* beam length along the turret to the wall */
    if (len <= 0) continue;
    int half = len / 2;
    int mx = tx + ((tco * half) >> TRIG_SHIFT), my = ty + ((tsi * half) >> TRIG_SHIFT);
    int cz = FLOOR_H + HULL_H;                          /* at turret height */
    k = push(out, k, mx, my, cz, half, 16, LASER_H / 2, tco, tsi, COL_LASER_GLOW);
    k = push(out, k, mx, my, cz, half,  5, LASER_H / 4, tco, tsi, COL_LASER_CORE);
  }
  for (uint32_t i = 0; i < N_FX; i++) {
    if (!w->fx_t[i]) continue;
    int px = xy_lo(w->fx_xy[i]), py = xy_hi(w->fx_xy[i]);
    if (!in_box(b, px, py)) continue;
    int age = FX_DURATION - (int)w->fx_t[i];            /* 0 (fresh) .. FX_DURATION-1 */
    int half = 22 + age * 9;                            /* expands as it fades */
    k = push(out, k, px, py, FLOOR_H + half, half, half, half, 16384, 0, burst_colour(age));
  }
  return k;
}

/* the routed paths: a translucent tile on each cell a routing tank will cross,
 * read straight out of the same path tables the tank follows (path_trace). Collected
 * once (capped at PATH_MAX) so the emit just places the ones inside the box. */
static uint16_t g_trace[BIG_W * BIG_H];
static uint16_t g_path_cell[PATH_MAX];
static uint32_t g_path_col[PATH_MAX];
static uint32_t g_path_n;
static void collect_paths(const World* w) {
  g_path_n = 0;
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (!w->phas[t] || w->pstatus[t] != PS_ROUTING) continue;
    uint32_t n = path_trace(w, t, g_trace, BIG_W * BIG_H);
    for (uint32_t i = 0; i < n && g_path_n < PATH_MAX; i++) {
      g_path_cell[g_path_n] = g_trace[i]; g_path_col[g_path_n] = COL_PATH[t]; g_path_n++;
    }
  }
}
static uint32_t emit_path(Inst* out, uint32_t k, const Box* b) {
  for (uint32_t i = 0; i < g_path_n; i++) {
    if (!cell_in_box(b, g_path_cell[i])) continue;
    int px = wc_x(g_path_cell[i]) * SUB + SUB / 2, py = wc_y(g_path_cell[i]) * SUB + SUB / 2;
    k = push(out, k, px, py, FLOOR_H + TILE_LIFT, PATH_HALF, PATH_HALF, 3, 16384, 0, g_path_col[i]);
  }
  return k;
}

/* the cursor-cell highlight: one bright translucent tile on the hovered world cell */
static uint32_t emit_hover(Inst* out, uint32_t k, uint32_t hover_cell, const Box* b) {
  if (hover_cell == REC_EMPTY || !cell_in_box(b, hover_cell)) return k;
  int px = wc_x(hover_cell) * SUB + SUB / 2, py = wc_y(hover_cell) * SUB + SUB / 2;
  return push(out, k, px, py, FLOOR_H + TILE_LIFT + 1, HOVER_HALF, HOVER_HALF, 4, 16384, 0, COL_HOVER);
}

/* painter-sort the translucent range back-to-front (ascending view_depth_key, so the
 * farthest blends first). Few translucent instances are visible, so a simple
 * insertion sort over the small range is cheaper than any structure. */
static void sort_translucent(Inst* a, uint32_t n) {
  for (uint32_t i = 1; i < n; i++) {
    Inst v = a[i]; int32_t vk = view_depth_key(v.wx, v.wy, v.wz);
    uint32_t j = i;
    while (j > 0 && view_depth_key(a[j - 1].wx, a[j - 1].wy, a[j - 1].wz) > vk) { a[j] = a[j - 1]; j--; }
    a[j] = v;
  }
}

uint32_t build_view(const World* w, Inst* out, DrawList* dl,
                    int32_t wx0, int32_t wy0, int32_t wx1, int32_t wy1, uint32_t hover_cell) {
  Box b;                                           /* the visible box, grown + clamped to the world */
  b.x0 = wx0 - VIS_MARGIN; if (b.x0 < 0) b.x0 = 0;
  b.y0 = wy0 - VIS_MARGIN; if (b.y0 < 0) b.y0 = 0;
  b.x1 = wx1 + VIS_MARGIN; if (b.x1 > ARENA_W_SUB - 1) b.x1 = ARENA_W_SUB - 1;
  b.y1 = wy1 + VIS_MARGIN; if (b.y1 > ARENA_H_SUB - 1) b.y1 = ARENA_H_SUB - 1;

  collect_paths(w);

  uint32_t k = 0, base;
  base = k; k = emit_terrain(w, out, k, 0, &b);        dl->opaque[K_FLOOR]  = k - base;
  base = k; k = emit_terrain(w, out, k, 1, &b);        dl->opaque[K_WALL]   = k - base;
  base = k; k = emit_nests(w, out, k, &b);             dl->opaque[K_NEST]   = k - base;
  base = k; k = emit_rings(w, out, k, &b);             dl->opaque[K_RING]   = k - base;
  base = k; k = emit_mites(w, out, k, &b);             dl->opaque[K_MITE]   = k - base;
  base = k; k = emit_tank_part(w, out, k, K_HULL,  &b); dl->opaque[K_HULL]   = k - base;
  base = k; k = emit_tank_part(w, out, k, K_TURRET, &b); dl->opaque[K_TURRET] = k - base;
  base = k; k = emit_tank_part(w, out, k, K_BARREL, &b); dl->opaque[K_BARREL] = k - base;
  base = k; k = emit_dest(w, out, k, &b);              dl->opaque[K_DEST]   = k - base;

  uint32_t opaque_end = k;
  k = emit_fx(w, out, k, &b);
  k = emit_path(out, k, &b);
  k = emit_hover(out, k, hover_cell, &b);
  dl->translucent = k - opaque_end;
  sort_translucent(out + opaque_end, dl->translucent);
  return k;
}
