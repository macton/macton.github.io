/* agent_move — independent transform: advance a set of bodies, resolve wall
 * collision. Touches position/velocity, reads heading/input and the grid bitset;
 * shares nothing with the turn transform.
 *
 * Size is data, not a fork. The body's collision half-extent is a `radius`
 * parameter, threaded into blocked_x/blocked_y: the tanks pass TANK_R, the mites
 * pass the smaller MITE_R. One transform serves both — the auto-steer keys on the
 * 4-neighbour wall pattern, which both sizes share. */
#ifndef AGENT_MOVE_H
#define AGENT_MOVE_H

#include "defs.h"

/* For each of n bodies: step along the heading by `speed` subcells/tick,
 * resolving against the grid one axis at a time so a body slides along a wall it
 * hits at an angle, with collision half-extent `radius`. `xy` is packed position
 * (x | y<<16); writes the applied move to `vxy` (packed vx | vy<<16), and sets
 * hit[i] to a bitmask of which intended axis moves were rejected: bit0 = x
 * blocked, bit1 = y blocked (so the value names the wall orientation), 0 = no
 * collision. */
void agent_move(uint32_t* xy, uint32_t* vxy, uint8_t* hit,
                const uint16_t* ang, const uint8_t* in, uint32_t n,
                int32_t speed, int32_t coll_scale, const uint32_t* grid, int32_t radius);

#endif
