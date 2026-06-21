#include "tanks_turn.h"
#include "dirtab.h"
#include "collide.h"

/* Is the neighbouring cell one full cell along direction `d` open? We probe a
 * whole cell (not a sub-cell nudge) so a dead-end that only admits a tiny wiggle
 * inside the current cell is correctly seen as NOT a way out. Coordinates wrap. */
static int dir_movable(const uint32_t* grid, int32_t x, int32_t y, uint32_t d) {
  int32_t dx = (dir_cos(d) * SUB) >> TRIG_SHIFT;
  int32_t dy = (dir_sin(d) * SUB) >> TRIG_SHIFT;
  if (dx && blocked(grid, x + dx, y)) return 0;
  if (dy && blocked(grid, x, y + dy)) return 0;
  return 1;
}

void tanks_turn(const int16_t* x, const int16_t* y, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    /* player's manual turn */
    int32_t d = (int32_t)((in[i] & IN_RIGHT) != 0) - (int32_t)((in[i] & IN_LEFT) != 0);
    ang[i] = (uint16_t)((int32_t)ang[i] + (int32_t)rate * d);   /* wraps mod 65536 */

    /* auto-steer out of a collision seen last tick (else nothing to do) */
    if (hit[i] == 0) continue;
    int32_t thr = (int32_t)((in[i] & IN_FWD) != 0) - (int32_t)((in[i] & IN_BACK) != 0);
    if (thr == 0) continue;

    /* travel = the direction the tank actually moves (forward: heading; back:
     * heading + half a turn). */
    uint32_t off    = (thr < 0) ? (N_DIRS / 2) : 0;
    uint32_t travel = ((ang[i] >> ANGLE_SHIFT) + off) & (N_DIRS - 1);

    /* search outward for the nearest travel direction whose next cell is open,
     * preferring +k (a consistent handedness so symmetric traps never oscillate). */
    int found = 0, plus = 1; uint32_t best = travel;
    for (uint32_t k = 1; k <= N_DIRS / 2 && !found; k++) {
      uint32_t a = (travel + k) & (N_DIRS - 1);
      if (dir_movable(grid, x[i], y[i], a)) { best = a; found = 1; plus = 1; break; }
      if (k == N_DIRS / 2) break;                 /* +k and -k coincide at a half turn */
      uint32_t b = (travel - k) & (N_DIRS - 1);
      if (dir_movable(grid, x[i], y[i], b)) { best = b; found = 1; plus = 0; break; }
    }
    if (!found) continue;                          /* fully enclosed: nowhere to go */

    /* Turn toward `best` in the SAME handedness the search found it, by up to
     * `rate`. Turning by the handedness (not the shortest signed delta) keeps a
     * 180-degree escape from oscillating: at the antipode the shortest delta
     * flips sign every tick, but a fixed handedness sweeps cleanly. */
    uint16_t target = (uint16_t)(((best + off) & (N_DIRS - 1)) << ANGLE_SHIFT);
    uint16_t dist = (uint16_t)(plus ? (target - ang[i]) : (ang[i] - target));
    uint16_t step = dist < rate ? dist : rate;     /* clamp so we snap, not overshoot */
    ang[i] = (uint16_t)(plus ? (ang[i] + step) : (ang[i] - step));
  }
}
