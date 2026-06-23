#include "gait.h"
#include "terrain.h"
#include "fxmath.h"

/* Leg configuration — constant geometry, read directly (a const table is not
 * state). Eight legs: 0..3 near side front->back, 4..7 far side front->back.
 *
 * Spider anatomy (see the reference): all legs mount on the CEPHALOTHORAX (the
 * front body section), clustered there, NOT on the abdomen. So the hips sit in a
 * tight forward group while the feet FAN OUT wide — front legs far forward, rear
 * legs sweeping back past the abdomen. The legs are longer than the hip-to-foot
 * distance, so the length solve folds them and they ARCH UP to a high knee, the
 * iconic spider profile. Far legs mount higher (behind) and reach a touch wider
 * and are drawn dimmer, for a side-view read of depth; every leg runs the same solve.
 *   HIPX/HIPY  hip mount, offset from the body centre (subcells; +x = forward)
 *   STANCE     the foot's home x offset from the hip (the fan)
 *   GROUP      one of two alternating footfall groups (a diagonal tetrapod)
 *   BEND       the bend side (pole) — chosen so the leg arches UP */
/* STANCE keeps every foot clearly to one side of its hip (|stance| > half a
 * stride), so a foot never swings across under its own hip — that keeps the
 * chosen bend side a consistent ARCH UP through the whole gait, with no frame the
 * pole would want to flip (which would pop).
 *
 * SCALE foreshortens the middle legs. In 3-D a spider's middle legs point out to
 * the sides, so in this side elevation they read shorter; shortening them also
 * stops a long leg from coiling when its foot sits nearly under the hip (a coil
 * would jut a knee out over an adjacent step). Front and rear legs lie more in
 * the view plane, so they stay full length and arch the most. */
static const int32_t LEG_HIPX  [N_LEGS] = {  100,  70,  40,  10,   100,  70,  40,  10 };
static const int32_t LEG_HIPY  [N_LEGS] = {    0,   0,   0,   0,   -86, -86, -86, -86 };
static const int32_t LEG_STANCE[N_LEGS] = {  320, 150,-230,-400,   350, 170,-250,-430 };
static const int32_t LEG_SCALE [N_LEGS] = {  100,  78,  78, 100,   100,  78,  78, 100 }; /* per-100 */
static const uint8_t LEG_GROUP [N_LEGS] = {    0,   1,   0,   1,     1,   0,   1,   0 };
static const int8_t  LEG_BEND  [N_LEGS] = {   +1,  +1,  -1,  -1,    +1,  +1,  -1,  -1 };

/* Hip mounts ride on the body and turn with its lean, so the legs re-solve to
 * keep their planted feet as the body pitches — that adaptation is the natural
 * part (and shows the IK working). */
void gait_hip(const World* w, int leg, int32_t* hx, int32_t* hy) {
  int32_t ox = LEG_HIPX[leg], oy = LEG_HIPY[leg];
  int32_t c = w->body_cos, s = w->body_sin;
  *hx = w->body_x + (int32_t)(((int64_t)ox * c - (int64_t)oy * s) >> 14);
  *hy = w->body_y + (int32_t)(((int64_t)ox * s + (int64_t)oy * c) >> 14);
}
int8_t  gait_bend (int leg) { return LEG_BEND[leg]; }
int32_t gait_scale(int leg) { return LEG_SCALE[leg]; }

static int32_t home_x(const World* w, int leg) {
  return w->body_x + LEG_HIPX[leg] + LEG_STANCE[leg];
}

/* gait cadence is measured in body travel, so it tracks the walk speed. One cycle
 * spans step_len of travel; a leg is airborne for the first `swing` of its
 * group's window. The two groups are half a cycle apart (swing <= period/2), so
 * a support set is always planted. */
static uint32_t period_of(const World* w) { return w->step_len ? w->step_len : 1; }
static uint32_t swing_of(uint32_t period) {
  uint32_t s = period * 2 / 5;          /* ~40% duty */
  if (s < 1) s = 1;
  if (s > period / 2) s = period / 2;
  return s;
}
static uint32_t leg_phase(const World* w, int i, uint32_t period, uint8_t* airborne) {
  int32_t cyc = w->body_x + (int32_t)(LEG_GROUP[i] ? period / 2 : 0);
  int32_t p = cyc % (int32_t)period; if (p < 0) p += period;
  *airborne = (uint32_t)p < swing_of(period);
  return (uint32_t)p;
}

/* The lift height for a swing: at least step_lift, but raised so the arc clears
 * whatever terrain (a step riser, an obstacle) sits between the two footholds —
 * computed once at lift-off. This is what lets a swinging foot step OVER an
 * obstacle smoothly instead of being clamped to its face (which would pop). */
static int32_t swing_peak(const World* w, int32_t fx, int32_t tx) {
  int32_t lo = fx < tx ? fx : tx, hiX = fx < tx ? tx : fx;
  int32_t a = terrain_y(fx), b = terrain_y(tx);
  int32_t lowfoot = a > b ? a : b;                  /* lower foothold (larger y)   */
  int32_t topY = terrain_max_y_up(lo, hiX);         /* highest ground between (small y) */
  int32_t need = (lowfoot - topY) + 24;             /* clear the bump above it + margin */
  return need > (int32_t)w->step_lift ? need : (int32_t)w->step_lift;
}

/* Place foot i for the current phase. A stance foot rests on its foothold. A
 * swinging foot follows a smooth arc between its lift-off and landing footholds
 * (interpolating the two foothold HEIGHTS, plus a half-cosine lift sized to clear
 * the terrain between) — it does NOT hug the instantaneous ground, which on
 * stepped terrain would teleport the foot at a riser. A final clamp to
 * min(arc, terrain) means a foot is never below the surface, by construction. */
static void set_foot(World* w, int i, uint32_t p, uint32_t swing) {
  if (!w->swinging[i]) {
    w->foot_x[i] = w->plant_x[i];
    w->foot_y[i] = terrain_y(w->plant_x[i]);
    return;
  }
  uint32_t t = ((uint32_t)p << 16) / swing;          /* Q16 progress 0..1 */
  int32_t span = w->swing_tx[i] - w->swing_fx[i];
  int32_t fx = w->swing_fx[i] + (int32_t)(((int64_t)span * t) >> 16);
  int32_t ay = terrain_y(w->swing_fx[i]), by = terrain_y(w->swing_tx[i]);
  int32_t base = ay + (int32_t)(((int64_t)(by - ay) * t) >> 16);
  int32_t lift = (int32_t)(((int64_t)w->swing_peak[i] * (TRIG_ONE - icos(t))) >> 15); /* (1-cos)/2 */
  int32_t fy = base - lift;
  int32_t ground = terrain_y(fx);                    /* never below the surface */
  w->foot_x[i] = fx;
  w->foot_y[i] = fy < ground ? fy : ground;
}

/* Body weight & lean (the "natural" layer). The body is not a rigid level slab:
 *  - it rides stand_h above the mean foot height (floored to clear an obstacle
 *    directly beneath it), with a small gait-synced vertical BOB;
 *  - it PITCHES toward the ground's grade (sampled over a wide span so it follows
 *    the slope, not each step), clamped small and smoothed so it leans into hills;
 *  - plus a small gait-synced pitch SWAY so it rocks as it steps.
 * On `snap` (init) the orientation is set directly; otherwise it eases toward the
 * target so it never jerks. */
#define SLOPE_SPAN    600   /* terrain half-span sampled for the grade, subcells */
#define PITCH_SIN_MAX 3200  /* clamp the lean to ~11 deg (Q14 sine)              */
#define ORIENT_SMOOTH  28   /* /256 eased toward the target orientation per tick */
#define GAIT_SWAY     300   /* gait-synced pitch sway amplitude, angle units     */
#define BOB_AMP         9   /* gait-synced vertical bob amplitude, subcells      */

static void seat_body(World* w, int snap) {
  int64_t s = 0;
  for (int i = 0; i < N_LEGS; i++) s += w->foot_y[i];
  int32_t ride = (int32_t)(s / N_LEGS) - w->stand_h;          /* ride above the feet */
  int32_t under = terrain_max_y_up(w->body_x + BODY_X0, w->body_x + BODY_X1);
  int32_t clear = under - BODY_LOW - 16;                      /* never sink into ground */
  w->body_y = ride < clear ? ride : clear;

  uint32_t period = w->step_len ? w->step_len : 1;
  int32_t pm = w->body_x % (int32_t)period; if (pm < 0) pm += period;
  uint32_t phase = (uint32_t)(((int64_t)pm * ANG_FULL) / period);
  w->body_y += (BOB_AMP * isin(phase)) >> 14;                 /* subtle bob */

  /* target lean = the ground tangent over a wide span, clamped, then a small sway */
  int32_t dx = 2 * SLOPE_SPAN;
  int32_t dy = terrain_y(w->body_x + SLOPE_SPAN) - terrain_y(w->body_x - SLOPE_SPAN);
  uint32_t mag = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy)); if (!mag) mag = 1;
  int32_t tsin = (int32_t)((int64_t)dy * TRIG_ONE / (int64_t)mag);
  if (tsin >  PITCH_SIN_MAX) tsin =  PITCH_SIN_MAX;
  if (tsin < -PITCH_SIN_MAX) tsin = -PITCH_SIN_MAX;
  int32_t tcos = (int32_t)isqrt64((uint64_t)((int64_t)TRIG_ONE * TRIG_ONE - (int64_t)tsin * tsin));
  int32_t sway = (GAIT_SWAY * isin(phase)) >> 14;             /* small angle */
  int32_t sc = icos((uint32_t)sway), ss = isin((uint32_t)sway);
  int32_t cx = (int32_t)(((int64_t)tcos * sc - (int64_t)tsin * ss) >> 14);  /* compose */
  int32_t sx = (int32_t)(((int64_t)tcos * ss + (int64_t)tsin * sc) >> 14);

  if (snap) { w->body_cos = (int16_t)cx; w->body_sin = (int16_t)sx; return; }
  int32_t bc = w->body_cos + (((cx - w->body_cos) * ORIENT_SMOOTH) >> 8);
  int32_t bs = w->body_sin + (((sx - w->body_sin) * ORIENT_SMOOTH) >> 8);
  uint32_t m2 = isqrt64((uint64_t)((int64_t)bc * bc + (int64_t)bs * bs)); if (!m2) m2 = 1;
  w->body_cos = (int16_t)((int64_t)bc * TRIG_ONE / (int64_t)m2);
  w->body_sin = (int16_t)((int64_t)bs * TRIG_ONE / (int64_t)m2);
}

void gait_init(World* w) {
  uint32_t period = period_of(w), swing = swing_of(period);
  for (int i = 0; i < N_LEGS; i++) {
    uint8_t air;
    uint32_t p = leg_phase(w, i, period, &air);
    /* seat each leg consistent with its phase so the first frame doesn't jump */
    w->swinging[i] = air;
    if (air) {
      w->swing_tx[i] = home_x(w, i) + (int32_t)w->step_len / 2;
      w->swing_fx[i] = w->swing_tx[i] - (int32_t)w->step_len;
      w->swing_peak[i] = swing_peak(w, w->swing_fx[i], w->swing_tx[i]);
    } else {
      w->plant_x[i] = home_x(w, i);
    }
    set_foot(w, i, p, swing);
  }
  seat_body(w, 1);   /* settle orientation directly so the first frame is calm */
}

void gait_step(World* w) {
  w->body_x += w->walk_speed;
  uint32_t period = period_of(w), swing = swing_of(period);

  for (int i = 0; i < N_LEGS; i++) {
    uint8_t now_air;
    uint32_t p = leg_phase(w, i, period, &now_air);

    if (now_air && !w->swinging[i]) {                 /* lift off */
      w->swing_fx[i] = w->plant_x[i];
      w->swing_tx[i] = home_x(w, i) + (int32_t)w->step_len / 2;  /* land ahead of home */
      w->swing_peak[i] = swing_peak(w, w->swing_fx[i], w->swing_tx[i]);
    } else if (!now_air && w->swinging[i]) {          /* touch down */
      w->plant_x[i] = w->swing_tx[i];
    }
    w->swinging[i] = now_air;
    set_foot(w, i, p, swing);
  }
  seat_body(w, 0);   /* ease the lean toward the slope */
}
