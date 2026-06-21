/* tanks_steer — when a tank collides while moving, rotate its heading toward
 * the direction it is actually sliding, until aligned. Pipeline stage after
 * tanks_move: it consumes that transform's output (vx, vy, hit). */
#ifndef TANK_STEER_H
#define TANK_STEER_H

#include "defs.h"

/* For each of n tanks that collided this tick (hit[i]) while still moving,
 * rotate ang[i] toward the cardinal direction the tank should *face*, by up to
 * `rate` angle-units, snapping exactly when aligned. Moving forward, that is
 * the slide (resolved velocity) direction; moving backward, the tank faces the
 * opposite way, so the target is 180 degrees from the slide direction. `in`
 * supplies the throttle sign. No effect when the tank did not collide or is
 * not moving. */
void tanks_steer(uint16_t* ang, const int16_t* vx, const int16_t* vy,
                 const uint8_t* hit, const uint8_t* in, uint32_t n, uint16_t rate);

#endif
