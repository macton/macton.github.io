#include "render.h"
#include "terrain.h"
#include "fxmath.h"

#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

#define COL   32        /* terrain sample spacing, subcells (~0.125 cell)   */
#define GROUND_BOTTOM (VIEW_H / 2 + 2 * SUB)   /* fill extends below the view */

static const uint32_t COL_GROUND     = RGBA( 58,  70,  66, 255);
static const uint32_t COL_GROUND_TOP = RGBA(104, 138, 116, 255);
static const uint32_t COL_BODY       = RGBA(214, 156,  78, 255);
static const uint32_t COL_ABDOMEN    = RGBA(170, 116,  58, 255);
static const uint32_t COL_LEG_NEAR   = RGBA(232, 196, 120, 255);
static const uint32_t COL_LEG_FAR    = RGBA(146, 120,  84, 255);
static const uint32_t COL_JOINT      = RGBA( 36,  40,  48, 255);
static const uint32_t COL_FOOT       = RGBA(245, 236, 200, 255);
static const uint32_t COL_CLAMP      = RGBA(232,  92,  74, 255);  /* foot can't reach */
/* teaching overlay (semi-transparent; the host enables alpha blending) */
static const uint32_t COL_GHOST      = RGBA(108, 160, 214, 150);  /* pre-aim pose    */
static const uint32_t COL_REF        = RGBA(108, 160, 214, 120);  /* reference ray   */
static const uint32_t COL_TARGET     = RGBA(244, 184,  96, 235);  /* the foot target */

static uint32_t push(Inst* out, uint32_t k, int32_t cx, int32_t cy, int32_t hx, int32_t hy,
                     int32_t co, int32_t si, uint32_t rgba) {
  if (k >= INST_MAX) return k;
  Inst* o = &out[k];
  o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->hx = (int16_t)hx; o->hy = (int16_t)hy;
  o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}

/* an oriented quad (a segment/limb) between two camera-relative points, half-width hw */
static uint32_t cap(Inst* out, uint32_t k, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                    int32_t hw, uint32_t rgba) {
  int32_t dx = bx - ax, dy = by - ay;
  uint32_t len = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
  int32_t co = TRIG_ONE, si = 0;
  if (len > 0) { co = (int32_t)((int64_t)dx * TRIG_ONE / len); si = (int32_t)((int64_t)dy * TRIG_ONE / len); }
  return push(out, k, (ax + bx) / 2, (ay + by) / 2, (int32_t)len / 2, hw, co, si, rgba);
}

/* draw one solved leg: its segments, joints, and the foot marker */
static uint32_t draw_leg(const World* w, Inst* out, uint32_t k, int i, int32_t camx, int32_t camy) {
  int near = (i < N_LEGS / 2);
  uint32_t segcol = near ? COL_LEG_NEAR : COL_LEG_FAR;
  int32_t base_hw = near ? 13 : 9;
  const int32_t* JX = &w->joint_x[i * N_JOINT];
  const int32_t* JY = &w->joint_y[i * N_JOINT];

  for (int s = 0; s < N_SEG; s++) {
    int32_t ax = JX[s] - camx,     ay = JY[s] - camy;
    int32_t bx = JX[s + 1] - camx, by = JY[s + 1] - camy;
    k = cap(out, k, ax, ay, bx, by, base_hw - 2 * s, segcol);   /* tapers toward the foot */
  }
  for (int j = 0; j <= N_SEG; j++) {
    int32_t hw = (j == 0) ? base_hw - 1 : base_hw - 4;          /* hip knuckle a bit bigger */
    k = push(out, k, JX[j] - camx, JY[j] - camy, hw, hw, TRIG_ONE, 0, COL_JOINT);
  }
  int32_t fx = JX[N_SEG] - camx, fy = JY[N_SEG] - camy;
  k = push(out, k, fx, fy, base_hw - 3, base_hw - 3, TRIG_ONE, 0,
           w->leg_clamped[i] ? COL_CLAMP : COL_FOOT);
  return k;
}

/* the length/direction teaching overlay for one leg: the pre-aim (length-only)
 * pose recovered by un-rotating the solved chain, plus the target marker. The
 * faint chain is the chain folded to the right reach but NOT yet aimed; the solid
 * chain (drawn elsewhere) is the same chain after the single hip rotation. */
static uint32_t draw_overlay(const World* w, Inst* out, uint32_t k, int i, int32_t camx, int32_t camy) {
  int32_t hx = w->joint_x[i * N_JOINT], hy = w->joint_y[i * N_JOINT];
  int32_t dcos = w->leg_dcos[i], dsin = w->leg_dsin[i];
  int32_t gx[N_JOINT], gy[N_JOINT];
  for (int j = 0; j < N_JOINT; j++) {
    int32_t wx = w->joint_x[i * N_JOINT + j] - hx;
    int32_t wy = w->joint_y[i * N_JOINT + j] - hy;
    /* local = R(-delta) * (world - hip): undo the aim to see the length-only pose */
    int32_t lx = (int32_t)(((int64_t)wx * dcos + (int64_t)wy * dsin) >> 14);
    int32_t ly = (int32_t)((-(int64_t)wx * dsin + (int64_t)wy * dcos) >> 14);
    gx[j] = hx + lx - camx;
    gy[j] = hy + ly - camy;
  }
  for (int s = 0; s < N_SEG; s++) k = cap(out, k, gx[s], gy[s], gx[s + 1], gy[s + 1], 6, COL_GHOST);
  /* reference ray: hip -> pre-aim end (the direction the aim rotates AWAY from) */
  k = cap(out, k, gx[0], gy[0], gx[N_SEG], gy[N_SEG], 3, COL_REF);
  /* the target the foot is solving for, as a diamond (rotated square, ~45 deg) */
  k = push(out, k, w->foot_x[i] - camx, w->foot_y[i] - camy, 15, 15, 11585, 11585, COL_TARGET);
  return k;
}

uint32_t build_view(const World* w, Inst* out, int show_stages, int focus) {
  uint32_t k = 0;
  int32_t camx = w->body_x, camy = CAM_Y;

  /* ---- terrain: fill columns + a lighter surface line ---- */
  int32_t x0 = camx - VIEW_W / 2 - COL, x1 = camx + VIEW_W / 2 + COL;
  int32_t psx = 0, psy = 0; int have = 0;
  for (int32_t x = x0; x <= x1; x += COL) {
    int32_t rx = x - camx;
    int32_t rty = terrain_y(x) - camy;
    k = push(out, k, rx, (rty + GROUND_BOTTOM) / 2, COL / 2 + 2, (GROUND_BOTTOM - rty) / 2,
             TRIG_ONE, 0, COL_GROUND);
    if (have) k = cap(out, k, psx, psy, rx, rty, 7, COL_GROUND_TOP);
    psx = rx; psy = rty; have = 1;
  }

  /* ---- far legs (behind) ---- */
  for (int i = N_LEGS / 2; i < N_LEGS; i++) k = draw_leg(w, out, k, i, camx, camy);

  /* ---- body: abdomen (rear), cephalothorax, a small head bump (forward = +x) ---- */
  int32_t rby = w->body_y - camy;
  k = push(out, k, -84, rby + 8, 66, 52, TRIG_ONE, 0, COL_ABDOMEN);
  k = push(out, k,   0, rby,     56, 40, TRIG_ONE, 0, COL_BODY);
  k = push(out, k,  58, rby - 2, 24, 22, TRIG_ONE, 0, COL_BODY);

  /* ---- near legs (in front) ---- */
  for (int i = 0; i < N_LEGS / 2; i++) k = draw_leg(w, out, k, i, camx, camy);

  /* ---- teaching overlay on the focus leg ---- */
  if (show_stages) {
    if (focus < 0) focus = 0;
    if (focus >= N_LEGS) focus = N_LEGS - 1;
    k = draw_overlay(w, out, k, focus, camx, camy);
  }
  return k;
}
