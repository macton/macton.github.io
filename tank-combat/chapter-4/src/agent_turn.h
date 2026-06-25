/* agent_turn — all heading rotation for a set of driven bodies, in one transform:
 * the manual turn input (decoded branch-free from the input bits) AND the
 * auto-steer that keeps a throttled body from getting stuck (one lookup in the
 * escape table). Both write the heading, so they live together. Runs before
 * agent_move (so a turn takes effect the same tick) and reacts to the previous
 * tick's collision flag.
 *
 * It operates on "a set of moving bodies," not on tanks specifically: chapter 3
 * drives both the tanks and the mites through it. The auto-steer keys on a cell's
 * 4-neighbour wall pattern, which is independent of body size, so the turn
 * transform needs no radius — only agent_move does (collide.h).
 *
 * Auto-steer: when a body collided last tick while driving, rotate it toward the
 * nearest direction whose neighbouring cell is open — sliding along flat walls,
 * turning out of convex corners, and rotating around to the opening of a concave
 * pocket. See CONTRACT.md. */
#ifndef AGENT_TURN_H
#define AGENT_TURN_H

#include "defs.h"

/* For each of n bodies: apply the manual left/right turn (±rate, wrapping),
 * then, if it collided last tick (hit[i] != 0) while driving, rotate by up to
 * `rate` toward an open direction. The escape direction is one lookup in the
 * baked PATTERN_ESCAPE constant (escape_table.h), indexed
 * [pattern*N_DIRS + travel_dir], where `pattern` is the body cell's 4-neighbour
 * pattern (read live from `grid`, 4 bits). The table is a compile-time constant,
 * referenced directly like the trig table — not a parameter, since it never
 * varies. */
void agent_turn(const uint32_t* xy, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint32_t* grid);

#endif
