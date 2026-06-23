#include "render.h"
#include "terrain.h"
#include "fxmath.h"

#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

/* Vertical reference the camera holds steady (CAM_Y, render.h). */
#define COL   28        /* terrain sample spacing, subcells                 */
#define GROUND_BOTTOM (VIEW_H / 2 + 2 * SUB)   /* fill extends below the view */

/* The scene is lit LIGHT so the dark legs read against it (see app.js clear).
 * Ground is a mid tone — darker than the sky so the stepped silhouette shows,
 * lighter than the legs so the feet show against it. */
static const uint32_t COL_GROUND     = RGBA(120, 126, 112, 255);
static const uint32_t COL_GROUND_TOP = RGBA(150, 162, 134, 255);
/* body: a big orange abdomen (rear) and a darker cephalothorax (front) */
static const uint32_t COL_ABDOMEN    = RGBA(224, 138,  52, 255);
static const uint32_t COL_ABDOMEN_HI = RGBA(242, 170,  88, 255);
static const uint32_t COL_CEPH       = RGBA( 58,  62,  74, 255);
static const uint32_t COL_EYE        = RGBA( 18,  20,  26, 255);
/* legs: an orange femur down to a near-black tarsus; far side dimmer (but still
 * clearly darker than the light background, so it stays visible) */
static const uint32_t COL_FEMUR_N    = RGBA(238, 152,  58, 255);
static const uint32_t COL_FEMUR_F    = RGBA(176, 118,  58, 255);
static const uint32_t COL_LOWER_N    = RGBA( 30,  32,  40, 255);
static const uint32_t COL_LOWER_F    = RGBA( 78,  82,  94, 255);
static const uint32_t COL_KNEE_N     = RGBA(238, 152,  58, 255);
static const uint32_t COL_KNEE_F     = RGBA(176, 118,  58, 255);
static const uint32_t COL_FOOT       = RGBA( 24,  26,  32, 255);
static const uint32_t COL_CLAMP      = RGBA(214,  64,  48, 255);  /* foot can't reach */
/* teaching overlay (semi-transparent; the host enables alpha blending) */
static const uint32_t COL_GHOST      = RGBA( 40, 104, 198, 190);  /* pre-aim pose    */
static const uint32_t COL_REF        = RGBA( 40, 104, 198, 150);  /* reference ray   */
static const uint32_t COL_TARGET     = RGBA(228, 120,  36, 245);  /* the foot target */

static uint32_t push(Inst* out, uint32_t k, int32_t cx, int32_t cy, int32_t hx, int32_t hy,
                     int32_t co, int32_t si, uint32_t rgba) {
  if (k >= INST_MAX) return k;
  Inst* o = &out[k];
  o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->hx = (int16_t)hx; o->hy = (int16_t)hy;
  o->co = (int16_t)co; o->si = (int16_t)si; o->rgba = rgba;
  return k + 1;
}

/* an oriented quad (a limb segment / ray) between two camera-relative points */
static uint32_t cap(Inst* out, uint32_t k, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                    int32_t hw, uint32_t rgba) {
  int32_t dx = bx - ax, dy = by - ay;
  uint32_t len = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
  int32_t co = TRIG_ONE, si = 0;
  if (len > 0) { co = (int32_t)((int64_t)dx * TRIG_ONE / len); si = (int32_t)((int64_t)dy * TRIG_ONE / len); }
  return push(out, k, (ax + bx) / 2, (ay + by) / 2, (int32_t)len / 2, hw, co, si, rgba);
}

/* one leg's chain: orange femur, dark tibia, thin dark tarsus, with knee knuckles.
 * The foot marker is drawn LATER (after the ground), so it stays on top. */
static uint32_t draw_leg(const World* w, Inst* out, uint32_t k, int i, int32_t camx, int32_t camy) {
  int near = (i < N_LEGS / 2);
  const int32_t* JX = &w->joint_x[i * N_JOINT];
  const int32_t* JY = &w->joint_y[i * N_JOINT];
  uint32_t fem = near ? COL_FEMUR_N : COL_FEMUR_F;
  uint32_t low = near ? COL_LOWER_N : COL_LOWER_F;
  uint32_t kn  = near ? COL_KNEE_N  : COL_KNEE_F;
  int32_t hw[N_SEG]; /* per-segment half-width: thick femur -> thin tarsus */
  hw[0] = near ? 13 : 9; hw[1] = near ? 9 : 6; hw[2] = near ? 5 : 4;
  for (int s = 0; s < N_SEG; s++)
    k = cap(out, k, JX[s] - camx, JY[s] - camy, JX[s + 1] - camx, JY[s + 1] - camy,
            hw[s], s == 0 ? fem : low);
  /* knee knuckles at the interior joints */
  for (int j = 1; j < N_SEG; j++)
    k = push(out, k, JX[j] - camx, JY[j] - camy, hw[j] + 1, hw[j] + 1, TRIG_ONE, 0, kn);
  return k;
}

/* the length/direction teaching overlay for one leg: the pre-aim (length-only)
 * pose recovered by un-rotating the solved chain, plus the target marker. */
static uint32_t draw_overlay(const World* w, Inst* out, uint32_t k, int i, int32_t camx, int32_t camy) {
  int32_t hx = w->joint_x[i * N_JOINT], hy = w->joint_y[i * N_JOINT];
  int32_t dcos = w->leg_dcos[i], dsin = w->leg_dsin[i];
  int32_t gx[N_JOINT], gy[N_JOINT];
  for (int j = 0; j < N_JOINT; j++) {
    int32_t wx = w->joint_x[i * N_JOINT + j] - hx;
    int32_t wy = w->joint_y[i * N_JOINT + j] - hy;
    int32_t lx = (int32_t)(((int64_t)wx * dcos + (int64_t)wy * dsin) >> 14);   /* R(-delta) */
    int32_t ly = (int32_t)((-(int64_t)wx * dsin + (int64_t)wy * dcos) >> 14);
    gx[j] = hx + lx - camx;
    gy[j] = hy + ly - camy;
  }
  for (int s = 0; s < N_SEG; s++) k = cap(out, k, gx[s], gy[s], gx[s + 1], gy[s + 1], 6, COL_GHOST);
  k = cap(out, k, gx[0], gy[0], gx[N_SEG], gy[N_SEG], 3, COL_REF);            /* reference ray */
  k = push(out, k, w->foot_x[i] - camx, w->foot_y[i] - camy, 15, 15, 11585, 11585, COL_TARGET);
  return k;
}

/* a body quad at body-local offset (ox,oy), turned with the body's lean (bc,bs)
 * about the body centre (already at camera-relative (0, rbc)) */
static uint32_t body_part(Inst* out, uint32_t k, int32_t rbc, int32_t bc, int32_t bs,
                          int32_t ox, int32_t oy, int32_t hx, int32_t hy, uint32_t col) {
  int32_t rx = (int32_t)(((int64_t)ox * bc - (int64_t)oy * bs) >> 14);
  int32_t ry = (int32_t)(((int64_t)ox * bs + (int64_t)oy * bc) >> 14);
  return push(out, k, rx, rbc + ry, hx, hy, bc, bs, col);
}

uint32_t build_view(const World* w, Inst* out, int show_stages, int focus) {
  uint32_t k = 0;
  int32_t camx = w->body_x, camy = CAM_Y;

  /* ---- the walker, drawn first so the OPAQUE ground (below) occludes any part
   * of a leg that dips below the surface — a leg is never seen under the ground,
   * the same way a side-scroller's foreground ground hides it. far legs behind,
   * body, near legs in front. ---- */
  for (int i = N_LEGS / 2; i < N_LEGS; i++) k = draw_leg(w, out, k, i, camx, camy);

  /* body (abdomen, cephalothorax, head), turned with the body's lean about its centre */
  int32_t rbc = w->body_y - camy, bc = w->body_cos, bs = w->body_sin;
  k = body_part(out, k, rbc, bc, bs,  -96,  6, 78, 60, COL_ABDOMEN);     /* abdomen (rear) */
  k = body_part(out, k, rbc, bc, bs, -110, -6, 52, 40, COL_ABDOMEN_HI);  /*  + highlight   */
  k = body_part(out, k, rbc, bc, bs,   44,  0, 52, 38, COL_CEPH);        /* cephalothorax  */
  k = body_part(out, k, rbc, bc, bs,   92, -2, 14, 12, COL_EYE);         /* head/eyes      */

  for (int i = 0; i < N_LEGS / 2; i++) k = draw_leg(w, out, k, i, camx, camy);

  /* ---- the ground ON TOP: fill columns (opaque) then a stepped tread highlight.
   * Drawing fill here hides the below-surface part of any leg behind it. ---- */
  int32_t x0 = camx - VIEW_W / 2 - COL, x1 = camx + VIEW_W / 2 + COL;
  for (int32_t x = x0; x <= x1; x += COL) {
    int32_t rty = terrain_y(x) - camy;
    k = push(out, k, x - camx, (rty + GROUND_BOTTOM) / 2, COL / 2 + 2, (GROUND_BOTTOM - rty) / 2,
             TRIG_ONE, 0, COL_GROUND);
    k = push(out, k, x - camx, rty, COL / 2 + 2, 4, TRIG_ONE, 0, COL_GROUND_TOP);  /* tread cap */
  }

  /* ---- feet ON TOP of the ground (the contact points stay visible) ---- */
  for (int i = 0; i < N_LEGS; i++) {
    int32_t fx = w->joint_x[i * N_JOINT + N_SEG] - camx;
    int32_t fy = w->joint_y[i * N_JOINT + N_SEG] - camy;
    int near = (i < N_LEGS / 2);
    k = push(out, k, fx, fy, near ? 5 : 4, near ? 6 : 5, TRIG_ONE, 0,
             w->leg_clamped[i] ? COL_CLAMP : COL_FOOT);
  }

  /* ---- teaching overlay on the focus leg (on top of everything) ---- */
  if (show_stages) {
    if (focus < 0) focus = 0;
    if (focus >= N_LEGS) focus = N_LEGS - 1;
    k = draw_overlay(w, out, k, focus, camx, camy);
  }
  return k;
}
