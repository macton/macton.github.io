#include "tanks_steer.h"

void tanks_steer(uint16_t* ang, const uint8_t* hit, uint32_t n, uint16_t rate) {
  for (uint32_t i = 0; i < n; i++) {
    uint32_t h = hit[i];
    if (h == 0 || h == 3) continue;   /* not blocked, or wedged in a corner   */

    /* The blocked axis names the wall; the two headings parallel to it are the
     * candidates. Horizontal wall (y blocked) -> run along x (E or W); vertical
     * wall (x blocked) -> run along y (the two y cardinals). */
    uint16_t ca, cb;
    if (h & 2) { ca = (uint16_t)(0u << ANGLE_SHIFT);            cb = (uint16_t)((N_DIRS / 2) << ANGLE_SHIFT); }
    else       { ca = (uint16_t)((N_DIRS / 4) << ANGLE_SHIFT); cb = (uint16_t)((3 * N_DIRS / 4) << ANGLE_SHIFT); }

    int32_t da = (int16_t)(ca - ang[i]);   /* shortest signed distance to each  */
    int32_t db = (int16_t)(cb - ang[i]);
    int32_t ada = da < 0 ? -da : da;
    int32_t adb = db < 0 ? -db : db;
    int32_t delta = (ada <= adb) ? da : db;   /* turn toward the nearer parallel */

    int32_t step = rate;
    if (delta >  step) delta =  step;          /* clamp so we don't overshoot    */
    else if (delta < -step) delta = -step;
    ang[i] = (uint16_t)(ang[i] + delta);       /* snaps when |delta| <= rate      */
  }
}
