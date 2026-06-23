/* test.c — native, GPU-free tests of the solver and the walk.
 *
 * The contract the page relies on, made explicit and checked:
 *  - LENGTH: squared reach is monotone on [0, curl_max] (the bisection's premise).
 *  - The two stages together land the foot on the target when it is reachable.
 *  - Out-of-reach targets are clamped (flagged) to maximum extension toward them.
 *  - The aim is a unit rotation.
 *  - The walk keeps feet on the ground, the body above them, and never pops a
 *    foot across the screen between ticks. */
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"
#include "gait.h"
#include "ik.h"
#include "terrain.h"
#include "fxmath.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
  printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static int32_t idist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  int64_t dx = ax - bx, dy = ay - by;
  return (int32_t)isqrt64((uint64_t)(dx * dx + dy * dy));
}

/* The LENGTH stage hits any goal across the reachable band — for a range of
 * segment lengths, not just the default. This is what the bisection promises;
 * its monotone-on-[0,curl_max] premise is what makes it true (away from the flat
 * top, where reach is at its rounding-noisy maximum and any curl there is fine). */
static void test_reach_band(void) {
  for (int seg = 80; seg <= 400; seg += 40) {
    uint16_t cmax = ik_curl_max(seg, +1);
    CHECK(cmax > 0, "curl_max==0 for seg=%d", seg);
    int32_t maxr = (int32_t)isqrt64((uint64_t)ik_reach2(0, seg, +1));
    int32_t minr = (int32_t)isqrt64((uint64_t)ik_reach2(cmax, seg, +1));
    CHECK(abs(maxr - 3 * seg) <= 2, "max reach %d != 3*seg=%d", maxr, 3 * seg);
    CHECK(minr < maxr / 2, "fold too shallow: min %d max %d", minr, maxr);
    for (int32_t goal = minr + 12; goal <= maxr - 12; goal += (maxr - minr) / 20 + 1) {
      IkResult r = ik_solve(0, 0, goal, 0, seg, cmax, +1);  /* target straight ahead */
      CHECK(!r.clamped, "seg=%d goal=%d unexpectedly clamped", seg, goal);
      CHECK(abs(r.reach - goal) <= 8, "seg=%d reach %d != goal %d", seg, r.reach, goal);
      int e = idist(r.jx[N_SEG], r.jy[N_SEG], goal, 0);
      CHECK(e <= 8, "seg=%d foot off goal by %d", seg, e);
    }
  }
}

/* Both stages: a reachable target ends up under the foot; the aim is a unit. */
static void test_reach_targets(void) {
  const int32_t seg = DEF_SEG_LEN;
  const uint16_t cmax = ik_curl_max(seg, +1);
  const int32_t maxr = 3 * seg;
  const int32_t sx = 1000, sy = 1000;
  int worst = 0;
  for (int32_t tx = sx - maxr; tx <= sx + maxr; tx += 37)
    for (int32_t ty = sy - maxr; ty <= sy + maxr; ty += 37) {
      int32_t d = idist(sx, sy, tx, ty);
      if (d > maxr - 20 || d < 40) continue;             /* stay inside the reachable band */
      IkResult r = ik_solve(sx, sy, tx, ty, seg, cmax, +1);
      CHECK(!r.clamped, "unexpected clamp d=%d", d);
      int e = idist(r.jx[N_SEG], r.jy[N_SEG], tx, ty);   /* foot vs target */
      if (e > worst) worst = e;
      CHECK(e <= 8, "foot missed target by %d (d=%d)", e, d);
      int64_t m = (int64_t)r.dcos * r.dcos + (int64_t)r.dsin * r.dsin;
      int unit = (int)isqrt64((uint64_t)m);
      CHECK(unit >= TRIG_ONE - 12 && unit <= TRIG_ONE + 12, "aim not unit: %d", unit);
      CHECK(r.jx[0] == sx && r.jy[0] == sy, "hip moved");
    }
  printf("  reachable targets: worst foot error %d sub (<=8)\n", worst);
}

/* Out of reach: clamp flagged, foot at max extension straight toward the target. */
static void test_clamp(void) {
  const int32_t seg = DEF_SEG_LEN;
  const uint16_t cmax = ik_curl_max(seg, +1);
  const int32_t maxr = 3 * seg;
  int32_t sx = 0, sy = 0, tx = 4000, ty = 1500;          /* far away */
  IkResult r = ik_solve(sx, sy, tx, ty, seg, cmax, +1);
  CHECK(r.clamped, "far target not clamped");
  CHECK(abs(r.reach - maxr) <= 4, "clamped reach %d != max %d", r.reach, maxr);
  /* foot direction parallel to target direction: cross product ~ 0 */
  int64_t fx = r.jx[N_SEG] - sx, fy = r.jy[N_SEG] - sy;
  int64_t cross = fx * (ty - sy) - fy * (tx - sx);
  int64_t scale = (int64_t)maxr * idist(sx, sy, tx, ty);
  CHECK(llabs(cross) <= scale / 50, "clamped foot not aimed at target");
}

/* The integrated walk: step the sim and check the gait/IK invariants each tick. */
static void test_walk(void) {
  World w;
  sim_init(&w);
  int32_t prev_foot_x[N_LEGS], prev_foot_y[N_LEGS];
  for (int i = 0; i < N_LEGS; i++) { prev_foot_x[i] = w.foot_x[i]; prev_foot_y[i] = w.foot_y[i]; }

  int worst_foot = 0, worst_pop = 0;
  for (int t = 0; t < 2000; t++) {
    sim_tick(&w);
    int64_t avg = 0;
    for (int i = 0; i < N_LEGS; i++) {
      /* the solved foot joint is on the gait target */
      int e = idist(w.joint_x[i * N_JOINT + N_SEG], w.joint_y[i * N_JOINT + N_SEG],
                    w.foot_x[i], w.foot_y[i]);
      if (!w.leg_clamped[i] && e > worst_foot) worst_foot = e;
      CHECK(w.leg_clamped[i] || e <= 8, "tick %d leg %d foot off target by %d", t, i, e);

      /* a planted foot sits exactly on the terrain */
      if (!w.swinging[i]) {
        int32_t gy = terrain_y(w.foot_x[i]);
        CHECK(w.foot_y[i] == gy, "tick %d leg %d planted foot off ground (%d vs %d)",
              t, i, w.foot_y[i], gy);
      }
      /* no popping: a foot moves a bounded amount per tick */
      int pop = idist(w.foot_x[i], w.foot_y[i], prev_foot_x[i], prev_foot_y[i]);
      if (pop > worst_pop) worst_pop = pop;
      CHECK(pop <= 80, "tick %d leg %d foot popped %d sub", t, i, pop);
      prev_foot_x[i] = w.foot_x[i]; prev_foot_y[i] = w.foot_y[i];
      avg += w.foot_y[i];
    }
    avg /= N_LEGS;
    /* body rides above its feet (smaller y == higher) */
    CHECK(w.body_y < avg, "tick %d body not above feet", t);
  }
  printf("  walk: worst foot error %d sub, worst per-tick foot move %d sub\n", worst_foot, worst_pop);
}

static void test_terrain_periodic(void) {
  for (int32_t x = 0; x < 4000; x += 333)
    CHECK(terrain_y(x) == terrain_y(x + WORLDP), "terrain not periodic at x=%d", x);
}

int main(void) {
  printf("single-chain-ik tests\n");
  test_reach_band();
  test_reach_targets();
  test_clamp();
  test_walk();
  test_terrain_periodic();
  if (g_fail) { printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
  printf("\nall tests passed\n");
  return 0;
}
