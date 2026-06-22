#include "grid_paths.h"
#include "collide.h"   /* cell_is_wall */

/* BFS scratch. Single-threaded module, so file-static buffers are fine and keep
 * the build allocation-free (no malloc, no large stack frames). */
static uint16_t g_bd[N_CELLS];   /* BFS distance from the current source */
static uint16_t g_q [N_CELLS];   /* BFS queue */

void l1_build_screen(uint16_t* dist_screen, const uint32_t* grid, uint32_t screen) {
  int32_t sox = (int32_t)(screen % SCREENS_X) * GRID_W;   /* screen origin, world cells */
  int32_t soy = (int32_t)(screen / SCREENS_X) * GRID_H;

  for (uint32_t i = 0; i < TRI; i++) dist_screen[i] = (uint16_t)L1_INF;

  for (uint32_t s = 0; s < N_CELLS; s++) {
    int32_t scx = (int32_t)(s % GRID_W), scy = (int32_t)(s / GRID_W);
    if (cell_is_wall(grid, sox + scx, soy + scy)) continue;   /* wall: leave its pairs INF */

    for (uint32_t i = 0; i < N_CELLS; i++) g_bd[i] = (uint16_t)L1_INF;
    uint32_t head = 0, tail = 0;
    g_bd[s] = 0; g_q[tail++] = (uint16_t)s;
    while (head < tail) {
      uint32_t c = g_q[head++];
      uint16_t cd = g_bd[c];
      int32_t cx = (int32_t)(c % GRID_W), cy = (int32_t)(c / GRID_W);
      for (int d = 0; d < 4; d++) {
        int32_t nx = cx + CARD_DCX[d], ny = cy + CARD_DCY[d];
        if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) continue;  /* confined to screen */
        uint32_t n = (uint32_t)(ny * GRID_W + nx);
        if (g_bd[n] != (uint16_t)L1_INF) continue;
        if (cell_is_wall(grid, sox + nx, soy + ny)) continue;
        g_bd[n] = (uint16_t)(cd + 1);
        g_q[tail++] = (uint16_t)n;
      }
    }
    for (uint32_t t = s; t < N_CELLS; t++)        /* upper triangle only */
      if (g_bd[t] != (uint16_t)L1_INF) dist_screen[tri_index(s, t)] = g_bd[t];
  }
}

void l1_build_all(uint16_t* dist, const uint32_t* grid) {
  for (uint32_t s = 0; s < N_SCREENS; s++)
    l1_build_screen(dist + (uint32_t)s * TRI, grid, s);
}

uint8_t l1_next_dir(const uint16_t* dist_screen, uint32_t src, uint32_t dst) {
  if (src == dst) return DIR_NONE;                 /* arrived */
  uint16_t d0 = l1_get(dist_screen, src, dst);
  if (d0 == (uint16_t)L1_INF) return DIR_NONE;     /* no path within the screen */

  int32_t scx = (int32_t)(src % GRID_W), scy = (int32_t)(src / GRID_W);
  for (uint8_t d = 0; d < 4; d++) {                /* canonical N,E,S,W */
    int32_t nx = scx + CARD_DCX[d], ny = scy + CARD_DCY[d];
    if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) continue;  /* a screen exit is Level 2's job */
    uint32_t n = (uint32_t)(ny * GRID_W + nx);
    if (l1_get(dist_screen, n, dst) == (uint16_t)(d0 - 1)) return d;
  }
  return DIR_NONE;                                 /* unreachable (finite d0 guarantees one above) */
}
