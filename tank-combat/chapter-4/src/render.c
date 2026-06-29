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

/* per-tank identity colours: hull/turret, the darker barrel, and the lighter path */
static const uint32_t COL_BODY[N_TANKS] = {
  RGBA(242, 158, 41, 255), RGBA(77, 179, 230, 255), RGBA(120, 205, 120, 255), RGBA(196, 140, 235, 255) };
/* (the tank's darker parts — tracks/wheels — now come from the baked mesh's per-material
 * grey, multiplied by the body tint above, so a separate barrel colour is no longer needed) */
static const uint32_t COL_PATH[N_TANKS] = {   /* a lighter, translucent shade of the body colour */
  RGBA(255, 205, 120, 205), RGBA(150, 215, 250, 205), RGBA(180, 240, 180, 205), RGBA(225, 190, 250, 205) };
static const uint32_t COL_HOVER    = RGBA(255, 255, 255, 140);   /* translucent cursor cell */

/* a mite's tint shows its CURRENT role (re-derived every tick, never stale):
 * idle teal (wander) -> hunting red (hot, fading cold as the sighting ages) ->
 * homing in its NEST's colour (the 20% relaying a record back to a nest). */
static const uint32_t COL_MITE_IDLE      = RGBA( 96, 150, 138, 255);
static const uint32_t COL_MITE_HUNT_HOT  = RGBA(240,  92,  60, 255);
static const uint32_t COL_MITE_HUNT_COLD = RGBA(150,  64,  74, 255);
/* the nests (NEST_COUNT of them on the 8x8 map), a full hue wheel — one distinct tint per
 * nest (matches NESTC in app.js). S=(230-110)/230, V=230/255, hues evenly spaced. Shared
 * (declared in render.h): the mite homing tint here + the static landmark tint in staticmap.c. */
const uint32_t COL_NEST[NEST_COUNT] = {
  RGBA(230, 146, 110, 255), RGBA(230, 157, 110, 255), RGBA(230, 169, 110, 255), RGBA(230, 180, 110, 255),
  RGBA(230, 192, 110, 255), RGBA(230, 203, 110, 255), RGBA(230, 215, 110, 255), RGBA(230, 226, 110, 255),
  RGBA(223, 230, 110, 255), RGBA(211, 230, 110, 255), RGBA(200, 230, 110, 255), RGBA(188, 230, 110, 255),
  RGBA(177, 230, 110, 255), RGBA(165, 230, 110, 255), RGBA(154, 230, 110, 255), RGBA(143, 230, 110, 255),
  RGBA(131, 230, 110, 255), RGBA(120, 230, 110, 255), RGBA(110, 230, 112, 255), RGBA(110, 230, 123, 255),
  RGBA(110, 230, 135, 255), RGBA(110, 230, 146, 255), RGBA(110, 230, 157, 255), RGBA(110, 230, 169, 255),
  RGBA(110, 230, 180, 255), RGBA(110, 230, 192, 255), RGBA(110, 230, 203, 255), RGBA(110, 230, 215, 255),
  RGBA(110, 230, 226, 255), RGBA(110, 223, 230, 255), RGBA(110, 211, 230, 255), RGBA(110, 200, 230, 255),
  RGBA(110, 188, 230, 255), RGBA(110, 177, 230, 255), RGBA(110, 165, 230, 255), RGBA(110, 154, 230, 255),
  RGBA(110, 143, 230, 255), RGBA(110, 131, 230, 255), RGBA(110, 120, 230, 255), RGBA(112, 110, 230, 255),
  RGBA(123, 110, 230, 255), RGBA(135, 110, 230, 255), RGBA(146, 110, 230, 255), RGBA(157, 110, 230, 255),
  RGBA(169, 110, 230, 255), RGBA(180, 110, 230, 255), RGBA(192, 110, 230, 255), RGBA(203, 110, 230, 255),
  RGBA(215, 110, 230, 255), RGBA(226, 110, 230, 255), RGBA(230, 110, 223, 255), RGBA(230, 110, 211, 255),
  RGBA(230, 110, 200, 255), RGBA(230, 110, 188, 255), RGBA(230, 110, 177, 255), RGBA(230, 110, 165, 255),
  RGBA(230, 110, 154, 255), RGBA(230, 110, 143, 255), RGBA(230, 110, 131, 255), RGBA(230, 110, 120, 255),
  RGBA(230, 112, 110, 255), RGBA(230, 123, 110, 255), RGBA(230, 135, 110, 255) };
static const uint32_t COL_BOLT_GLOW = RGBA(255,  96,  64, 170);   /* the bolt's hot trailing streak */
static const uint32_t COL_BOLT_CORE = RGBA(255, 244, 224, 255);   /* its bright leading head */
#define MITE_AGE_HOT 150   /* frames a sighting stays "hot" in the hunt tint */

/* per-kind heights + footprints, in subcells (render-only — z lives nowhere in the
 * sim). The terrain and nests are no longer here — they are static town map data
 * (staticmap.c). FLOOR_H is kept as the ground plane the dynamic things sit on: tanks
 * and mites sit on the floor; the overlays sit just above it (a hair, to avoid
 * z-fighting) or rise as a beacon/spike. */
#define FLOOR_H    (SUB / 8)        /* 32: the ground plane the dynamic things sit on */
#define MITE_H     (SUB / 3)        /* ~85 */
#define HULL_H     (SUB / 2)        /* 128 */
#define TURRET_H   (SUB / 4)        /* 64, on top of the hull */
#define BARREL_H   (SUB / 6)        /* ~42 */
#define BOLT_H     (SUB / 8)        /* 32: a thin bolt */
#define MITE_HALF  40
#define TANK_H     120              /* tank half-extent (subcells): the baked tank mesh's shared unit box scales by this */
#define HULL_HX    87
#define HULL_HY    67
#define TURRET_HALF 42
#define BARREL_HW  16               /* barrel half-width */
/* interaction overlays */
#define TILE_LIFT  6                /* lift overlay tiles above the floor (no z-fight) */
#define ARROW_HALF 56               /* the selected tank's hovering down-arrow marker: width */
#define ARROW_H    104              /* its height (the apex points DOWN at the tank) */
#define ARROW_Z    150              /* apex height above the floor — just over the tank, in its colour */
#define DEST_H     (2 * SUB)        /* a tall destination beacon */
#define DEST_HALF  26
#define PATH_HALF  (SUB / 2 - 22)   /* a routed-path tile, inset from the cell */
#define HOVER_HALF (SUB / 2 - 6)    /* the cursor-cell highlight, near cell-sized */
#define VIS_MARGIN (2 * SUB)        /* grow the visible box so edge walls/units don't pop */

static uint32_t burst_colour(int age) {     /* a mite popping: a white-gold flash -> transparent */
  int n = FX_DURATION - 1; if (n < 1) n = 1; if (age > n) age = n;
  int a = 230 + (0 - 230) * age / n;
  return RGBA(255, 232, 150, a);
}
static uint32_t burst_colour_impact(int age) {  /* a bolt splashing on a wall: a hot orange flash */
  int n = FX_DURATION - 1; if (n < 1) n = 1; if (age > n) age = n;
  int a = 230 + (0 - 230) * age / n;
  return RGBA(255, 170, 78, a);
}
static uint32_t mite_colour(const World* w, uint32_t m) {
  uint8_t mode = w->mite_mode[m];
  if (mode == MM_HOME) return COL_NEST[nest_of(m)];
  if (mode == MM_HUNT)
    return (w->frame - w->mite_rec_time[w->rec_buf][m]) < MITE_AGE_HOT ? COL_MITE_HUNT_HOT : COL_MITE_HUNT_COLD;
  return COL_MITE_IDLE;
}

/* ---- render-time interpolation (presentation-only) ---------------------------------
 * The sim runs at a fixed TICK_DT; a free-running display beats against it, so a thing
 * that moves a constant amount per tick lands on a slightly different screen pixel each
 * frame — visible judder even at a rock-steady frame rate. The cure is render-side: hold
 * the PREVIOUS tick's positions alongside the current ones and draw each moving thing at
 * lerp(prev, cur, alpha), alpha = (time since the last tick) / TICK_DT in [0,1). The
 * display then glides between ticks (one tick of latency, no sim change).
 *
 * g_alpha_q8 = 0 (the DEFAULT) means "no interpolation": every read returns the current
 * position byte-for-byte, so build_view stays identical to the un-interpolated emit — the
 * determinism tests and the sim hash are untouched. The host opts in per frame by setting
 * a non-zero alpha + the prev-position pointers (render_set_interp). Positions only; angles
 * snap (a tick's worth of turn is small, and wrap-safe angle lerp isn't worth the risk).
 *
 * A TELEPORT GUARD snaps to the current position when a thing jumps more than INTERP_MAX
 * subcells in one tick — a respawn, a toroidal wrap, or a slot reused by a new entity —
 * so it never streaks across the map. INTERP_MAX (8 cells) sits well above any legit
 * one-tick move of a tank or mite and below any wrap/respawn jump (those cross the arena). */
#define INTERP_MAX (8 * SUB)         /* 2048: beyond this in a tick, treat as a teleport and snap */
static uint32_t       g_alpha_q8     = 0;   /* 0..255 lerp weight toward the current tick (0 = off) */
static const uint32_t* g_prev_tank_xy = 0;
static const uint32_t* g_prev_mite_xy = 0;
static const uint32_t* g_prev_proj_xy = 0;
/* the bursts (fx_xy) are NOT interpolated: a burst is pinned to the spot a mite died / a
 * bolt struck and only EXPANDS over its life, so its position never moves between ticks. */

void render_set_interp(uint32_t alpha_q8, const uint32_t* prev_tank_xy,
                       const uint32_t* prev_mite_xy, const uint32_t* prev_proj_xy) {
  g_alpha_q8 = alpha_q8 > 255 ? 255 : alpha_q8;
  g_prev_tank_xy = prev_tank_xy; g_prev_mite_xy = prev_mite_xy; g_prev_proj_xy = prev_proj_xy;
}

/* PER-MITE DISTANCE LOD state — the camera eye (subcells) + the detail radius (subcells) and
 * the prefix cap. near_d 0 (the default) means "no detail bucket": emit_mites then emits the
 * whole swarm into the far range, byte-identical to the un-LOD'd emit. */
static int32_t  g_eye_x = 0, g_eye_y = 0, g_eye_z = 0;
static int64_t  g_mite_near_d2 = 0;      /* squared detail radius (subcells^2); 0 disables the detail bucket */
static uint32_t g_mite_near_cap = 0;     /* max detailed mites (triangle-budget guard) */
void render_set_lod(int32_t eye_x, int32_t eye_y, int32_t eye_z,
                    uint32_t near_d, uint32_t near_cap) {
  g_eye_x = eye_x; g_eye_y = eye_y; g_eye_z = eye_z;
  g_mite_near_d2 = (int64_t)near_d * (int64_t)near_d;
  g_mite_near_cap = near_cap;
}

/* unpack `cur` to subcell x,y, interpolated from prev_arr[idx] by g_alpha_q8 when enabled
 * (and not a teleport). With g_alpha_q8 == 0 or a null prev table this is just xy_lo/xy_hi. */
static void interp_xy(uint32_t cur, const uint32_t* prev_arr, uint32_t idx, int* ox, int* oy) {
  int cx = xy_lo(cur), cy = xy_hi(cur);
  if (g_alpha_q8 == 0 || !prev_arr) { *ox = cx; *oy = cy; return; }
  uint32_t p = prev_arr[idx];
  int px = xy_lo(p), py = xy_hi(p), dx = cx - px, dy = cy - py;
  if (dx > INTERP_MAX || dx < -INTERP_MAX || dy > INTERP_MAX || dy < -INTERP_MAX) {
    *ox = cx; *oy = cy; return;                       /* teleport (respawn / wrap / reused slot): snap */
  }
  *ox = px + ((dx * (int)g_alpha_q8) >> 8);
  *oy = py + ((dy * (int)g_alpha_q8) >> 8);
}

/* emit one box (centre, half-extent, facing, tint) into the kind-grouped buffer */
static uint32_t push(Inst* out, uint32_t k, int cx, int cy, int cz,
                     int hx, int hy, int hz, int co, int si, uint32_t rgba) {
  if (k >= INST_MAX) return k;
  Inst* o = &out[k];
  /* wx,wy are int32 — the 8x8 arena reaches 40960 subcells, past int16. Store them
   * WITHOUT a 16-bit cast (the old (int16_t) truncation wrapped the east columns to
   * large negatives, drawing them as a displaced strip far to the west). The remaining
   * fields are int16 and small (heights/half-extents in subcells, facing in Q14). */
  o->wx = (int32_t)cx; o->wy = (int32_t)cy; o->wz = (int16_t)cz; o->hz = (int16_t)hz;
  o->hx = (int16_t)hx; o->hy = (int16_t)hy; o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}

/* the visible world-space box (subcells), grown by the margin; what to emit */
typedef struct { int x0, y0, x1, y1; } Box;
static int in_box(const Box* b, int px, int py) { return px >= b->x0 && px <= b->x1 && py >= b->y0 && py <= b->y1; }
static int cell_in_box(const Box* b, uint32_t cell) {
  return in_box(b, wc_x(cell) * SUB + SUB / 2, wc_y(cell) * SUB + SUB / 2);
}

/* ---- per-kind emitters: each appends its kind's instances inside the box --------
 * The terrain (floors/walls) and the nests are gone from here: they are STATIC TOWN
 * map data now (build_static_map in staticmap.c), placed once and drawn from the
 * uploaded-once static buffer. What follows emits only the DYNAMIC things. */

/* the selected tank's selection mark — a single down-arrow hovering above the tank in the
 * tank's OWN identity colour, apex pointing down at it. UNSELECTED tanks draw none, so this
 * IS the selection indicator; there is no ground ring/block (it would sit on the grass/road
 * and clutter the floor), and the arrow reads from anywhere in the connected world. */
static uint32_t emit_rings(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    if (w->tstate[t] == TS_UNSELECTED) continue;
    int px, py; interp_xy(w->tank_xy[t], g_prev_tank_xy, t, &px, &py);
    if (!in_box(b, px, py)) continue;
    /* a single down-arrow hovering above the tank, in the tank's OWN identity colour: its apex
     * points down at the tank, and that IS the selection mark — no ground ring/block any more. */
    k = push(out, k, px, py, FLOOR_H + ARROW_Z + ARROW_H / 2, ARROW_HALF, ARROW_HALF, ARROW_H / 2,
             16384, 0, COL_BODY[t]);
  }
  return k;
}

static uint32_t push_mite(const World* w, Inst* out, uint32_t k, uint32_t m, int px, int py) {
  uint32_t di = w->mite_ang[m] >> ANGLE_SHIFT;
  return push(out, k, px, py, FLOOR_H + MITE_H / 2, MITE_HALF, MITE_HALF, MITE_H / 2,
              dir_cos(di), dir_sin(di), mite_colour(w, m));
}
/* a visible mite is "near" if its 3-D distance to the eye is within the detail radius */
static int mite_near(int px, int py) {
  int64_t dx = px - g_eye_x, dy = py - g_eye_y, dz = (int64_t)(FLOOR_H + MITE_H / 2) - g_eye_z;
  return dx * dx + dy * dy + dz * dz <= g_mite_near_d2;
}
/* PER-MITE DISTANCE LOD: emit the mites NEAR the eye first (3-D distance <= the detail radius),
 * capped at g_mite_near_cap for the triangle budget, then the rest. dl->mite_near records the
 * size of that detailed prefix so the host binds the crab to it and the spike to the tail.
 * Pass 1 takes the first near_n qualifiers in index order; pass 2 re-counts qualifiers and
 * skips exactly those first near_n so nothing is double-emitted. With the detail radius 0 (the
 * default / native tests) there are no qualifiers: pass 1 is empty and the whole swarm lands in
 * the far range, byte-identical to the old single-pass emit. */
static uint32_t emit_mites(const World* w, Inst* out, uint32_t k, const Box* b, uint32_t* near_out) {
  uint32_t near_n = 0;
  if (g_mite_near_d2 > 0) {                            /* pass 1: the close mites (detailed) */
    for (uint32_t m = 0; m < N_MITES && near_n < g_mite_near_cap; m++) {
      if (w->mite_resp[m]) continue;
      int px, py; interp_xy(w->mite_xy[m], g_prev_mite_xy, m, &px, &py);
      if (!in_box(b, px, py) || !mite_near(px, py)) continue;
      k = push_mite(w, out, k, m, px, py); near_n++;
    }
  }
  uint32_t q = 0;                                       /* qualifiers seen in index order */
  for (uint32_t m = 0; m < N_MITES; m++) {             /* pass 2: the far mites (spike) */
    if (w->mite_resp[m]) continue;                     /* dead mites aren't drawn (or indexed) */
    int px, py; interp_xy(w->mite_xy[m], g_prev_mite_xy, m, &px, &py);
    if (!in_box(b, px, py)) continue;
    if (g_mite_near_d2 > 0 && mite_near(px, py) && q++ < near_n) continue;  /* taken by pass 1 */
    k = push_mite(w, out, k, m, px, py);
  }
  if (near_out) *near_out = near_n;
  return k;
}

/* The tank is two meshes authored in ONE shared unit-box frame (M_HULL = body, M_TURRET =
 * dome). The renderer draws BOTH at the same centre + the same uniform half-extent and rests
 * them on the floor (the mesh anchors the tank bottom at z = -1); only the rotation differs —
 * the hull spins with the chassis heading, the turret with the aim — so they stay registered
 * as one tank while the turret tracks independently. K_BARREL emits nothing (no separate gun
 * mesh in the placeholder). */
static uint32_t emit_tank_part(const World* w, Inst* out, uint32_t k, int part, const Box* b) {
  if (part == K_BARREL) return k;                       /* the gun is part of the turret mesh now */
  for (uint32_t t = 0; t < N_TANKS; t++) {
    int px, py; interp_xy(w->tank_xy[t], g_prev_tank_xy, t, &px, &py);
    if (!in_box(b, px, py)) continue;
    if (part == K_HULL) {
      uint32_t bi = w->tank_ang[t] >> ANGLE_SHIFT;                            /* chassis on the 32-dir table */
      k = push(out, k, px, py, FLOOR_H + TANK_H, TANK_H, TANK_H, TANK_H, dir_cos(bi), dir_sin(bi), COL_BODY[t]);
    } else { /* K_TURRET (turret + gun) — same centre + scale as the hull, spun by the fine aim */
      int tco = fine_cos(w->tank_turret[t]), tsi = fine_sin(w->tank_turret[t]);
      k = push(out, k, px, py, FLOOR_H + TANK_H, TANK_H, TANK_H, TANK_H, tco, tsi, COL_BODY[t]);
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

/* the translucent FX: piercing bolts in flight (a hot streak + a bright head) and
 * destruction bursts, all drawn as cubes in the second pass — depth-tested, not
 * depth-written, painter-sorted. */
static uint32_t emit_fx(const World* w, Inst* out, uint32_t k, const Box* b) {
  for (uint32_t bb = 0; bb < N_TANKS * PROJ_MAX; bb++) {   /* up to PROJ_MAX live bolts per tank */
    if (!w->tank_proj_live[bb]) continue;
    /* the bolt spawns at the tank centre, so its first stretch is still inside the barrel;
     * only the length the head has flown BEYOND the barrel tip (87 + 56 subcells) is drawn,
     * the streak's tail clipped there. While still in the barrel, the bolt isn't drawn. */
    int beyond = (int)w->tank_proj_dist[bb] - (87 + 56);
    if (beyond <= 0) continue;
    int bx, by; interp_xy(w->tank_proj_xy[bb], g_prev_proj_xy, bb, &bx, &by);
    if (!in_box(b, bx, by)) continue;
    int co = fine_cos(w->tank_proj_dir[bb]), si = fine_sin(w->tank_proj_dir[bb]);  /* bolt at the fine turret angle */
    int seg = w->proj_speed; if (seg > beyond) seg = beyond;   /* clip the tail at the barrel tip */
    int half = seg / 2;                                        /* the streak trails back over the swept segment */
    int mx = bx - ((co * half) >> TRIG_SHIFT), my = by - ((si * half) >> TRIG_SHIFT);
    int cz = FLOOR_H + HULL_H;                                 /* the bolt flies at turret height */
    k = push(out, k, mx, my, cz, half, 12, BOLT_H / 2, co, si, COL_BOLT_GLOW);   /* trailing glow streak */
    k = push(out, k, bx, by, cz, 24,    9, BOLT_H / 4, co, si, COL_BOLT_CORE);   /* bright leading head */
  }
  for (uint32_t i = 0; i < N_FX; i++) {
    if (!w->fx_t[i]) continue;
    int px = xy_lo(w->fx_xy[i]), py = xy_hi(w->fx_xy[i]);
    if (!in_box(b, px, py)) continue;
    int age = FX_DURATION - (int)w->fx_t[i];            /* 0 (fresh) .. FX_DURATION-1 */
    int half = 22 + age * 9;                            /* expands as it fades */
    /* white-gold where a mite died, hot orange where a bolt struck a wall */
    uint32_t bcol = w->fx_kind[i] == FX_IMPACT ? burst_colour_impact(age) : burst_colour(age);
    k = push(out, k, px, py, FLOOR_H + half, half, half, half, 16384, 0, bcol);
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
  base = k; k = emit_rings(w, out, k, &b);             dl->opaque[K_RING]   = k - base;
  base = k; k = emit_mites(w, out, k, &b, &dl->mite_near); dl->opaque[K_MITE] = k - base;
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

/* ---- the deferred point lights (one per mite / per FX) ------------------------------
 * A light is an Inst used as a light VOLUME: a cube of half-extent = radius centred on the
 * light, rgba = colour, ALPHA = intensity (the shader scales it up into HDR). The host draws
 * the volume additively and reads the G-buffer inside it, so each light pays only for the
 * pixels it covers. Radii in subcells; SUB = 256 = one cell. */
#define MITE_LIGHT_R   300         /* a faint ~1-cell glow under each mite */
#define BURST_LIGHT_R  720         /* an explosion floods a few cells */
#define BOLT_LIGHT_R   340         /* the bolt's travelling hot point */

static uint32_t push_light(Inst* out, uint32_t k, int cx, int cy, int cz, int radius, uint32_t rgba) {
  if (k >= LIGHT_MAX) return k;
  Inst* o = &out[k];
  o->wx = (int32_t)cx; o->wy = (int32_t)cy; o->wz = (int16_t)cz; o->hz = (int16_t)radius;
  o->hx = (int16_t)radius; o->hy = (int16_t)radius; o->co = 16384; o->si = 0; o->rgba = rgba;   /* axis-aligned cube */
  return k + 1;
}
/* pack a colour's RGB with an intensity into the alpha byte (the light shader reads a as the
 * HDR intensity scale, so a bright FX can push the lit pixel well past 1.0). */
static uint32_t light_rgba(uint32_t col, int intensity) {
  if (intensity < 0) intensity = 0; if (intensity > 255) intensity = 255;
  return (col & 0x00FFFFFFu) | ((uint32_t)intensity << 24);
}

uint32_t build_lights(const World* w, Inst* out, int32_t wx0, int32_t wy0, int32_t wx1, int32_t wy1) {
  Box b;                                           /* the visible box, grown + clamped (as build_view) */
  b.x0 = wx0 - VIS_MARGIN; if (b.x0 < 0) b.x0 = 0;
  b.y0 = wy0 - VIS_MARGIN; if (b.y0 < 0) b.y0 = 0;
  b.x1 = wx1 + VIS_MARGIN; if (b.x1 > ARENA_W_SUB - 1) b.x1 = ARENA_W_SUB - 1;
  b.y1 = wy1 + VIS_MARGIN; if (b.y1 > ARENA_H_SUB - 1) b.y1 = ARENA_H_SUB - 1;

  uint32_t k = 0;
  /* a faint light per visible mite, in its CURRENT role colour — hunters burn a little hotter */
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;                                 /* dead mites cast no light */
    int px, py; interp_xy(w->mite_xy[m], g_prev_mite_xy, m, &px, &py);
    if (!in_box(&b, px, py)) continue;
    uint8_t mode = w->mite_mode[m];
    int inten = mode == MM_HUNT ? 72 : mode == MM_HOME ? 56 : 38;
    k = push_light(out, k, px, py, FLOOR_H + MITE_H / 2, MITE_LIGHT_R, light_rgba(mite_colour(w, m), inten));
  }
  /* a bright, fading light per visible burst (a mite popping / a bolt striking a wall) */
  for (uint32_t i = 0; i < N_FX; i++) {
    if (!w->fx_t[i]) continue;
    int px = xy_lo(w->fx_xy[i]), py = xy_hi(w->fx_xy[i]);
    if (!in_box(&b, px, py)) continue;
    int n = FX_DURATION > 1 ? FX_DURATION - 1 : 1, age = FX_DURATION - (int)w->fx_t[i];   /* 0 fresh .. n old */
    int inten = 235 - age * 215 / n;                                /* a hot flash that decays */
    uint32_t col = w->fx_kind[i] == FX_IMPACT ? RGBA(255, 150, 60, 0) : RGBA(255, 226, 150, 0);
    k = push_light(out, k, px, py, FLOOR_H + 48, BURST_LIGHT_R, light_rgba(col, inten));
  }
  /* a travelling hot point light per bolt in flight (only the part beyond the barrel tip) */
  for (uint32_t bb = 0; bb < N_TANKS * PROJ_MAX; bb++) {
    if (!w->tank_proj_live[bb]) continue;
    if ((int)w->tank_proj_dist[bb] - (87 + 56) <= 0) continue;
    int bx, by; interp_xy(w->tank_proj_xy[bb], g_prev_proj_xy, bb, &bx, &by);
    if (!in_box(&b, bx, by)) continue;
    k = push_light(out, k, bx, by, FLOOR_H + HULL_H, BOLT_LIGHT_R, light_rgba(RGBA(255, 120, 70, 0), 150));
  }
  return k;
}
