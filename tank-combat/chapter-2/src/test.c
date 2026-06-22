/* test.c — native (host) tests for the chapter-2 simulation core. No wasm, no
 * render, no GPU. Covers the pathing tables (Level 1 / Level 2), the route
 * behaviour contract (CONTRACT.md), and that the inherited movement model still
 * holds on the big toroidal grid.
 *
 * Build & run: ./test.sh */

#include "sim.h"
#include "collide.h"
#include "grid_paths.h"
#include "edge_paths.h"
#include "tanks_move.h"
#include <stdio.h>

static World W;                 /* static: the World is ~1.5 MB, too big for the stack */
static void place_tank_for_test(World* w, uint32_t t, int wcx, int wcy);
static int g_checks = 0, g_fails = 0;
static void check(int cond, const char* msg) {
  g_checks++;
  if (!cond) { g_fails++; printf("  FAIL: %s\n", msg); }
  else       { printf("  ok:   %s\n", msg); }
}

/* ---- helpers ------------------------------------------------------------- */
static uint32_t wcell(int wcx, int wcy) { return (uint32_t)(wrap_wcy(wcy) * BIG_W + wrap_wcx(wcx)); }
static uint32_t tank_wcell(World* w, uint32_t t) {
  return wcell(xy_lo(w->tank_xy[t]) >> SUB_SHIFT, xy_hi(w->tank_xy[t]) >> SUB_SHIFT);
}
static int footprint_in_wall(World* w, uint32_t t) {
  int x = xy_lo(w->tank_xy[t]), y = xy_hi(w->tank_xy[t]);
  for (int dx = -TANK_R; dx <= TANK_R; dx += 2 * TANK_R)
    for (int dy = -TANK_R; dy <= TANK_R; dy += 2 * TANK_R)
      if (cell_is_wall(w->grid, (x + dx) >> SUB_SHIFT, (y + dy) >> SUB_SHIFT)) return 1;
  return 0;
}

/* brute-force shortest distance over the whole BIG_W x BIG_H toroidal grid
 * (open cells only) — the ground truth the hierarchical route must match. */
static int big_bfs(World* w, uint32_t src, uint32_t dst) {
  static int bd[BIG_W * BIG_H];
  static uint32_t q[BIG_W * BIG_H];
  for (int i = 0; i < BIG_W * BIG_H; i++) bd[i] = -1;
  uint32_t head = 0, tail = 0;
  bd[src] = 0; q[tail++] = src;
  while (head < tail) {
    uint32_t c = q[head++]; int cx = c % BIG_W, cy = c / BIG_W;
    for (int d = 0; d < 4; d++) {
      uint32_t n = wcell(cx + CARD_DCX[d], cy + CARD_DCY[d]);
      if (bd[n] != -1) continue;
      if (cell_is_wall(w->grid, n % BIG_W, n / BIG_W)) continue;
      bd[n] = bd[c] + 1; q[tail++] = n;
    }
  }
  return bd[dst];
}

/* drive a pathed tank to a destination; report arrival, wall-cleanliness, ticks */
static void run_to_dest(World* w, uint32_t tank, int wcx, int wcy,
                        int maxticks, int* arrived, int* footbad, int* ticks) {
  uint32_t p = tank - 1;
  sim_set_dest(w, tank, (uint32_t)wrap_wcx(wcx), (uint32_t)wrap_wcy(wcy));
  *arrived = 0; *footbad = 0; *ticks = maxticks;
  for (int i = 0; i < maxticks; i++) {
    sim_tick(w);
    if (footprint_in_wall(w, tank)) *footbad = 1;
    if (w->pstatus[p] == PS_ARRIVED) { *arrived = 1; *ticks = i; break; }
    if (w->pstatus[p] == PS_NOPATH) { *ticks = i; break; }
  }
}

/* ---- Level 1 ------------------------------------------------------------- */
static void t_level1(void) {
  printf("Level 1 — per-screen all-pairs distance:\n");
  sim_init(&W);
  const uint8_t* L1 = W.l1dist + 0 * TRI;      /* screen 0 */
  int sym = 1, tri_ok = 1;
  for (uint32_t a = 0; a < N_CELLS; a += 7)
    for (uint32_t b = 0; b < N_CELLS; b += 5) {
      if (l1_get(L1, a, b) != l1_get(L1, b, a)) sym = 0;
    }
  check(sym, "distance is symmetric: dist(a,b) == dist(b,a)");

  /* next_dir steps must reach the target in exactly dist steps, on open cells */
  uint32_t src = 7 * GRID_W + 2, dst = 9 * GRID_W + 17;   /* two open cells in screen 0 */
  if (l1_reachable(L1, src, dst)) {
    uint32_t c = src; int steps = 0;
    for (; c != dst && steps <= N_CELLS; steps++) {
      uint8_t d = l1_next_dir(L1, c, dst);
      if (d == DIR_NONE) break;
      int cx = c % GRID_W + CARD_DCX[d], cy = c / GRID_W + CARD_DCY[d];
      c = (uint32_t)(cy * GRID_W + cx);
    }
    check(c == dst, "next_dir walk reaches the target");
    check(steps == l1_get(L1, src, dst), "next_dir walk length == stored distance (shortest)");
  } else { check(0, "test cells reachable"); tri_ok = 0; }
  (void)tri_ok;
}

/* ---- Level-1 distances fit in a byte (sized to the real data) ------------ */
static void t_byte_fit(void) {
  printf("Level-1 distances fit a byte (provable ceiling + measured max):\n");
  sim_init(&W);
  check(L1_DIST_CEIL < L1_INF, "2x2-block ceiling is below the byte sentinel");
  printf("  (provable ceiling %d; default map measured max %u)\n", L1_DIST_CEIL, W.l1_maxdist);
  check(W.l1_maxdist < L1_INF, "default map: measured longest distance fits a byte");

  /* carve a 7-row serpentine into screen 0 to force a long shortest path; it
   * must still fit the byte (and we measure it, so a violation would be seen). */
  for (int r = 0; r < GRID_H; r++) W.grid[r] = (1u << GRID_W) - 1u;          /* fill screen 0 */
  int rows[7] = {1, 3, 5, 7, 9, 11, 13};
  uint32_t corridor = 0; for (int c = 1; c <= 18; c++) corridor |= 1u << c;
  for (int i = 0; i < 7; i++) W.grid[rows[i]] &= ~corridor;                  /* open corridors */
  int conn[6][2] = {{2,18},{4,1},{6,18},{8,1},{10,18},{12,1}};
  for (int i = 0; i < 6; i++) W.grid[conn[i][0]] &= ~(1u << conn[i][1]);     /* link them into one snake */
  uint32_t m = l1_build_screen(W.l1dist, W.grid, 0);
  printf("  (serpentine screen measured max %u)\n", m);
  check(m > 80 && m < L1_INF, "long serpentine path is large but still fits/detected within a byte");
}

/* ---- Level 2: edge points + next-hop matrix ------------------------------ */
static void t_level2(void) {
  printf("Level 2 — edge points + next-hop matrix:\n");
  sim_init(&W);
  printf("  (%u edge points)\n", W.ep_count);
  check(W.ep_count > 0 && W.ep_count <= N_EDGE_MAX, "edge points enumerated within the cap");

  int partners_ok = 1, cross_ok = 1;
  for (uint32_t i = 0; i < W.ep_count; i++) {
    uint8_t pj = W.ep_partner[i];
    if (pj == 0xFF) { partners_ok = 0; continue; }
    if (W.ep_partner[pj] != i) partners_ok = 0;          /* partnership is mutual */
    /* the partner sits exactly across the matched border, both cells open */
    uint32_t wc = ep_world_cell(&W, i);
    int nx = wrap_wcx((int)(wc % BIG_W) + CARD_DCX[W.ep_cross[i]]);
    int ny = wrap_wcy((int)(wc / BIG_W) + CARD_DCY[W.ep_cross[i]]);
    if (ep_world_cell(&W, pj) != wcell(nx, ny)) cross_ok = 0;
    if (cell_is_wall(W.grid, wc % BIG_W, wc / BIG_W) || cell_is_wall(W.grid, nx, ny)) cross_ok = 0;
  }
  check(partners_ok, "every edge point has a mutual partner across its border");
  check(cross_ok, "partner is the matched open cell across the border");

  /* the next-hop hop-distances equal the composed Level-1 + crossing distances:
   * walk nexthop from A to B, summing each step's weight (1 to a partner, else
   * the Level-1 distance between two same-screen edge cells), and compare. */
  int compose_ok = 1, checked = 0;
  for (uint32_t a = 0; a < W.ep_count; a += 3)
    for (uint32_t b = 0; b < W.ep_count; b += 5) {
      uint16_t d2 = W.dist2[a * N_EDGE_MAX + b];
      if (d2 == (uint16_t)D2_INF) continue;
      checked++;
      uint32_t cur = a, sum = 0, guard = 0;
      while (cur != b && guard++ < N_EDGE_MAX) {
        uint8_t nx = W.nexthop[cur * N_EDGE_MAX + b];
        if (nx == 0xFF) { compose_ok = 0; break; }
        if (W.ep_partner[cur] == nx) sum += 1;            /* a border crossing */
        else {
          const uint8_t* L1 = W.l1dist + (uint32_t)W.ep_screen[cur] * TRI;
          sum += l1_get(L1, W.ep_cell[cur], W.ep_cell[nx]);  /* a same-screen hop */
        }
        cur = nx;
      }
      if (cur != b || sum != d2) compose_ok = 0;
    }
  check(checked > 0 && compose_ok, "next-hop hop distances == composed Level-1 + crossing distances");
}

/* ---- routes are globally shortest (vs brute-force big-grid BFS) ----------- */
static void t_shortest(void) {
  printf("routes are globally shortest (hierarchical == big-grid BFS):\n");
  sim_init(&W);
  static uint16_t trace[BIG_W * BIG_H];
  /* test several start/dest pairs across screens and the wrap */
  struct { int sx, sy, dx, dy; const char* what; } cases[] = {
    { 10, 7, 17, 11, "same screen" },
    { 10, 7, 30, 22, "to screen (1,1)" },
    { 10, 7, 50, 37, "across two screens" },
    { 10, 7, 10, 52, "across the top/bottom wrap" },
    { 10, 7, 70, 7,  "across the left/right wrap" },
  };
  for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    /* park pathed tank 1 at the start cell, route to the dest */
    place_tank_for_test(&W, 1, cases[k].sx, cases[k].sy);
    sim_set_dest(&W, 1, (uint32_t)cases[k].dx, (uint32_t)cases[k].dy);
    uint32_t n = path_trace(&W, 0, trace, BIG_W * BIG_H);
    int true_d = big_bfs(&W, wcell(cases[k].sx, cases[k].sy), wcell(cases[k].dx, cases[k].dy));
    int reaches = n > 0 && trace[n - 1] == wcell(cases[k].dx, cases[k].dy);
    char msg[96];
    snprintf(msg, sizeof msg, "%s: trace reaches dest", cases[k].what);
    check(reaches, msg);
    snprintf(msg, sizeof msg, "%s: length %d == BFS shortest %d", cases[k].what, (int)n - 1, true_d);
    check((int)n - 1 == true_d, msg);
  }
}

/* helper for the shortest test: re-place a tank (test-only) */
static void place_tank_for_test(World* w, uint32_t t, int wcx, int wcy) {
  w->tank_xy[t] = xy_pack(wcx * SUB + SUB / 2, wcy * SUB + SUB / 2);
  w->tank_ang[t] = 0;
  w->tank_in[t] = 0; w->tank_vxy[t] = 0; w->tank_hit[t] = 0;
}

/* ---- the live tank reaches reachable targets, wall-free ------------------- */
static void t_drive_reachable(void) {
  printf("pathed tank drives to reachable targets, never entering a wall:\n");
  sim_init(&W);
  int arrived, footbad, ticks;

  run_to_dest(&W, 1, 17, 11, 4000, &arrived, &footbad, &ticks);     /* same screen */
  check(arrived, "same-screen target reached");
  check(!footbad, "footprint never overlapped a wall (same screen)");
  check(tank_wcell(&W, 1) == wcell(17, 11), "stopped on the destination cell");

  sim_init(&W);
  run_to_dest(&W, 1, 50, 37, 12000, &arrived, &footbad, &ticks);    /* two screens away */
  check(arrived, "cross-screen target reached (across matched inner edges)");
  check(!footbad, "footprint never overlapped a wall (cross-screen)");

  sim_init(&W);
  run_to_dest(&W, 1, 10, 52, 12000, &arrived, &footbad, &ticks);    /* across the outer wrap */
  check(arrived, "target across the outer toroidal wrap reached");
  check(!footbad, "footprint never overlapped a wall (wrap)");
}

/* ---- unreachable target is reported, not looped on ----------------------- */
static void t_unreachable(void) {
  printf("unreachable target is reported (PS_NOPATH), not looped on:\n");
  sim_init(&W);
  /* seal a 1-cell pocket in screen 0: wall all 4 neighbours of cell (5,5) so it
   * is its own island, then target it. */
  uint32_t s = 0;
  int cx = 5, cy = 5;
  W.grid[s * GRID_H + (cy - 1)] |= (1u << cx);
  W.grid[s * GRID_H + (cy + 1)] |= (1u << cx);
  W.grid[s * GRID_H + cy] |= (1u << (cx + 1));
  W.grid[s * GRID_H + cy] |= (1u << (cx - 1));
  l1_build_screen(W.l1dist + s * TRI, W.grid, s);
  l2_build(&W);
  for (uint32_t p = 0; p < N_PATHED; p++) l2_compute_pg(&W, p);

  int arrived, footbad, ticks;
  uint32_t before = W.tank_xy[1];
  run_to_dest(&W, 1, 5, 5, 1500, &arrived, &footbad, &ticks);
  check(!arrived && W.pstatus[0] == PS_NOPATH, "sealed cell reported as no-path");
  check(!footbad, "no-path tank never drove into a wall");
  check(W.tank_xy[1] == before, "no-path tank stays put (no spin/grind)");
}

/* ---- selection & destination are explicit and single-target -------------- */
static void t_contract_select_dest(void) {
  printf("selection & destination are explicit, single-target:\n");
  sim_init(&W);
  sim_set_dest(&W, 1, 17, 11);
  check(W.phas[0] == 1 && W.pdest_cell[0] == (uint16_t)(11 * GRID_W + 17), "destination set");
  sim_set_dest(&W, 1, 3, 3);                 /* replaces the prior one */
  check(W.pdest_cell[0] == (uint16_t)(3 * GRID_W + 3), "new destination replaces the old (single-target)");
  sim_clear_dest(&W, 1);
  check(W.phas[0] == 0, "destination cleared");

  sim_set_selected(&W, 2);
  check(W.selected == 2, "selection set");
  sim_set_selected(&W, 0);                   /* index 0 is manual: not selectable as a pathed target */
  check(W.selected == 2, "manual tank cannot be selected as a pathed target");
}

/* ---- the viewport is presentation only ----------------------------------- */
static void t_contract_viewport(void) {
  printf("viewport scrolling never changes the simulation:\n");
  sim_init(&W);
  sim_set_dest(&W, 1, 50, 37);
  for (int i = 0; i < 300; i++) sim_tick(&W);
  uint32_t xy = W.tank_xy[1]; uint16_t ang = W.tank_ang[1]; uint32_t fr = W.frame;
  sim_scroll(&W, 1, 0); sim_scroll(&W, 0, 1); sim_scroll(&W, -1, 0);
  check(W.cam_sx == 0 && W.cam_sy == 1, "camera moved by whole screens, toroidally");
  check(W.tank_xy[1] == xy && W.tank_ang[1] == ang && W.frame == fr,
        "scrolling did not touch tank state or the frame");
  /* a tank keeps pathing whether or not its screen is in view */
  int arrived, footbad, ticks; (void)footbad; (void)ticks;
  for (int i = 0; i < 12000 && W.pstatus[0] != PS_ARRIVED; i++) sim_tick(&W);
  arrived = (W.pstatus[0] == PS_ARRIVED);
  check(arrived, "off-screen tank still reached its destination");
}

/* ---- inherited movement model still holds on the big grid ---------------- */
static void t_inherited_movement(void) {
  printf("inherited movement model (manual tank) on the big grid:\n");
  sim_init(&W);
  /* open space: no auto-rotate, moves along heading */
  W.tank_ang[0] = 0; W.tank_xy[0] = xy_pack(40 * SUB + 128, 7 * SUB + 128);  /* an open cell */
  /* clear a horizontal run so it can roll east */
  uint16_t a0 = W.tank_ang[0];
  W.tank_in[0] = IN_FWD;
  int moved = 0;
  for (int i = 0; i < 20; i++) { uint32_t b = W.tank_xy[0]; sim_tick(&W); if (W.tank_xy[0] != b) moved = 1; }
  check(moved, "manual tank moves under throttle");
  (void)a0;

  /* the manual tank wraps across the whole world torus */
  sim_init(&W);
  place_tank_for_test(&W, 0, 10, 7);
  W.tank_ang[0] = 0;                         /* east */
  W.tank_in[0] = IN_FWD;
  int crossed_screen = 0;
  uint32_t s0 = (tank_wcell(&W, 0) % BIG_W) / GRID_W;
  for (int i = 0; i < 4000; i++) {
    sim_tick(&W);
    uint32_t s = (tank_wcell(&W, 0) % BIG_W) / GRID_W;
    if (s != s0) { crossed_screen = 1; break; }
  }
  check(crossed_screen, "manual tank drives across a screen border (one connected world)");
}

int main(void) {
  t_level1();
  t_byte_fit();
  t_level2();
  t_shortest();
  t_drive_reachable();
  t_unreachable();
  t_contract_select_dest();
  t_contract_viewport();
  t_inherited_movement();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
