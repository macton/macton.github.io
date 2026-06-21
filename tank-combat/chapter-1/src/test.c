/* test.c — native (host) tests for the simulation core. No wasm, no render.
 * Verifies the movement contract in CONTRACT.md: a tank holding a throttle is
 * never left permanently stuck while a reachable opening exists.
 *
 * Build & run: ./test.sh  (compiles sim + transforms + this file with host cc) */

#include "sim.h"
#include <stdio.h>

static int g_checks = 0, g_fails = 0;
static void check(int cond, const char* msg) {
  g_checks++;
  if (!cond) { g_fails++; printf("  FAIL: %s\n", msg); }
  else       { printf("  ok:   %s\n", msg); }
}

/* --- scenario helpers ----------------------------------------------------- */
/* These edit the grid directly. No cache to refresh: the steer reads each cell's
 * 4-neighbour pattern live, and the pattern->escape table is grid-independent
 * (built once in sim_init), so a grid edit takes effect on the next tick. */
static void arena_open(World* w) {            /* border walls only */
  for (int r = 0; r < GRID_H; r++)
    w->grid[r] = (r == 0 || r == GRID_H - 1) ? ((1u << GRID_W) - 1u)
                                             : (1u | (1u << (GRID_W - 1)));
}
static void wall(World* w, int c, int r, int v) {
  if (v) w->grid[r] |= (1u << c); else w->grid[r] &= ~(1u << c);
}
static void place(World* w, int t, double cx, double cy, unsigned dir) {
  w->tank_x[t] = (int16_t)(cx * SUB);
  w->tank_y[t] = (int16_t)(cy * SUB);
  w->tank_ang[t] = (uint16_t)(dir << ANGLE_SHIFT);
}
static double cellx(World* w, int t) { return (double)w->tank_x[t] / SUB; }
static double celly(World* w, int t) { return (double)w->tank_y[t] / SUB; }

/* run `ticks`, return the longest run of consecutive ticks with zero movement */
static int run_max_stuck(World* w, int ticks) {
  int cur = 0, mx = 0;
  for (int i = 0; i < ticks; i++) {
    sim_tick(w);
    if (w->tank_vx[0] == 0 && w->tank_vy[0] == 0) { cur++; if (cur > mx) mx = cur; }
    else cur = 0;
  }
  return mx;
}

/* --- scenarios ------------------------------------------------------------ */
static void t_open_space(void) {
  printf("open space, hold forward:\n");
  World w; sim_init(&w); arena_open(&w);
  place(&w, 0, 5, 7.5, 0);                  /* facing east */
  w.tank_in[0] = IN_FWD;
  uint16_t a0 = w.tank_ang[0];
  double x0 = cellx(&w, 0);
  for (int i = 0; i < 30; i++) sim_tick(&w);
  check(w.tank_ang[0] == a0, "heading unchanged in open space");
  check(cellx(&w, 0) > x0 + 1.0, "moved east");
}

static void t_flat_wall_slide(void) {
  printf("angled into flat wall, hold forward:\n");
  World w; sim_init(&w); arena_open(&w);
  for (int c = 1; c <= 18; c++) wall(&w, c, 9, 1);   /* horizontal wall */
  place(&w, 0, 9.5, 7.9, 4);                          /* NE into the wall */
  w.tank_in[0] = IN_FWD;
  for (int i = 0; i < 40; i++) sim_tick(&w);
  check(w.tank_vx[0] != 0 && w.tank_vy[0] == 0, "ends sliding along wall (x only)");
}

static void t_headon(unsigned dir, int back, const char* label) {
  printf("head-on into wall (%s):\n", label);
  World w; sim_init(&w); arena_open(&w);
  for (int c = 1; c <= 18; c++) wall(&w, c, 9, 1);
  /* face straight at the wall: facing +y (dir 8) and forward, OR facing -y and reversing */
  place(&w, 0, 9.5, 8.4, dir);
  w.tank_in[0] = back ? IN_BACK : IN_FWD;
  int stuck = run_max_stuck(&w, 200);
  check(stuck < 150, "rotates off the wall (not permanently stuck)");
  check(w.tank_vx[0] != 0 || w.tank_vy[0] != 0, "moving by the end");
}

static void t_convex_corner(void) {
  printf("convex (inside) corner, hold forward:\n");
  World w; sim_init(&w); arena_open(&w);
  for (int r = 4; r <= 10; r++) wall(&w, 10, r, 1);   /* vertical wall col 10 */
  for (int c = 4; c <= 10; c++) wall(&w, c, 10, 1);   /* horizontal wall row 10 */
  place(&w, 0, 9.4, 9.4, 4);                           /* driving SE into the corner */
  w.tank_in[0] = IN_FWD;
  int stuck = run_max_stuck(&w, 400);
  check(stuck < 150, "turns out of the corner (not permanently stuck)");
}

static void t_pocket(int back, const char* label) {
  printf("concave U-pocket, drive into closed end (%s):\n", label);
  World w; sim_init(&w); arena_open(&w);
  /* horizontal dead-end slot on row 7, cols 10..13, capped at the east (col 14),
   * open to the west. */
  for (int c = 9; c <= 14; c++) { wall(&w, c, 6, 1); wall(&w, c, 8, 1); }
  wall(&w, 14, 7, 1);
  /* face the closed (east) end; forward drives in, reverse needs facing west */
  unsigned dir = back ? 16u : 0u;
  place(&w, 0, 13.4, 7.5, dir);
  w.tank_in[0] = back ? IN_BACK : IN_FWD;
  int escaped = 0;
  for (int i = 0; i < 600 && !escaped; i++) { sim_tick(&w); if (cellx(&w, 0) < 8.5) escaped = 1; }
  check(escaped, "rotates around and exits the pocket to the west");
}

static void t_map_corner(void) {
  printf("arena (border) corner, hold forward:\n");
  World w; sim_init(&w); arena_open(&w);
  place(&w, 0, 1.6, 1.6, 20);                /* NW into the top-left corner */
  w.tank_in[0] = IN_FWD;
  int stuck = run_max_stuck(&w, 400);
  check(stuck < 150, "turns out of the map corner (not permanently stuck)");
}

static void t_default_map_roam(void) {
  printf("default map, hold forward 3000 ticks:\n");
  World w; sim_init(&w);                      /* the real level with its blocks */
  w.tank_in[0] = IN_FWD;
  int stuck = run_max_stuck(&w, 3000);
  check(stuck < 150, "never permanently stuck anywhere in the real map");
  printf("  (longest stationary run: %d ticks)\n", stuck);
}

static void t_no_corner_trap(void) {
  printf("default map, forward from the start, never trapped:\n");
  World w; sim_init(&w);                      /* real level, start pose */
  w.tank_in[0] = IN_FWD;
  for (int i = 0; i < 1500; i++) sim_tick(&w);   /* settle into wall-following */
  double xmin = 99, xmax = -99, ymin = 99, ymax = -99;
  for (int i = 0; i < 2000; i++) {
    sim_tick(&w);
    double x = cellx(&w, 0), y = celly(&w, 0);
    if (x < xmin) xmin = x; if (x > xmax) xmax = x;
    if (y < ymin) ymin = y; if (y > ymax) ymax = y;
  }
  /* if it were stuck in a corner the bbox would be tiny; it should roam widely */
  check((xmax - xmin) > 10.0 && (ymax - ymin) > 8.0,
        "roams the map (not trapped in a corner)");
  printf("  (roaming bbox: %.1f x %.1f cells)\n", xmax - xmin, ymax - ymin);
}

static void t_wrap_through(void) {
  printf("opposite edges open, drive into the gap:\n");
  World w; sim_init(&w); arena_open(&w);
  for (int r = 0; r < GRID_H; r++) wall(&w, 10, r, 0);   /* clear all of column 10, incl. both borders */
  place(&w, 0, 10.5, 7.0, 8);                             /* facing +y (down) */
  w.tank_in[0] = IN_FWD;
  int wrapped = 0;
  for (int i = 0; i < 500 && !wrapped; i++) {
    sim_tick(&w);
    if (celly(&w, 0) < 1.0) wrapped = 1;                  /* reached the top => wrapped past the bottom */
  }
  check(wrapped, "passes through the bottom and reappears at the top");
}

static void t_wrap_blocked(void) {
  printf("near edge open, far edge wall, drive into the gap:\n");
  World w; sim_init(&w); arena_open(&w);
  for (int r = 1; r <= 14; r++) wall(&w, 10, r, 0);       /* clear column 10 channel + bottom border */
  /* top border (10,0) left as a wall: the far side of the wrap is solid */
  place(&w, 0, 10.5, 7.0, 8);
  w.tank_in[0] = IN_FWD;
  double ymin = 99, ymax = -99;
  for (int i = 0; i < 600; i++) {
    sim_tick(&w);
    double y = celly(&w, 0);
    if (y < ymin) ymin = y; if (y > ymax) ymax = y;
  }
  check(ymin > 0.8, "collides with the wrapped far wall (never reaches the top)");
  check((ymax - ymin) > 8.0, "no oscillation: bounces the channel instead of jittering in place");
}

static void t_no_throttle(void) {
  printf("against wall, no throttle:\n");
  World w; sim_init(&w); arena_open(&w);
  for (int c = 1; c <= 18; c++) wall(&w, c, 9, 1);
  place(&w, 0, 9.5, 8.4, 8);                  /* facing into the wall, no input */
  w.tank_in[0] = 0;
  uint16_t a0 = w.tank_ang[0]; int16_t x0 = w.tank_x[0], y0 = w.tank_y[0];
  for (int i = 0; i < 30; i++) sim_tick(&w);
  check(w.tank_ang[0] == a0, "no auto-steer when not driving");
  check(w.tank_x[0] == x0 && w.tank_y[0] == y0, "stays put when not driving");
}

int main(void) {
  t_open_space();
  t_flat_wall_slide();
  t_headon(8, 0, "forward, facing +y");
  t_headon(24, 1, "reverse, facing -y");
  t_convex_corner();
  t_pocket(0, "forward");
  t_pocket(1, "reverse");
  t_map_corner();
  t_default_map_roam();
  t_no_corner_trap();
  t_wrap_through();
  t_wrap_blocked();
  t_no_throttle();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
