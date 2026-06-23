/* test.c — native (host) tests for the chapter-2 simulation core. No wasm, no
 * render, no GPU. Covers the pathing tables (Level 1 / Level 2), the byte-fit,
 * the route behaviour, the tank state machine (CONTRACT.md), and that the
 * inherited movement model still holds on the big toroidal grid.
 *
 * Build & run: ./test.sh */

#include "sim.h"
#include "collide.h"
#include "grid_paths.h"
#include "edge_paths.h"
#include "tanks_move.h"
#include <stdio.h>

static World W;                 /* static: the World is large, too big for the stack */
static void place_tank(World* w, uint32_t t, int wcx, int wcy);
static int g_checks = 0, g_fails = 0;
static void check(int cond, const char* msg) {
  g_checks++;
  if (!cond) { g_fails++; printf("  FAIL: %s\n", msg); }
  else       { printf("  ok:   %s\n", msg); }
}

/* ---- helpers ------------------------------------------------------------- */
static uint32_t wcell(int wcx, int wcy) { return (uint32_t)(wrap_wcy(wcy) * BIG_W + wrap_wcx(wcx)); }
/* a known-open ring cell (1,7) of screen (sx,sy) */
static uint32_t ring_wcx(int sx) { return (uint32_t)(sx * GRID_W + 1); }
static uint32_t ring_wcy(int sy) { return (uint32_t)(sy * GRID_H + 7); }
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
/* brute-force shortest distance over the whole BIG_W x BIG_H toroidal grid */
static int big_bfs(World* w, uint32_t src, uint32_t dst) {
  static int bd[BIG_W * BIG_H]; static uint32_t q[BIG_W * BIG_H];
  for (int i = 0; i < BIG_W * BIG_H; i++) bd[i] = -1;
  uint32_t head = 0, tail = 0; bd[src] = 0; q[tail++] = src;
  while (head < tail) { uint32_t c = q[head++]; int cx = c % BIG_W, cy = c / BIG_W;
    for (int d = 0; d < 4; d++) { uint32_t n = wcell(cx + CARD_DCX[d], cy + CARD_DCY[d]);
      if (bd[n] != -1) continue; if (cell_is_wall(w->grid, n % BIG_W, n / BIG_W)) continue;
      bd[n] = bd[c] + 1; q[tail++] = n; } }
  return bd[dst];
}
static void place_tank(World* w, uint32_t t, int wcx, int wcy) {
  w->tank_xy[t] = xy_pack(wcx * SUB + SUB / 2, wcy * SUB + SUB / 2);
  w->tank_ang[t] = 0; w->tank_in[t] = 0; w->tank_vxy[t] = 0; w->tank_hit[t] = 0;
}
/* drive tank `t` (UNSELECTED, so it follows its own path) to a destination */
static void run_to_dest(World* w, uint32_t t, int wcx, int wcy, int maxticks,
                        int* arrived, int* footbad, int* ticks) {
  sim_set_dest(w, t, (uint32_t)wrap_wcx(wcx), (uint32_t)wrap_wcy(wcy));
  *arrived = 0; *footbad = 0; *ticks = maxticks;
  for (int i = 0; i < maxticks; i++) {
    sim_tick(w);
    if (footprint_in_wall(w, t)) *footbad = 1;
    if (w->pstatus[t] == PS_ARRIVED) { *arrived = 1; *ticks = i; break; }
    if (w->pstatus[t] == PS_NOPATH) { *ticks = i; break; }
  }
}

/* ---- Level 1 ------------------------------------------------------------- */
static void t_level1(void) {
  printf("Level 1 — per-screen all-pairs distance:\n");
  sim_init(&W);
  const uint8_t* L1 = W.l1dist + 0 * TRI;
  int sym = 1;
  for (uint32_t a = 0; a < N_CELLS; a += 7) for (uint32_t b = 0; b < N_CELLS; b += 5)
    if (l1_get(L1, a, b) != l1_get(L1, b, a)) sym = 0;
  check(sym, "distance is symmetric: dist(a,b) == dist(b,a)");

  uint32_t src = 1 * GRID_W + 1, dst = 13 * GRID_W + 18;   /* two open ring cells of screen 0 */
  check(l1_reachable(L1, src, dst), "ring cells are reachable");
  uint32_t c = src; int steps = 0;
  for (; c != dst && steps <= (int)N_CELLS; steps++) {
    uint8_t d = l1_next_dir(L1, c, dst); if (d == DIR_NONE) break;
    int cx = c % GRID_W + CARD_DCX[d], cy = c / GRID_W + CARD_DCY[d]; c = (uint32_t)(cy * GRID_W + cx);
  }
  check(c == dst, "next_dir walk reaches the target");
  check(steps == l1_get(L1, src, dst), "next_dir walk length == stored distance (shortest)");
}

/* ---- Level-1 distances fit a byte ---------------------------------------- */
static void t_byte_fit(void) {
  printf("Level-1 distances fit a byte (provable ceiling + measured max):\n");
  sim_init(&W);
  check(L1_DIST_CEIL < L1_INF, "2x2-block ceiling is below the byte sentinel");
  printf("  (provable ceiling %d; default map measured max %u)\n", L1_DIST_CEIL, W.l1_maxdist);
  check(W.l1_maxdist < L1_INF, "default map: measured longest distance fits a byte");
  for (int r = 0; r < GRID_H; r++) W.grid[r] = (1u << GRID_W) - 1u;
  int rows[7] = {1,3,5,7,9,11,13}; uint32_t corridor = 0;
  for (int cc = 1; cc <= 18; cc++) corridor |= 1u << cc;
  for (int i = 0; i < 7; i++) W.grid[rows[i]] &= ~corridor;
  int conn[6][2] = {{2,18},{4,1},{6,18},{8,1},{10,18},{12,1}};
  for (int i = 0; i < 6; i++) W.grid[conn[i][0]] &= ~(1u << conn[i][1]);
  uint32_t mx = l1_build_screen(W.l1dist, W.grid, 0);
  printf("  (serpentine screen measured max %u)\n", mx);
  check(mx > 80 && mx < L1_INF, "long serpentine path is large but still fits/detected within a byte");
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
    if (W.ep_partner[pj] != i) partners_ok = 0;
    uint32_t wc = ep_world_cell(&W, i);
    int nx = wrap_wcx((int)(wc % BIG_W) + CARD_DCX[W.ep_cross[i]]);
    int ny = wrap_wcy((int)(wc / BIG_W) + CARD_DCY[W.ep_cross[i]]);
    if (ep_world_cell(&W, pj) != wcell(nx, ny)) cross_ok = 0;
    if (cell_is_wall(W.grid, wc % BIG_W, wc / BIG_W) || cell_is_wall(W.grid, nx, ny)) cross_ok = 0;
  }
  check(partners_ok, "every edge point has a mutual partner across its border");
  check(cross_ok, "partner is the matched open cell across the border");

  int compose_ok = 1, checked = 0;
  for (uint32_t a = 0; a < W.ep_count; a += 3) for (uint32_t b = 0; b < W.ep_count; b += 5) {
    uint16_t d2 = W.dist2[a * N_EDGE_MAX + b]; if (d2 == (uint16_t)D2_INF) continue; checked++;
    uint32_t cur = a, sum = 0, guard = 0;
    while (cur != b && guard++ < N_EDGE_MAX) {
      uint8_t nx = W.nexthop[cur * N_EDGE_MAX + b]; if (nx == 0xFF) { compose_ok = 0; break; }
      if (W.ep_partner[cur] == nx) sum += 1;
      else { const uint8_t* L1 = W.l1dist + (uint32_t)W.ep_screen[cur] * TRI; sum += l1_get(L1, W.ep_cell[cur], W.ep_cell[nx]); }
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
  struct { int sx, sy; const char* what; } dests[] = {
    { 0, 0, "same screen" }, { 1, 1, "to screen (1,1)" }, { 2, 2, "across two screens" },
    { 0, 3, "across the top/bottom wrap" }, { 3, 0, "across the left/right wrap" },
  };
  for (unsigned k = 0; k < sizeof(dests)/sizeof(dests[0]); k++) {
    int sx0 = 1, sy0 = 7;                                /* start: screen 0 ring (1,7) */
    int dx = (int)ring_wcx(dests[k].sx), dy = (int)ring_wcy(dests[k].sy);
    if (k == 0) { dx = 18; dy = 0 * GRID_H + 13; }       /* same-screen: a different ring cell */
    place_tank(&W, 1, sx0, sy0);
    sim_set_dest(&W, 1, (uint32_t)dx, (uint32_t)dy);
    uint32_t n = path_trace(&W, 1, trace, BIG_W * BIG_H);
    int true_d = big_bfs(&W, wcell(sx0, sy0), wcell(dx, dy));
    char msg[96];
    snprintf(msg, sizeof msg, "%s: trace reaches dest", dests[k].what);
    check(n > 0 && trace[n - 1] == wcell(dx, dy), msg);
    snprintf(msg, sizeof msg, "%s: length %d == BFS shortest %d", dests[k].what, (int)n - 1, true_d);
    check((int)n - 1 == true_d, msg);
  }
}

/* ---- the live tank reaches reachable targets, wall-free ------------------- */
static void t_drive_reachable(void) {
  printf("a tank drives to reachable targets, never entering a wall:\n");
  int arrived, footbad, ticks;
  sim_init(&W);
  run_to_dest(&W, 0, 18, 13, 6000, &arrived, &footbad, &ticks);                  /* same screen */
  check(arrived, "same-screen target reached");
  check(!footbad, "footprint never overlapped a wall (same screen)");
  check(tank_wcell(&W, 0) == wcell(18, 13), "stopped on the destination cell");

  sim_init(&W);
  run_to_dest(&W, 0, (int)ring_wcx(2), (int)ring_wcy(2), 16000, &arrived, &footbad, &ticks); /* two screens */
  check(arrived, "cross-screen target reached (across matched openings)");
  check(!footbad, "footprint never overlapped a wall (cross-screen)");

  sim_init(&W);
  run_to_dest(&W, 0, (int)ring_wcx(0), (int)ring_wcy(3), 16000, &arrived, &footbad, &ticks); /* wrap */
  check(arrived, "target across the outer toroidal wrap reached");
  check(!footbad, "footprint never overlapped a wall (wrap)");
}

/* ---- unreachable target is reported, not looped on ----------------------- */
static void t_unreachable(void) {
  printf("unreachable target is reported (PS_NOPATH), not looped on:\n");
  sim_init(&W);
  int cx = 3, cy = 3;                                    /* an open interior cell of screen 0 */
  W.grid[cy - 1] |= (1u << cx); W.grid[cy + 1] |= (1u << cx);
  W.grid[cy] |= (1u << (cx + 1)); W.grid[cy] |= (1u << (cx - 1));
  W.l1_screen_max[0] = (uint16_t)l1_build_screen(W.l1dist, W.grid, 0);
  l2_build(&W); for (uint32_t t = 0; t < N_TANKS; t++) l2_compute_pg(&W, t);

  int arrived, footbad, ticks; uint32_t before = W.tank_xy[0];
  run_to_dest(&W, 0, 3, 3, 1500, &arrived, &footbad, &ticks);
  check(!arrived && W.pstatus[0] == PS_NOPATH, "sealed cell reported as no-path");
  check(!footbad, "no-path tank never drove into a wall");
  check(W.tank_xy[0] == before, "no-path tank stays put (no spin/grind)");
}

/* ---- the tank state machine (CONTRACT.md) -------------------------------- */
static void t_states(void) {
  printf("tank state machine — cycle / select / UNSELECTED keeps pathing:\n");
  sim_init(&W);
  check(W.selected == SEL_NONE, "nothing selected at start");
  sim_cycle_tank(&W, 2);
  check(W.selected == 2 && W.tstate[2] == TS_AUTOPATH, "click an unselected tank -> selected, AUTOPATH");
  sim_cycle_tank(&W, 2);
  check(W.tstate[2] == TS_MANUAL, "click again -> MANUAL");
  sim_cycle_tank(&W, 2);
  check(W.tstate[2] == TS_UNSELECTED && W.selected == SEL_NONE, "click again -> UNSELECTED, deselected");
  sim_cycle_tank(&W, 1); sim_cycle_tank(&W, 3);
  check(W.selected == 3 && W.tstate[1] == TS_UNSELECTED && W.tstate[3] == TS_AUTOPATH,
        "selecting another tank deselects the first");

  /* selecting another tank leaves the first UNSELECTED but still pathing */
  sim_init(&W);
  sim_cycle_tank(&W, 1);                                 /* AUTOPATH */
  sim_set_dest(&W, 1, ring_wcx(1), ring_wcy(1));
  sim_cycle_tank(&W, 2);                                 /* select tank 2: tank 1 -> UNSELECTED, keeps dest */
  check(W.phas[1] == 1 && W.tstate[1] == TS_UNSELECTED, "selecting another tank leaves the first pathing (keeps dest)");
  int arrived = 0;
  for (int i = 0; i < 16000 && !arrived; i++) { sim_tick(&W); if (W.pstatus[1] == PS_ARRIVED) arrived = 1; }
  check(arrived, "UNSELECTED tank that was auto-pathing still reaches it");

  /* taking MANUAL control abandons the destination — unselecting then stays put */
  sim_init(&W);
  sim_cycle_tank(&W, 1);                                 /* AUTOPATH */
  sim_set_dest(&W, 1, ring_wcx(2), ring_wcy(2));         /* a cross-screen dest */
  sim_cycle_tank(&W, 1);                                 /* -> MANUAL */
  check(W.phas[1] == 0, "entering MANUAL abandons the destination");
  sim_cycle_tank(&W, 1);                                 /* -> UNSELECTED */
  uint32_t xy1 = W.tank_xy[1];
  W.tank_in[1] = 0; for (int i = 0; i < 200; i++) sim_tick(&W);
  check(W.tank_xy[1] == xy1, "after MANUAL + unselect, the tank stays put (no auto-path back)");

  /* a MANUAL tank ignores its destination and is driven by its input */
  sim_init(&W);
  sim_set_dest(&W, 0, ring_wcx(1), ring_wcy(1));
  W.tstate[0] = TS_MANUAL; W.selected = 0;
  uint32_t xy0 = W.tank_xy[0];
  W.tank_in[0] = 0; for (int i = 0; i < 60; i++) sim_tick(&W);
  check(W.tank_xy[0] == xy0, "MANUAL tank with a destination but no input stays put (no auto-path)");
}

/* ---- inherited movement model still holds on the big grid ---------------- */
static void t_inherited_movement(void) {
  printf("inherited movement model (manual driving) on the big grid:\n");
  sim_init(&W);
  place_tank(&W, 0, 1, 7); W.tank_ang[0] = 0;            /* screen 0 ring, facing east */
  W.tstate[0] = TS_MANUAL; W.selected = 0; W.tank_in[0] = IN_FWD;
  int moved = 0;
  for (int i = 0; i < 20; i++) { uint32_t b = W.tank_xy[0]; sim_tick(&W); if (W.tank_xy[0] != b) moved = 1; }
  check(moved, "manual tank moves under throttle");

  /* holding forward, the inherited auto-steer keeps it from getting stuck */
  int maxstuck = 0, cur = 0;
  for (int i = 0; i < 3000; i++) { uint32_t b = W.tank_xy[0]; sim_tick(&W);
    if (W.tank_xy[0] == b) { cur++; if (cur > maxstuck) maxstuck = cur; } else cur = 0; }
  check(maxstuck < 300, "manual tank never permanently stuck (auto-steer) on the big grid");
}

int main(void) {
  t_level1();
  t_byte_fit();
  t_level2();
  t_shortest();
  t_drive_reachable();
  t_unreachable();
  t_states();
  t_inherited_movement();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
