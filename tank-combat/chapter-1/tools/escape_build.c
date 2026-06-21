/* escape_build.c — HOST-ONLY. The escape-table generation logic, moved out of
 * the runtime (it used to run in sim_init). Reuses the SAME collision geometry
 * the game uses (blocked_x/blocked_y), so the baked table can never drift from
 * the runtime collision behaviour. Linked only into host tools. */
#include "escape_build.h"
#include "../src/collide.h"
#include "../src/dirtab.h"
#include "../src/escape_table.h"   /* ESC_NONE */

void patterns_build_open(uint32_t pattern_open[N_PATTERNS]) {
  /* The probe only reads the 4 orthogonal neighbours, so synthesise a tiny grid
   * holding exactly the pattern's neighbours around one interior cell and run the
   * same centre-origin probe collide uses everywhere. 16 patterns, not 300 cells. */
  int32_t cx = GRID_W / 2, cy = GRID_H / 2;        /* interior: no wrap overlap */
  int32_t px = cx * SUB + SUB / 2, py = cy * SUB + SUB / 2;
  for (uint32_t p = 0; p < N_PATTERNS; p++) {
    uint32_t g[GRID_H] = {0};
    if (p & 1u) g[cy - 1] |= 1u << cx;             /* N */
    if (p & 2u) g[cy]     |= 1u << (cx + 1);       /* E */
    if (p & 4u) g[cy + 1] |= 1u << cx;             /* S */
    if (p & 8u) g[cy]     |= 1u << (cx - 1);       /* W */
    uint32_t mask = 0;
    for (uint32_t d = 0; d < N_DIRS; d++) {
      int32_t dx = (dir_cos(d) * SUB) >> TRIG_SHIFT;             /* one cell over */
      int32_t dy = (dir_sin(d) * SUB) >> TRIG_SHIFT;
      if (dx && blocked_x(g, wrap_x(px + dx), py, dx)) continue;
      if (dy && blocked_y(g, px, wrap_y(py + dy), dy)) continue;
      mask |= (1u << d);
    }
    pattern_open[p] = mask;
  }
}

/* Escape-search order: directions to try relative to `travel`, nearest first,
 * +k before -k (a consistent handedness). +16 (half turn) appears once. */
static const int8_t STEER_SCAN[31] = {
  1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8,
  9, -9, 10, -10, 11, -11, 12, -12, 13, -13, 14, -14, 15, -15, 16,
};

void build_escape_table(uint8_t* pattern_escape, const uint32_t* pattern_open) {
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
