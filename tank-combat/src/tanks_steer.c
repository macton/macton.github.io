#include "tanks_steer.h"

void tanks_steer(uint16_t* ang, const int16_t* vx, const int16_t* vy,
                 const uint8_t* hit, uint32_t n, uint16_t rate) {
  for (uint32_t i = 0; i < n; i++) {
    if (!hit[i]) continue;
    int32_t mvx = vx[i], mvy = vy[i];
    if (mvx == 0 && mvy == 0) continue;   /* both axes blocked: no slide dir  */

    /* per-axis collision leaves the resolved velocity axis-aligned, so the
     * slide direction is one of the four cardinals. */
    uint32_t dir;
    if (mvy == 0) dir = (mvx > 0) ? 0u : (N_DIRS / 2);
    else          dir = (mvy > 0) ? (N_DIRS / 4) : (3 * N_DIRS / 4);
    uint16_t target = (uint16_t)(dir << ANGLE_SHIFT);

    int32_t delta = (int16_t)(target - ang[i]);   /* shortest signed distance */
    int32_t step  = rate;
    if (delta >  step) delta =  step;             /* clamp so we don't overshoot */
    else if (delta < -step) delta = -step;
    ang[i] = (uint16_t)(ang[i] + delta);          /* snaps to target when |delta|<=rate */
  }
}
