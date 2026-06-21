/* tanks_turn — independent transform: rotate headings by input.
 * Touches only angle and input; shares nothing with the move transform. */
#ifndef TANK_TURN_H
#define TANK_TURN_H

#include "defs.h"

/* For each of n tanks: angle += step if turning right, -= step if left,
 * wrapping mod 65536. Batch transform; the 2-tank case is just n == 2. */
void tanks_turn(uint16_t* ang, const uint8_t* in, uint32_t n, uint16_t step);

#endif
