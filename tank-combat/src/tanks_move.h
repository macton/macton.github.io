/* tanks_move — independent transform: advance positions, resolve collision.
 * Touches position/velocity, reads heading/input and the grid bitset; shares
 * nothing with the turn transform. */
#ifndef TANK_MOVE_H
#define TANK_MOVE_H

#include "defs.h"

/* For each of n tanks: step along the heading by `speed` subcells/tick,
 * resolving against the grid one axis at a time so a tank slides along a wall
 * it hits at an angle. Writes the applied move to vx/vy (subcells), and sets
 * hit[i] = 1 if an intended axis move was rejected (i.e. the tank collided
 * while moving), else 0. */
void tanks_move(int16_t* x, int16_t* y, int16_t* vx, int16_t* vy, uint8_t* hit,
                const uint16_t* ang, const uint8_t* in, uint32_t n,
                int32_t speed, const uint32_t* grid);

#endif
