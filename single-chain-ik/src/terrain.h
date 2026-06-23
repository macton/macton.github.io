/* terrain.h — the rough ground the walker crosses.
 *
 * The terrain stores nothing. It is a pure function of world x: a sum of cosine
 * octaves read from the trig table. That is the right shape for this data — it is
 * fully determined, it is read at scattered x (every foothold, every render
 * column), and it must repeat seamlessly as the walker loops, which a harmonic
 * sum over the WORLDP period does for free. height in, world-y out; y increases
 * downward, so a smaller y is higher ground. */
#ifndef SCIK_TERRAIN_H
#define SCIK_TERRAIN_H

#include <stdint.h>

int32_t terrain_y(int32_t x);   /* ground surface world-y at world x, subcells */

#endif
