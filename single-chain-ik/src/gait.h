/* gait.h — the gait: where each foot should be this tick.
 *
 * The gait is the IK's input generator, kept separate from the solver. It owns
 * the body advance and the per-leg footfall timing; it does NOT pose the chain.
 * Two alternating groups step on a distance-based phase so the body is always
 * supported and the cadence tracks the walk speed. Stance feet stay planted in
 * the world while the body rides over them (this is what stretches a chain and
 * makes the IK work); swinging feet arc to the next foothold on the terrain. */
#ifndef SCIK_GAIT_H
#define SCIK_GAIT_H

#include "sim.h"

void gait_init(World* w);                 /* plant all feet under their stance homes */
void gait_step(World* w);                 /* advance body, update every foot target  */

/* Leg configuration is constant geometry (hip mounts, bend side), read directly
 * by the solver step; exposed as functions so the const tables stay private. */
void   gait_hip (const World* w, int leg, int32_t* hx, int32_t* hy); /* hip world pos */
int8_t gait_bend(int leg);                /* the leg's bend side (pole sign) */

#endif
