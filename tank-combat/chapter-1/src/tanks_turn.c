#include "tanks_turn.h"
#include "collide.h"

/* Escape-search order: directions to try relative to `travel`, nearest first,
 * +k before -k (a consistent handedness). +16 (half turn) appears once. Used
 * only to build the escape table; the per-tick path never scans. */
static const int8_t STEER_SCAN[31] = {
  1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8,
  9, -9, 10, -10, 11, -11, 12, -12, 13, -13, 14, -14, 15, -15, 16,
};

#define ESC_NONE 0xFFu   /* no open direction reachable from this pattern+travel */

void tanks_build_escape(uint8_t* pattern_escape, const uint32_t* pattern_open) {
  for (uint32_t p = 0; p < N_PATTERNS; p++) {
    uint32_t open = pattern_open[p];
    for (uint32_t travel = 0; travel < N_DIRS; travel++) {
      uint8_t out = ESC_NONE;
      for (uint32_t j = 0; j < sizeof STEER_SCAN; j++) {
        uint32_t cand = (uint32_t)((int32_t)travel + STEER_SCAN[j]) & (N_DIRS - 1);
        if ((open >> cand) & 1u) {                 /* nearest open: dir + handedness */
          out = (uint8_t)(((STEER_SCAN[j] > 0) << 5) | cand);
          break;
        }
      }
      pattern_escape[p * N_DIRS + travel] = out;
    }
  }
}

void tanks_turn(const int16_t* x, const int16_t* y, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint32_t* grid, const uint8_t* pattern_escape) {
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

    /* player's manual turn */
    ang[i] = (uint16_t)((int32_t)ang[i] + (int32_t)rate * turn);   /* wraps mod 65536 */

    /* auto-steer out of a collision seen last tick (else nothing to do) */
    if (hit[i] == 0 || thr == 0) continue;

    /* the tank cell's 4-neighbour pattern, read live (4 bits) — that is all the
     * steer needs; the escape per (pattern,travel) is the precomputed table */
    uint32_t pat    = cell_pattern(grid, x[i] >> SUB_SHIFT, y[i] >> SUB_SHIFT);
    uint32_t travel = ((ang[i] >> ANGLE_SHIFT) + travel_off) & (N_DIRS - 1);

    /* one lookup: the precomputed nearest-open escape for this pattern + travel */
    uint8_t esc = pattern_escape[pat * N_DIRS + travel];
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
