/* tanks_turn — all heading rotation, in one transform: the player's turn input
 * (decoded branch-free from the input bits) AND the auto-steer that keeps a
 * throttled tank from getting stuck (one lookup in the escape table). Both write
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
 * `rate` toward an open direction. The escape direction is one lookup in
 * `pattern_escape` indexed [pattern*N_DIRS + travel_dir], where `pattern` is the
 * tank cell's 4-neighbour pattern (read live from `grid`, 4 bits). */
void tanks_turn(const int16_t* x, const int16_t* y, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint32_t* grid, const uint8_t* pattern_escape);

/* Precompute the escape table from the 16 per-pattern open-direction masks: for
 * every local pattern and every travel direction, run the nearest-open scan once
 * and store the result. pattern_escape must hold N_PATTERNS*N_DIRS bytes. The
 * table is grid-independent (a pattern is a pattern), so it is built once — not
 * per grid change. */
void tanks_build_escape(uint8_t* pattern_escape, const uint32_t* pattern_open);

#endif
