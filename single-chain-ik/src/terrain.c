#include "terrain.h"
#include "defs.h"

/* Rough ground, still a pure periodic function of x (stores nothing). Three
 * layers add up:
 *   roll      a little smooth texture so treads aren't dead flat
 *   terraces  a medium cosine QUANTISED to STEP_H — flat treads, vertical risers
 *             (the "stairs"): a stepped mound that rises and falls each cycle
 *   obstacles a few flat-topped blocks with sharp vertical edges, placed by hand
 * All three are periodic over WORLDP, so the walk loops with no seam. Higher
 * ground is a smaller y (y increases downward), so we subtract the relief. */

#define ROLL0_A 40
#define ROLL1_A 24
#define TERR_AMP  130         /* terrace envelope, subcells           */
#define TERR_FREQ 4           /* up-down cycles of terracing / period */
#define STEP_H    44          /* riser height — one stair step        */

/* obstacle blocks within one period [0,WORLDP): {x0, x1, height}, sharp edges */
#define N_OBST 3
static const int32_t OBST_X0[N_OBST] = {  4864, 15104, 23808 };
static const int32_t OBST_X1[N_OBST] = {  6144, 15616, 25600 };
static const int32_t OBST_H [N_OBST] = {    64,    76,    68 };

/* round v to the nearest multiple of s, symmetric about zero */
static int32_t qstep(int32_t v, int32_t s) {
  return (v >= 0) ? ((v + s / 2) / s) * s : -((((-v) + s / 2) / s) * s);
}

int32_t terrain_y(int32_t x) {
  int32_t xm = x % WORLDP; if (xm < 0) xm += WORLDP;

  int32_t roll = (ROLL0_A * icos((uint32_t)(x * 2) << 1) +
                  ROLL1_A * icos((uint32_t)(x * 5) << 1)) >> 14;
  int32_t terr = qstep((TERR_AMP * icos((uint32_t)(x * TERR_FREQ) << 1)) >> 14, STEP_H);
  int32_t obs = 0;
  for (int i = 0; i < N_OBST; i++)
    if (xm >= OBST_X0[i] && xm < OBST_X1[i]) obs += OBST_H[i];

  return TERRAIN_MID - (roll + terr + obs);
}

int32_t terrain_max_y_up(int32_t x0, int32_t x1) {
  /* the highest ground (smallest y) over [x0,x1], sampled finely enough to catch
   * the narrowest obstacle. Used by the gait to keep the body clear of whatever
   * is directly beneath it. */
  int32_t best = terrain_y(x0);
  for (int32_t x = x0; x <= x1; x += 32) { int32_t y = terrain_y(x); if (y < best) best = y; }
  return best;
}
