#include "tanks_steer.h"
#include "dirtab.h"
#include "collide.h"

/* Can the tank at (x,y) move a full step along direction `d` with neither axis
 * blocked? (Sliding — one axis blocked — counts as not movable here, so the
 * tank keeps turning until it lines up cleanly with the opening or wall.) */
static int dir_movable(const uint32_t* grid, int32_t x, int32_t y, uint32_t d, int32_t speed) {
  int32_t dx = (dir_cos(d) * speed) >> TRIG_SHIFT;
  int32_t dy = (dir_sin(d) * speed) >> TRIG_SHIFT;
  if (dx && blocked(grid, x + dx, y)) return 0;
  if (dy && blocked(grid, x, y + dy)) return 0;
  return 1;
}

void tanks_steer(const int16_t* x, const int16_t* y, uint16_t* ang,
                 const uint8_t* in, uint32_t n, uint16_t rate,
                 int32_t speed, const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t thr = (int32_t)((in[i] & IN_FWD) != 0) - (int32_t)((in[i] & IN_BACK) != 0);
    if (thr == 0) continue;                       /* not driving: leave heading */

    /* travel = the direction the tank actually moves (forward: heading; back:
     * heading + half a turn). */
    uint32_t off    = (thr < 0) ? (N_DIRS / 2) : 0;
    uint32_t travel = ((ang[i] >> ANGLE_SHIFT) + off) & (N_DIRS - 1);

    if (dir_movable(grid, x[i], y[i], travel, speed)) continue;  /* moving fine */

    /* search outward for the nearest movable travel direction, preferring +k
     * (a consistent handedness so symmetric pockets resolve and never oscillate). */
    int found = 0; uint32_t best = travel;
    for (uint32_t k = 1; k <= N_DIRS / 2 && !found; k++) {
      uint32_t a = (travel + k) & (N_DIRS - 1);
      if (dir_movable(grid, x[i], y[i], a, speed)) { best = a; found = 1; break; }
      if (k == N_DIRS / 2) break;                 /* +k and -k coincide at a half turn */
      uint32_t b = (travel - k) & (N_DIRS - 1);
      if (dir_movable(grid, x[i], y[i], b, speed)) { best = b; found = 1; break; }
    }
    if (!found) continue;                          /* fully enclosed: nowhere to go */

    /* turn the heading so travel heads toward `best`, by up to `rate` */
    uint16_t target = (uint16_t)(((best + off) & (N_DIRS - 1)) << ANGLE_SHIFT);
    int32_t delta = (int16_t)(target - ang[i]);    /* shortest signed distance */
    int32_t step = rate;
    if (delta >  step) delta =  step;
    else if (delta < -step) delta = -step;
    ang[i] = (uint16_t)(ang[i] + delta);
  }
}
