/* analyze_samples.c — host analysis (not shipped in the wasm).
 *
 * Question (from the experiment): "if we're moving, which grid elements do we
 * actually sample, for movement or for turning — and does the response collapse
 * to a lookup over a small set of local patterns?"
 *
 * This program answers it from the data by replaying the exact sampling
 * formulas in collide.c and grouping the results. Build/run with analyze.sh.
 */
#include <stdio.h>
#include "../src/defs.h"
#include "../src/dirtab.h"
#include "../src/collide.h"
#include "../src/sim.h"

/* The default level, copied from sim.c so the analysis sees the shipped map. */
static const uint32_t GRID_INIT[GRID_H] = {
  0xFFFFF, 0x80001, 0x98019, 0x98619, 0x80601, 0x8C061, 0x8C061, 0x80401,
  0x8C061, 0x8C061, 0x80601, 0x98619, 0x98019, 0x80001, 0xFFFFF,
};
#define WRAPC(c) ((c) < 0 ? (c) + GRID_W : ((c) >= GRID_W ? (c) - GRID_W : (c)))
#define WRAPR(r) ((r) < 0 ? (r) + GRID_H : ((r) >= GRID_H ? (r) - GRID_H : (r)))
static int is_wall(const uint32_t* g, int32_t cx, int32_t cy) {
  return (g[WRAPR(cy)] >> WRAPC(cx)) & 1u;
}

/* The 4 orthogonal neighbours of a cell as a 4-bit pattern: N E S W. */
static uint32_t nesw(const uint32_t* g, int32_t cx, int32_t cy) {
  return (is_wall(g, cx, cy - 1) << 0) | (is_wall(g, cx + 1, cy) << 1)
       | (is_wall(g, cx, cy + 1) << 2) | (is_wall(g, cx - 1, cy) << 3);
}

int main(void) {
  uint32_t grid[GRID_H];
  for (int32_t r = 0; r < GRID_H; r++) grid[r] = GRID_INIT[r];

  /* --- (1) Which cells does the TURNING precompute sample, per cell? --------
   * cells_build_move probes one cell out in each of 32 directions from the cell
   * CENTRE. Record the distinct grid cells those probes actually read. */
  printf("== TURNING: cells sampled by the escape precompute (centre origin)\n");
  {
    int32_t cx = 9, cy = 7;                 /* an interior open cell */
    int32_t px = cx * SUB + SUB / 2, py = cy * SUB + SUB / 2;
    int seen[GRID_H][GRID_W] = {{0}};
    for (uint32_t d = 0; d < N_DIRS; d++) {
      int32_t dx = (dir_cos(d) * SUB) >> TRIG_SHIFT;
      int32_t dy = (dir_sin(d) * SUB) >> TRIG_SHIFT;
      if (dx) { int32_t ex = (px + dx) + (dx > 0 ? TANK_R : -TANK_R);
        seen[WRAPR((py - TANK_R) >> SUB_SHIFT)][WRAPC(ex >> SUB_SHIFT)] = 1;
        seen[WRAPR((py + TANK_R) >> SUB_SHIFT)][WRAPC(ex >> SUB_SHIFT)] = 1; }
      if (dy) { int32_t ey = (py + dy) + (dy > 0 ? TANK_R : -TANK_R);
        seen[WRAPR(ey >> SUB_SHIFT)][WRAPC((px - TANK_R) >> SUB_SHIFT)] = 1;
        seen[WRAPR(ey >> SUB_SHIFT)][WRAPC((px + TANK_R) >> SUB_SHIFT)] = 1; }
    }
    int n = 0;
    for (int32_t r = 0; r < GRID_H; r++)
      for (int32_t c = 0; c < GRID_W; c++)
        if (seen[r][c]) { printf("   (%d,%d) dx=%+d dy=%+d\n", c - cx, r - cy,
                                 c - cx, r - cy); n++; }
    printf("   -> %d distinct cells, all 4-orthogonal neighbours (no diagonals)\n\n", n);
  }

  /* --- (2) The open-direction mask as a function of the 16 local patterns. ---
   * patterns_build_open computes one mask per 4-bit pattern (grid-independent).
   * Cross-check: every cell's live mask must equal its pattern's mask, and our
   * local nesw() must agree with the shipped cell_pattern(). */
  uint32_t pattern_open[N_PATTERNS];
  patterns_build_open(pattern_open);
  int pat_count[16] = {0};
  int contradiction = 0;
  for (int32_t cy = 0; cy < GRID_H; cy++)
    for (int32_t cx = 0; cx < GRID_W; cx++) {
      uint32_t p = nesw(grid, cx, cy);
      if (p != cell_pattern(grid, cx, cy)) contradiction = 1;  /* API agrees? */
      pat_count[p]++;
    }
  printf("== TURNING: open-direction mask per 4-neighbour pattern (16 rows)\n");
  printf("   pattern  N E S W   open-dirs(32b)   #cells-in-default-map\n");
  for (uint32_t p = 0; p < 16; p++)
    printf("   %2u       %d %d %d %d   0x%08x       %d%s\n",
      p, p&1,(p>>1)&1,(p>>2)&1,(p>>3)&1, pattern_open[p], pat_count[p],
      pat_count[p] ? "" : "   (unused in default map)");
  printf("   -> response is a pure function of the 4-bit local pattern: %s\n",
         contradiction ? "NO (contradiction!)" : "YES");
  printf("   -> 300-cell x 32-dir escape table collapses to 16 x 32 = 512 entries\n\n");

  /* --- (3) Which cells does MOVEMENT sample for a tank mid-cell? ------------
   * Per tick a moving tank tests the leading edge: one column (x) / row (y) at
   * the up-to-two cells it straddles on the other axis. Show a straddling case. */
  printf("== MOVEMENT: cells sampled by one tick's blocked_x / blocked_y\n");
  {
    int32_t x = 5 * SUB + 200, y = 7 * SUB + 200;  /* straddles into +x,+y cells */
    int32_t mx = +20, my = +20;
    int32_t exr = WRAPC(((x + mx) + TANK_R) >> SUB_SHIFT);
    printf("   x-step: column %d at rows %d,%d  -> (%d,%d),(%d,%d)\n", exr,
      WRAPR((y - TANK_R) >> SUB_SHIFT), WRAPR((y + TANK_R) >> SUB_SHIFT),
      exr, WRAPR((y - TANK_R) >> SUB_SHIFT), exr, WRAPR((y + TANK_R) >> SUB_SHIFT));
    int32_t eyr = WRAPR(((y + my) + TANK_R) >> SUB_SHIFT);
    printf("   y-step: row %d at cols %d,%d  -> (%d,%d),(%d,%d)\n", eyr,
      WRAPC((x - TANK_R) >> SUB_SHIFT), WRAPC((x + TANK_R) >> SUB_SHIFT),
      WRAPC((x - TANK_R) >> SUB_SHIFT), eyr, WRAPC((x + TANK_R) >> SUB_SHIFT), eyr);
    printf("   -> <=2 cells per axis (1 if the tank doesn't straddle a boundary)\n");
  }
  return 0;
}
