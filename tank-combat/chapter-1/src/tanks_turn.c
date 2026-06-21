#include "tanks_turn.h"

/* Input decode, by table. The input bitfield (FWD=1 BACK=2 LEFT=4 RIGHT=8)
 * indexes this directly: `turn` is the manual turn direction, `thr` the throttle
 * sign, `travel_off` the heading->travel offset (half a turn when reversing). */
typedef struct { int8_t turn; int8_t thr; uint8_t travel_off; } TurnInput;
static const TurnInput INPUT[16] = {
  /* 0 ....*/ { +0, +0, 0 },
  /* 1 F...*/ { +0, +1, 0 },
  /* 2 .B..*/ { +0, -1, N_DIRS/2 },
  /* 3 FB..*/ { +0, +0, 0 },
  /* 4 ..L.*/ { -1, +0, 0 },
  /* 5 F.L.*/ { -1, +1, 0 },
  /* 6 .BL.*/ { -1, -1, N_DIRS/2 },
  /* 7 FBL.*/ { -1, +0, 0 },
  /* 8 ...R*/ { +1, +0, 0 },
  /* 9 F..R*/ { +1, +1, 0 },
  /*10 .B.R*/ { +1, -1, N_DIRS/2 },
  /*11 FB.R*/ { +1, +0, 0 },
  /*12 ..LR*/ { +0, +0, 0 },
  /*13 F.LR*/ { +0, +1, 0 },
  /*14 .BLR*/ { +0, -1, N_DIRS/2 },
  /*15 FBLR*/ { +0, +0, 0 },
};

/* Escape-search order: directions to try relative to `travel`, nearest first,
 * +k before -k (a consistent handedness). +16 (half turn) appears once. Used
 * only to build the escape table; the per-tick path never scans. */
static const int8_t STEER_SCAN[31] = {
  1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8,
  9, -9, 10, -10, 11, -11, 12, -12, 13, -13, 14, -14, 15, -15, 16,
};

#define ESC_NONE 0xFFu   /* no open direction reachable from this cell+travel */

void tanks_build_escape(uint8_t* cell_escape, const uint32_t* cell_move) {
  for (uint32_t cell = 0; cell < N_CELLS; cell++) {
    uint32_t open = cell_move[cell];
    for (uint32_t travel = 0; travel < N_DIRS; travel++) {
      uint8_t out = ESC_NONE;
      for (uint32_t j = 0; j < sizeof STEER_SCAN; j++) {
        uint32_t cand = (uint32_t)((int32_t)travel + STEER_SCAN[j]) & (N_DIRS - 1);
        if ((open >> cand) & 1u) {                 /* nearest open: dir + handedness */
          out = (uint8_t)(((STEER_SCAN[j] > 0) << 5) | cand);
          break;
        }
      }
      cell_escape[cell * N_DIRS + travel] = out;
    }
  }
}

void tanks_turn(const int16_t* x, const int16_t* y, uint16_t* ang,
                const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                const uint8_t* cell_escape) {
  for (uint32_t i = 0; i < n; i++) {
    TurnInput d = INPUT[in[i] & 0xF];

    /* player's manual turn */
    ang[i] = (uint16_t)((int32_t)ang[i] + (int32_t)rate * d.turn);   /* wraps mod 65536 */

    /* auto-steer out of a collision seen last tick (else nothing to do) */
    if (hit[i] == 0 || d.thr == 0) continue;

    uint32_t cell   = (uint32_t)(y[i] >> SUB_SHIFT) * GRID_W + (uint32_t)(x[i] >> SUB_SHIFT);
    uint32_t travel = ((ang[i] >> ANGLE_SHIFT) + d.travel_off) & (N_DIRS - 1);

    /* one lookup: the precomputed nearest-open escape for this cell + travel */
    uint8_t esc = cell_escape[cell * N_DIRS + travel];
    if (esc == ESC_NONE) continue;                 /* fully enclosed: nowhere to go */
    uint32_t best = esc & (N_DIRS - 1);
    int plus = (esc >> 5) & 1;

    /* Turn toward `best` in the handedness the scan found it, by up to `rate`.
     * Using the handedness rather than the shortest signed delta keeps a
     * 180-degree escape from oscillating: at the antipode the shortest delta
     * flips sign every tick, but a fixed handedness sweeps cleanly. */
    uint16_t target = (uint16_t)(((best + d.travel_off) & (N_DIRS - 1)) << ANGLE_SHIFT);
    uint16_t dist = (uint16_t)(plus ? (target - ang[i]) : (ang[i] - target));
    uint16_t step = dist < rate ? dist : rate;     /* clamp so we snap, not overshoot */
    ang[i] = (uint16_t)(plus ? (ang[i] + step) : (ang[i] - step));
  }
}
