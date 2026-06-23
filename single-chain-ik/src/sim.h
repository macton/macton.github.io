/* sim.h — the simulation core: all world data + the per-tick update.
 *
 * No dependency on rendering or wasm: plain C over the World struct, built and
 * run natively by the tests (test.c / test.sh). render.c and wasm.c sit on top.
 *
 * Each tick does two things, in order:
 *   gait_step  — advance the body and decide where every foot should be now
 *                (plant a stance foot, arc a swinging one over the ground);
 *   ik_solve   — for each leg, pose the chain so its foot lands on that target,
 *                by the length-then-direction method (ik.c).
 * The IK output (joint positions and the per-leg solve readouts) is written back
 * into the World: it is the very thing this project computes, so it is core
 * output the renderer and the page read and the tests check — not decoration. */
#ifndef SCIK_SIM_H
#define SCIK_SIM_H

#include "defs.h"

typedef struct World World;
struct World {
  /* ---- body ------------------------------------------------------------- */
  int32_t body_x, body_y;          /* world position, subcells (x grows as it walks) */
  int16_t body_cos, body_sin;      /* body in-plane orientation, Q14 (leans to the slope) */

  /* ---- per-leg gait state (SoA) ----------------------------------------- */
  int32_t plant_x [N_LEGS];        /* resting foothold x; its y is the terrain there  */
  uint8_t swinging[N_LEGS];        /* 1 while the foot is in the air                   */
  int32_t swing_fx[N_LEGS];        /* swing start x (where it lifted off)              */
  int32_t swing_tx[N_LEGS];        /* swing end x (where it will plant)                */
  int32_t swing_peak[N_LEGS];      /* this swing's lift height (raised to clear terrain)*/
  int32_t foot_x  [N_LEGS];        /* current foot target, world subcells             */
  int32_t foot_y  [N_LEGS];

  /* ---- per-leg IK output (consumed by render, the page, and the tests) --- */
  int32_t  joint_x [N_LEGS * N_JOINT];  /* world joints, hip[0]..foot[N_SEG]           */
  int32_t  joint_y [N_LEGS * N_JOINT];
  uint16_t leg_curl[N_LEGS];        /* LENGTH stage answer: solved internal bend       */
  int32_t  leg_reach[N_LEGS];       /* achieved |foot - hip|, subcells                 */
  int32_t  leg_goal [N_LEGS];       /* target distance aimed for (clamped), subcells   */
  uint8_t  leg_clamped[N_LEGS];     /* 1 if the foot target was out of reach           */
  int16_t  leg_dcos[N_LEGS];        /* DIRECTION stage answer: hip aim rotation, Q14    */
  int16_t  leg_dsin[N_LEGS];

  uint32_t frame;

  /* ---- tunables (editable on the page) ---------------------------------- */
  uint16_t walk_speed;             /* body advance, subcells per tick                  */
  uint16_t seg_len;                /* one segment length, subcells                     */
  uint16_t stand_h;                /* body height above its feet, subcells             */
  uint16_t step_len;               /* stride length / gait period, subcells of travel  */
  uint16_t step_lift;              /* peak foot lift during a swing, subcells          */

  /* ---- derived from seg_len (rebuilt only when it changes) -------------- */
  int32_t  seg_L[N_SEG];           /* the tapered segment lengths (femur..tarsus)      */
  uint16_t curl_max;               /* monotone ceiling for the length bisection        */
};

void sim_init(World* w);          /* place the walker, plant its feet, solve once */
void sim_tick(World* w);          /* advance one fixed step: gait, then IK per leg */
void sim_set_seg_len(World* w, uint16_t v); /* set seg_len and rebuild curl_max */

#endif
