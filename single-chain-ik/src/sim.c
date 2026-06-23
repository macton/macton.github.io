#include "sim.h"
#include "gait.h"
#include "ik.h"

/* Pose every leg from the current foot targets. This is the project's core
 * transform: for each leg, take the hip (root) and the foot target and run the
 * length-then-direction solve, then record the result in the World for the
 * renderer, the page, and the tests. ik_solve is pure; the World is just where
 * its answers are kept. */
static void solve_legs(World* w) {
  for (int i = 0; i < N_LEGS; i++) {
    int32_t hx, hy;
    gait_hip(w, i, &hx, &hy);
    /* per-leg foreshortening: scale the segment lengths. curl_max is the same —
     * the fold's bottom is at the same curl regardless of overall scale. */
    int32_t L[N_SEG], sc = gait_scale(i);
    for (int k = 0; k < N_SEG; k++) L[k] = w->seg_L[k] * sc / 100;
    IkResult r = ik_solve(hx, hy, w->foot_x[i], w->foot_y[i],
                          L, w->curl_max, gait_bend(i));
    for (int k = 0; k < N_JOINT; k++) {
      w->joint_x[i * N_JOINT + k] = r.jx[k];
      w->joint_y[i * N_JOINT + k] = r.jy[k];
    }
    w->leg_curl[i]    = r.curl;
    w->leg_reach[i]   = r.reach;
    w->leg_goal[i]    = r.goal_d;
    w->leg_clamped[i] = r.clamped;
    w->leg_dcos[i]    = r.dcos;
    w->leg_dsin[i]    = r.dsin;
  }
}

/* Segment taper, per-mille of the base length: a slightly long femur down to a
 * slightly short tarsus (a real leg's proportions, kept mild so the lower joints
 * stay near the foot and the legs don't cut into steps). */
static const int32_t SEG_RATIO[N_SEG] = { 1120, 1000, 880 };  /* sums to 3000 */

void sim_set_seg_len(World* w, uint16_t v) {
  w->seg_len = v ? v : 1;
  for (int k = 0; k < N_SEG; k++) w->seg_L[k] = (int32_t)w->seg_len * SEG_RATIO[k] / 1000;
  /* curl_max depends only on the segment lengths (and is the same for either bend
   * side, since squared reach is), so rebuild it once here, not per solve. */
  w->curl_max = ik_curl_max(w->seg_L, +1);
}

void sim_init(World* w) {
  w->frame = 0;
  w->walk_speed = DEF_WALK_SPEED;
  w->stand_h    = DEF_STAND_H;
  w->step_len   = DEF_STEP_LEN;
  w->step_lift  = DEF_STEP_LIFT;
  sim_set_seg_len(w, DEF_SEG_LEN);

  w->body_x = WORLDP / 4;        /* start over a rolling stretch of ground */
  gait_init(w);                  /* plant the feet and seat the body on them */
  solve_legs(w);                 /* a valid pose before the first frame is drawn */
}

void sim_tick(World* w) {
  gait_step(w);
  solve_legs(w);
  w->frame++;
}
