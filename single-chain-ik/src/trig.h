/* trig.h — one baked cosine table, read interpolated. The whole demo's angle
 * math runs off this table; sine is the same table a quarter-turn over, so there
 * is no separate sine table (data-oriented-design: don't store derivable data).
 *
 * Angle space is ANG_FULL == 65536 units per turn (a uint16 wraps a full turn on
 * its own). The table is Q14: cos in [-1,1] maps to [-16384, 16384]. */
#ifndef SCIK_TRIG_H
#define SCIK_TRIG_H

#include <stdint.h>

#define N_TRIG    1024     /* cosine samples over one full circle            */
#define TRIG_BITS 10       /* N_TRIG == 1 << TRIG_BITS                       */
#define TRIG_ONE  16384    /* Q14 unit                                       */
#define ANG_FULL  65536    /* angle units per full turn                      */
#define ANG_90    16384    /* quarter turn (ANG_FULL / 4)                    */
#define ANG_FRAC  6        /* interpolation bits == 16 - TRIG_BITS           */

extern const int16_t TRIG_COS[N_TRIG];   /* generated, tools/gen_trig.c */

/* cos(angle), Q14, linearly interpolated between table samples. The top
 * TRIG_BITS of the (wrapped) angle index the table; the low ANG_FRAC bits blend
 * toward the next sample, so a sweeping angle moves the foot smoothly instead of
 * stepping (matters for the per-frame solve — no popping). */
static inline int32_t icos(uint32_t a) {
  a &= (ANG_FULL - 1);
  uint32_t i = a >> ANG_FRAC;
  uint32_t f = a & ((1u << ANG_FRAC) - 1);
  int32_t c0 = TRIG_COS[i];
  int32_t c1 = TRIG_COS[(i + 1) & (N_TRIG - 1)];
  return c0 + (((c1 - c0) * (int32_t)f) >> ANG_FRAC);
}
static inline int32_t isin(uint32_t a) { return icos(a - ANG_90); }

#endif
