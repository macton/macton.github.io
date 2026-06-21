/* tanks_turn — all heading rotation, in one transform: the player's turn input
 * AND the auto-steer that keeps a throttled tank from getting stuck. Both write
 * the heading, so they live together. Runs before tanks_move (so a turn takes
 * effect the same tick) and reacts to the previous tick's collision flag.
 *
 * Auto-steer: when a tank collided last tick while driving, rotate it toward the
 * nearest direction whose neighbouring cell is open — sliding along flat walls,
 * turning out of convex corners, and rotating around to the opening of a concave
 * pocket. See CONTRACT.md. */
#ifndef TANK_TURN_H
#define TANK_TURN_H

#include "defs.h"

/* For each of n tanks: apply the player's left/right turn (±rate, wrapping),
 * then, if it collided last tick (hit[i] != 0) while driving, rotate by up to
 * `rate` toward the nearest direction whose neighbouring cell is open. The open
 * directions come from `cell_move` (the precomputed per-cell escape cache,
 * indexed by the tank's cell), not a live grid query. */
void tanks_turn(const int16_t* x, const int16_t* y, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint32_t* cell_move);

#endif
