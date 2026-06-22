/* tanks_move — independent transform: advance positions, resolve collision.
 * Touches position/velocity, reads heading/input and the grid bitset; shares
 * nothing with the turn transform. */
#ifndef TANK_MOVE_H
#define TANK_MOVE_H

#include "defs.h"

/* For each of n tanks: step along the heading by `speed` subcells/tick,
 * resolving against the grid one axis at a time so a tank slides along a wall
 * it hits at an angle. `xy` is packed position (x | y<<16); writes the applied
 * move to `vxy` (packed vx | vy<<16), and sets hit[i] to a bitmask of which
 * intended axis moves were rejected: bit0 = x blocked, bit1 = y blocked (so the
 * value names the wall orientation), 0 = no collision. */
void tanks_move(uint32_t* xy, uint32_t* vxy, uint8_t* hit,
                const uint16_t* ang, const uint8_t* in, uint32_t n,
                int32_t speed, int32_t coll_scale, const uint32_t* grid);

#endif
