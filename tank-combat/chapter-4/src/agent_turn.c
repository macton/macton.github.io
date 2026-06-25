#include "agent_turn.h"
#include "collide.h"
#include "escape_table.h"   /* PATTERN_ESCAPE (baked constant) + ESC_NONE */

void agent_turn(const uint32_t* xy, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    /* Decode the input bits directly, branch-free. Tabulating this first (an
     * INPUT[16] table) made the structure visible: turn/thr/travel_off are just
     * these bit functions, so the table isn't needed — the bits already hold it.
     * (FWD=1 BACK=2 LEFT=4 RIGHT=8.) */
    uint32_t b = in[i];
    int32_t fwd  = (int32_t)(b & 1u);
    int32_t back = (int32_t)((b >> 1) & 1u);
    int32_t turn = (int32_t)((b >> 3) & 1u) - (int32_t)((b >> 2) & 1u);  /* right - left */
    int32_t thr  = fwd - back;                                           /* fwd - back   */
    uint32_t travel_off = (uint32_t)(back & (fwd ^ 1)) * (N_DIRS / 2);   /* half turn when reversing */

    /* manual turn */
    ang[i] = (uint16_t)((int32_t)ang[i] + (int32_t)rate * turn);   /* wraps mod 65536 */

    /* auto-steer out of a collision seen last tick — but only when the driver is
     * not steering. Any manual turn in the input (LEFT or RIGHT held) means the
     * driver owns the heading this tick; don't fight or augment it. */
    if (hit[i] == 0 || thr == 0 || (b & (IN_LEFT | IN_RIGHT))) continue;

    /* the body cell's 4-neighbour pattern, read live (4 bits) — that is all the
     * steer needs; the escape per (pattern,travel) is the precomputed table */
    uint32_t pat    = cell_pattern(grid, xy_lo(xy[i]) >> SUB_SHIFT, xy_hi(xy[i]) >> SUB_SHIFT);
    uint32_t travel = ((ang[i] >> ANGLE_SHIFT) + travel_off) & (N_DIRS - 1);

    /* one lookup: the baked nearest-open escape for this pattern + travel.
     * PATTERN_ESCAPE is a compile-time constant (every possible pattern), so we
     * reference it directly, the same way movement reads the trig table. */
    uint8_t esc = PATTERN_ESCAPE[pat * N_DIRS + travel];
    if (esc == ESC_NONE) continue;                 /* fully enclosed: nowhere to go */
    uint32_t best = esc & (N_DIRS - 1);
    int plus = (esc >> 5) & 1;

    /* Turn toward `best` in the handedness the scan found it, by up to `rate`.
     * Using the handedness rather than the shortest signed delta keeps a
     * 180-degree escape from oscillating: at the antipode the shortest delta
     * flips sign every tick, but a fixed handedness sweeps cleanly. */
    uint16_t target = (uint16_t)(((best + travel_off) & (N_DIRS - 1)) << ANGLE_SHIFT);
    uint16_t dist = (uint16_t)(plus ? (target - ang[i]) : (ang[i] - target));
    uint16_t step = dist < rate ? dist : rate;     /* clamp so we snap, not overshoot */
    ang[i] = (uint16_t)(plus ? (ang[i] + step) : (ang[i] - step));
  }
}
