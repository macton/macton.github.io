/* tanks_steer — when a tank collides while moving, rotate its heading to run
 * parallel with the wall it hit, until aligned. Pipeline stage after
 * tanks_move: it consumes that transform's hit bitmask (which names the wall
 * orientation). */
#ifndef TANK_STEER_H
#define TANK_STEER_H

#include "defs.h"

/* For each of n tanks blocked on exactly one axis this tick (hit[i] is 1 or 2),
 * rotate ang[i] toward the nearer of the two headings parallel to that wall, by
 * up to `rate` angle-units, snapping when aligned. Derived from the wall
 * orientation, not the velocity, so it works at any approach angle including
 * head-on (a tank backing straight into a wall pivots to slide along it instead
 * of sticking facing away). No effect when not blocked or wedged in a corner
 * (hit == 0 or 3). */
void tanks_steer(uint16_t* ang, const uint8_t* hit, uint32_t n, uint16_t rate);

#endif
