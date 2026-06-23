#include "ik.h"
#include "fxmath.h"

/* Forward kinematics of one leg in its LOCAL frame: root at the origin, the
 * chain's reference direction along +x. The internal pose is a single scalar
 * `curl`: each joint turns by the same angle, so segment k points along
 * bend_sign*k*curl. This one-parameter family is the chosen null-space policy —
 * of the many internal poses that hit a given reach, we take the uniform arc.
 * Fills the N_JOINT local joint positions; the last is the end (foot). */
static void fk_local(uint32_t curl, int32_t seg_len, int8_t bend_sign,
                     int32_t* lx, int32_t* ly) {
  lx[0] = 0; ly[0] = 0;
  for (int k = 0; k < N_SEG; k++) {
    uint32_t a = (uint32_t)((int32_t)bend_sign * k * (int32_t)curl);  /* cumulative angle */
    lx[k + 1] = lx[k] + ((seg_len * icos(a)) >> 14);
    ly[k + 1] = ly[k] + ((seg_len * isin(a)) >> 14);
  }
}

int64_t ik_reach2(uint32_t curl, int32_t seg_len, int8_t bend_sign) {
  int32_t lx[N_JOINT], ly[N_JOINT];
  fk_local(curl, seg_len, bend_sign, lx, ly);
  return (int64_t)lx[N_SEG] * lx[N_SEG] + (int64_t)ly[N_SEG] * ly[N_SEG];
}

uint16_t ik_curl_max(int32_t seg_len, int8_t bend_sign) {
  /* Scan a half-turn of curl for the global argmin of reach (the bottom of the
   * fold) and stop one step short of it, so the bisection interval is monotone.
   * For equal segments this is the geometry-only ~120 deg and is independent of
   * seg_len, but we measure it rather than assume it (it owns the data). We take
   * the global minimum, not the first dip: near curl 0 the reach is at its flat
   * maximum and integer rounding makes it wobble by a few units, which must not be
   * mistaken for the bottom of the fold. */
  const uint32_t STEP = ANG_FULL / 1024;       /* fine: ~0.35 deg */
  int64_t best = ik_reach2(0, seg_len, bend_sign);
  uint32_t best_c = 0;
  for (uint32_t c = STEP; c <= ANG_FULL / 2; c += STEP) {
    int64_t r = ik_reach2(c, seg_len, bend_sign);
    if (r < best) { best = r; best_c = c; }
  }
  return (uint16_t)(best_c > STEP ? best_c - STEP : 0);
}

IkResult ik_solve(int32_t sx, int32_t sy, int32_t tx, int32_t ty,
                  int32_t seg_len, uint16_t curl_max, int8_t bend_sign) {
  IkResult r;
  int64_t Dx = tx - sx, Dy = ty - sy;
  int64_t d2 = Dx * Dx + Dy * Dy;               /* squared target distance */

  int32_t lx[N_JOINT], ly[N_JOINT];
  int64_t max2 = ik_reach2(0, seg_len, bend_sign);        /* straight: longest reach */
  int64_t min2 = ik_reach2(curl_max, seg_len, bend_sign); /* most folded we allow    */

  /* ---- STEP 1: LENGTH -----------------------------------------------------
   * Clamp the requested distance into the reachable interval [min, max] (the
   * doc's feasibility condition; policy here is clamp), then bisect the single
   * internal DOF until squared reach meets squared goal. reach2(curl) is monotone
   * decreasing on [0, curl_max], so bisection converges with no derivative and no
   * square root anywhere in the loop. */
  r.clamped = 0;
  int64_t goal2 = d2;
  if (goal2 > max2) { goal2 = max2; r.clamped = 1; }
  if (goal2 < min2) { goal2 = min2; r.clamped = 1; }

  uint32_t lo = 0, hi = curl_max;
  for (int it = 0; it < 16; it++) {
    uint32_t mid = (lo + hi) >> 1;
    if (ik_reach2(mid, seg_len, bend_sign) > goal2) lo = mid;  /* more curl = shorter */
    else hi = mid;
  }
  uint32_t curl = (lo + hi) >> 1;
  fk_local(curl, seg_len, bend_sign, lx, ly);   /* the solved local pose */
  int64_t ex = lx[N_SEG], ey = ly[N_SEG];

  /* ---- STEP 2: DIRECTION --------------------------------------------------
   * One rotation about the root taking the solved chain's root->end direction
   * onto the root->target direction. In the plane this FromTo is exact from a dot
   * and a cross, and we need only the unit (cos,sin), so normalise (E.D, ExD)
   * once. The degenerate target-on-root / zero-reach case has no direction:
   * leave the aim at identity. */
  int64_t dot   = ex * Dx + ey * Dy;
  int64_t cross = ex * Dy - ey * Dx;
  int32_t dcos = TRIG_ONE, dsin = 0;
  uint64_t m2 = (uint64_t)(dot * dot + cross * cross);
  if (m2 > 0) {
    int64_t mag = (int64_t)isqrt64(m2);
    dcos = (int32_t)((dot   * TRIG_ONE) / mag);
    dsin = (int32_t)((cross * TRIG_ONE) / mag);
  }

  /* Bake the aim into every local joint: p_world = root + R(delta) * p_local.
   * The end then sits at root + reach * dir(target); when reach matches the
   * target distance (target was reachable), that is exactly the target. */
  for (int k = 0; k < N_JOINT; k++) {
    int64_t wx = ((int64_t)lx[k] * dcos - (int64_t)ly[k] * dsin) >> 14;
    int64_t wy = ((int64_t)lx[k] * dsin + (int64_t)ly[k] * dcos) >> 14;
    r.jx[k] = sx + (int32_t)wx;
    r.jy[k] = sy + (int32_t)wy;
  }
  r.curl   = (uint16_t)curl;
  r.reach  = (int32_t)isqrt64((uint64_t)(ex * ex + ey * ey));
  r.goal_d = (int32_t)isqrt64((uint64_t)goal2);
  r.dcos   = (int16_t)dcos;
  r.dsin   = (int16_t)dsin;
  return r;
}
