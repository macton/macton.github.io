#include "gait.h"
#include "terrain.h"

/* Leg configuration — constant geometry, read directly (a const table is not
 * state). Eight legs: 0..3 are the near side front->back, 4..7 the far side
 * front->back. The near/far split is a side-view reading aid (far legs drawn
 * dimmer and mounted a touch higher and longer so they don't hide behind the
 * near ones); every leg runs the identical solve.
 *   HIPX    hip x offset from body centre (subcells; +x is the walk direction)
 *   HIPY    hip y offset from body centre (far legs sit higher = behind)
 *   STANCE  the foot's home x offset from the hip (front legs reach forward)
 *   GROUP   one of two alternating footfall groups (always one group supports)
 *   BEND    which side the chain folds toward (the pole choice) */
static const int32_t LEG_HIPX [N_LEGS] = {  210,  72, -72, -210,   210,  72, -72, -210 };
static const int32_t LEG_HIPY [N_LEGS] = {    0,   0,   0,    0,   -80, -80, -80,  -80 };
static const int32_t LEG_STANCE[N_LEGS]= {  150,  50, -50, -150,   180,  75, -75, -180 };
static const uint8_t LEG_GROUP[N_LEGS] = {    0,   1,   0,    1,     1,   0,   1,    0 };
static const int8_t  LEG_BEND [N_LEGS] = {   -1,  -1,  +1,   +1,    -1,  -1,  +1,   +1 };

void gait_hip(const World* w, int leg, int32_t* hx, int32_t* hy) {
  *hx = w->body_x + LEG_HIPX[leg];
  *hy = w->body_y + LEG_HIPY[leg];
}
int8_t gait_bend(int leg) { return LEG_BEND[leg]; }

/* a leg's stance home in the world (where its foot wants to rest) */
static int32_t home_x(const World* w, int leg) {
  return w->body_x + LEG_HIPX[leg] + LEG_STANCE[leg];
}

/* the gait cycle is measured in body travel, so cadence tracks the walk speed.
 * One full cycle spans step_len of travel; a leg is airborne for the first
 * `swing` of its group's window. The two groups are half a cycle apart and never
 * airborne together (swing <= period/2), so a support set is always down. */
static uint32_t period_of(const World* w) { return w->step_len ? w->step_len : 1; }
static uint32_t swing_of(uint32_t period) {
  uint32_t s = period * 2 / 5;          /* ~40% duty */
  if (s < 1) s = 1;
  if (s > period / 2) s = period / 2;
  return s;
}

/* where leg i is in its cycle: position p in [0,period) and whether it is airborne */
static uint32_t leg_phase(const World* w, int i, uint32_t period, uint8_t* airborne) {
  int32_t cyc = w->body_x + (int32_t)(LEG_GROUP[i] ? period / 2 : 0);
  int32_t p = cyc % (int32_t)period; if (p < 0) p += period;
  *airborne = (uint32_t)p < swing_of(period);
  return (uint32_t)p;
}

/* place foot i for the current phase: a stance foot rests on its foothold; a
 * swinging foot arcs from its lift-off to its landing with a half-sine lift. */
static void set_foot(World* w, int i, uint32_t p, uint32_t swing) {
  if (w->swinging[i]) {
    uint32_t t = ((uint32_t)p << 16) / swing;        /* Q16 progress 0..1 */
    int32_t span = w->swing_tx[i] - w->swing_fx[i];
    int32_t fx = w->swing_fx[i] + (int32_t)(((int64_t)span * t) >> 16);
    int32_t lift = (int32_t)(((int64_t)w->step_lift * isin(t >> 1)) >> 14);
    w->foot_x[i] = fx;
    w->foot_y[i] = terrain_y(fx) - lift;
  } else {
    w->foot_x[i] = w->plant_x[i];
    w->foot_y[i] = terrain_y(w->plant_x[i]);
  }
}

static void seat_body(World* w) {
  int64_t s = 0;
  for (int i = 0; i < N_LEGS; i++) s += w->foot_y[i];
  w->body_y = (int32_t)(s / N_LEGS) - w->stand_h;   /* ride stand_h above the feet */
}

void gait_init(World* w) {
  uint32_t period = period_of(w), swing = swing_of(period);
  for (int i = 0; i < N_LEGS; i++) {
    uint8_t air;
    uint32_t p = leg_phase(w, i, period, &air);
    /* Start each leg consistent with its phase so the first frame doesn't jump:
     * a stance leg plants under its home; a swing leg gets a synthetic stride
     * centred on its home (lift-off half a stride back, landing half ahead). */
    w->swinging[i] = air;
    if (air) {
      w->swing_tx[i] = home_x(w, i) + (int32_t)w->step_len / 2;
      w->swing_fx[i] = w->swing_tx[i] - (int32_t)w->step_len;
    } else {
      w->plant_x[i] = home_x(w, i);
    }
    set_foot(w, i, p, swing);
  }
  seat_body(w);
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
    } else if (!now_air && w->swinging[i]) {          /* touch down */
      w->plant_x[i] = w->swing_tx[i];
    }
    w->swinging[i] = now_air;
    set_foot(w, i, p, swing);
  }
  seat_body(w);
}
