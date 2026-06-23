/* test.c — native (host) tests for the chapter-3 simulation core. No wasm, no
 * render, no GPU. Covers the inherited chapter-1/2 movement + pathing (a
 * regression after the agent_turn/agent_move rename) AND the new swarm: the mite
 * pool & per-cell index, the crowding cap, the last-write-wins gossip, the
 * sense/seek/arrive behaviour, wall safety at MITE_R, and determinism.
 *
 * Build & run: ./test.sh */

#include "sim.h"
#include "collide.h"
#include "grid_paths.h"
#include "edge_paths.h"
#include "agent_move.h"
#include "mites.h"
#include <stdio.h>

static World W, W2;             /* static: the World is large, too big for the stack */
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
  check(W.tank_xy[0] == before, "no-path tank stays put");
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

/* ======================== chapter 3: the swarm ============================ */

/* place mite m at the centre of world cell (wcx,wcy), at rest, record empty */
static void mite_reset(World* w, uint32_t m, int wcx, int wcy) {
  wcx = wrap_wcx(wcx); wcy = wrap_wcy(wcy);
  w->mite_xy[m]  = xy_pack(wcx * SUB + SUB / 2, wcy * SUB + SUB / 2);
  w->mite_ang[m] = 0; w->mite_in[m] = 0; w->mite_vxy[m] = 0; w->mite_hit[m] = 0;
  w->mite_mode[m] = MM_WANDER;
  w->mite_cell[m] = wc_pack(wcx, wcy); w->mite_tgt[m] = w->mite_cell[m];
}
/* a controlled gossip stage: park all tanks and mites [keep,N) far away on open
 * ring cells (so only mites [0,keep) interact), and clear every record. */
static void gossip_setup(World* w, int keep) {
  for (uint32_t t = 0; t < N_TANKS; t++) place_tank(w, t, 41, 31 + (int)t); /* screen (2,2) ring col */
  for (uint32_t m = (uint32_t)keep; m < N_MITES; m++) mite_reset(w, m, 41, 7); /* screen (2,0) ring col */
  for (uint32_t m = 0; m < N_MITES; m++) {
    w->mite_rec_cell[0][m] = REC_EMPTY; w->mite_rec_cell[1][m] = REC_EMPTY;
    w->mite_rec_time[0][m] = 0;         w->mite_rec_time[1][m] = 0;
  }
  w->rec_buf = 0;
}
static void set_rec(World* w, uint32_t m, uint16_t cell, uint32_t time) {
  w->mite_rec_cell[w->rec_buf][m] = cell; w->mite_rec_time[w->rec_buf][m] = time;
}
static uint16_t get_rec_cell(World* w, uint32_t m) { return w->mite_rec_cell[w->rec_buf][m]; }
static uint32_t get_rec_time(World* w, uint32_t m) { return w->mite_rec_time[w->rec_buf][m]; }
static int mite_foot_in_wall(World* w, uint32_t m) {
  int x = xy_lo(w->mite_xy[m]), y = xy_hi(w->mite_xy[m]);
  for (int dx = -MITE_R; dx <= MITE_R; dx += 2 * MITE_R)
    for (int dy = -MITE_R; dy <= MITE_R; dy += 2 * MITE_R)
      if (cell_is_wall(w->grid, (x + dx) >> SUB_SHIFT, (y + dy) >> SUB_SHIFT)) return 1;
  return 0;
}
static uint32_t mite_hash(World* w) {
  uint32_t h = 2166136261u, b = w->rec_buf;
  for (uint32_t m = 0; m < N_MITES; m++) {
    h = (h ^ w->mite_xy[m]) * 16777619u;
    h = (h ^ w->mite_ang[m]) * 16777619u;
    h = (h ^ ((uint32_t)w->mite_mode[m] | ((uint32_t)w->mite_tgt[m] << 8))) * 16777619u;
    h = (h ^ w->mite_rec_cell[b][m]) * 16777619u;
    h = (h ^ w->mite_rec_time[b][m]) * 16777619u;
  }
  return h;
}

/* ---- pool & per-cell index ----------------------------------------------- */
static void t_mite_pool_index(void) {
  printf("mite pool & per-cell index:\n");
  sim_init(&W);
  uint32_t sum = 0, maxc = 0;
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) { sum += W.mite_cnt[c]; if (W.mite_cnt[c] > maxc) maxc = W.mite_cnt[c]; }
  check(sum == N_MITES, "index counts sum to N_MITES");
  check(maxc <= MITE_CAP, "spawn places <= MITE_CAP (4) mites per cell");

  int list_ok = 1, open_ok = 1, reach_ok = 1;
  for (uint32_t m = 0; m < N_MITES; m++) {
    uint32_t c = W.mite_cell[m];
    int found = 0; uint8_t k = W.mite_cnt[c] <= MITE_CAP ? W.mite_cnt[c] : MITE_CAP;
    for (uint8_t j = 0; j < k; j++) if (W.mite_list[c * MITE_CAP + j] == m) found++;
    if (found != 1) list_ok = 0;
    if (cell_is_wall(W.grid, wc_x(c), wc_y(c))) open_ok = 0;
    if (big_bfs(&W, wcell(1, 7), c) < 0) reach_ok = 0;
  }
  check(list_ok, "each mite appears exactly once in its cell's list");
  check(open_ok, "every mite spawned on an open cell");
  check(reach_ok, "every mite spawned on a cell reachable from the open ring");
}

/* ---- crowding cap (<= MITE_CAP centres per cell, every tick) -------------- */
static void t_mite_cap(void) {
  printf("crowding cap — at most MITE_CAP centres per cell, every tick:\n");
  sim_init(&W);
  int breach = 0, reached = 0;
  for (int i = 0; i < 4000; i++) {
    sim_tick(&W);
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) {
      if (W.mite_cnt[c] > MITE_CAP) breach = 1;
      if (W.mite_cnt[c] == MITE_CAP) reached = 1;
    }
  }
  check(!breach, "no cell ever exceeds the cap across 4000 natural ticks");
  check(reached, "cells do reach the cap (the bound is binding, not vacuous)");

  /* stress: force the whole swarm to converge on ONE cell every tick */
  sim_init(&W);
  uint16_t C = (uint16_t)wcell(10, 7);
  int breach2 = 0;
  for (int i = 0; i < 2500; i++) {
    for (uint32_t m = 0; m < N_MITES; m++) {
      W.mite_rec_cell[W.rec_buf][m] = C; W.mite_rec_time[W.rec_buf][m] = 1000000u + (uint32_t)i;
      W.mite_mode[m] = MM_SEEK;
    }
    sim_tick(&W);
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (W.mite_cnt[c] > MITE_CAP) breach2 = 1;
  }
  check(!breach2, "cap holds under forced convergence (many mites targeting one cell)");
}

/* ---- gossip: last-write-wins, double-buffered ---------------------------- */
static void t_mite_gossip(void) {
  printf("gossip — last-write-wins record, double-buffered:\n");
  const int L = 12;                                  /* a vertical chain on the open ring col */
  sim_init(&W); gossip_setup(&W, L);
  for (int i = 0; i < L; i++) mite_reset(&W, (uint32_t)i, 1, 1 + i);
  uint16_t PC = (uint16_t)wcell(30, 30);
  set_rec(&W, (uint32_t)(L - 1), PC, 100);           /* plant at the far end */

  mites_build_index(&W); mites_step(&W);             /* one gossip step (frozen positions) */
  int holders1 = 0; for (int i = 0; i < L; i++) if (get_rec_cell(&W, (uint32_t)i) == PC) holders1++;
  check(holders1 == 2, "one step spreads exactly one hop (reads the previous tick: order-independent)");

  for (int s = 0; s < L; s++) { mites_build_index(&W); mites_step(&W); }
  int all = 1; for (int i = 0; i < L; i++) if (get_rec_cell(&W, (uint32_t)i) != PC) all = 0;
  check(all, "the planted record propagates along the whole in-range chain");

  /* newer overwrites older; older never overwrites newer */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 50);
  set_rec(&W, 1, (uint16_t)wcell(30, 30), 100);
  mites_build_index(&W); mites_step(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(30, 30) && get_rec_time(&W, 0) == 100, "a newer record overwrites an older one on contact");
  check(get_rec_cell(&W, 1) == (uint16_t)wcell(30, 30) && get_rec_time(&W, 1) == 100, "an older record never overwrites a newer one (timestamps monotone)");

  /* a newer EMPTY record is a readable value that propagates */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 50);
  set_rec(&W, 1, REC_EMPTY, 100);
  mites_build_index(&W); mites_step(&W);
  check(get_rec_cell(&W, 0) == REC_EMPTY && get_rec_time(&W, 0) == 100, "a newer EMPTY record propagates (overwrites an older cell)");
}

/* ---- behaviour: sense / seek / arrive / erase / refresh / 80-20 ---------- */
static void t_mite_behaviour(void) {
  printf("behaviour — sense, arrive/erase, refresh, and the 80/20 seek split:\n");

  /* sense a tank within range -> record its cell, stamp now, seek */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 10, 7);                            /* tank 0 near the lone mite */
  mite_reset(&W, 0, 11, 7);
  W.frame = 500;
  mites_build_index(&W); mites_step(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(10, 7), "sensing a tank records its cell");
  check(get_rec_time(&W, 0) == 500, "the sighting is stamped this frame");
  check(W.mite_mode[0] == MM_SEEK, "a self-sensed tank is always sought");

  /* reach the recorded cell with NO tank -> erase + wander */
  sim_init(&W); gossip_setup(&W, 1);
  mite_reset(&W, 0, 10, 7);
  set_rec(&W, 0, (uint16_t)wcell(10, 7), 50); W.mite_mode[0] = MM_SEEK;
  W.frame = 600;
  mites_build_index(&W); mites_step(&W);
  check(get_rec_cell(&W, 0) == REC_EMPTY && get_rec_time(&W, 0) == 600, "reaching the recorded cell with no tank erases the record (stamped now)");
  check(W.mite_mode[0] == MM_WANDER, "after erasing, the mite reverts to wander");

  /* reach the recorded cell WITH a tank there -> refresh (sense=0 so the arrive
   * branch, not the sense branch, does it) */
  sim_init(&W); gossip_setup(&W, 1);
  W.mite_sense = 0;
  place_tank(&W, 0, 10, 8);
  mite_reset(&W, 0, 10, 7);
  set_rec(&W, 0, (uint16_t)wcell(10, 8), 50); W.mite_mode[0] = MM_SEEK;
  W.frame = 700;
  mites_build_index(&W); mites_step(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(10, 8) && get_rec_time(&W, 0) == 700, "reaching the recorded cell with a tank there refreshes it (stamped now)");

  /* the 80/20 die: deterministic boundaries (pseek 0 / 100) on adoption */
  for (int boundary = 0; boundary <= 1; boundary++) {
    int pseek = boundary ? 100 : 0;
    const int L = 10;
    sim_init(&W); gossip_setup(&W, L); W.mite_pseek = (uint8_t)pseek;
    for (int i = 0; i < L; i++) mite_reset(&W, (uint32_t)i, 1, 1 + i);
    set_rec(&W, (uint32_t)(L - 1), (uint16_t)wcell(30, 30), 100);
    for (int s = 0; s < L + 1; s++) { mites_build_index(&W); mites_step(&W); }
    int ok = 1; uint8_t want = boundary ? MM_SEEK : MM_WANDER;
    for (int i = 0; i < L - 1; i++) if (W.mite_mode[i] != want) ok = 0;   /* 0..L-2 are adopters */
    check(ok, boundary ? "P_SEEK=100: every adoption chooses seek" : "P_SEEK=0: every adoption chooses wander");
  }

  /* the 80/20 die: statistical (~80% seek on adoption) */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  W.mite_pseek = 80;
  mites_build_index(&W);                               /* positions frozen: build once */
  int seeks = 0, trials = 12000;
  for (int i = 0; i < trials; i++) {
    set_rec(&W, 0, (uint16_t)wcell(30, 30), 1000000u + (uint32_t)i);   /* A always newest */
    W.mite_rec_cell[W.rec_buf][1] = REC_EMPTY; W.mite_rec_time[W.rec_buf][1] = 0; /* B adopts each step */
    mites_step(&W);
    if (W.mite_mode[1] == MM_SEEK) seeks++;
  }
  int pct = seeks * 100 / trials;
  printf("  (seek share over %d forced adoptions: %d%%)\n", trials, pct);
  check(pct >= 75 && pct <= 85, "the 80/20 seek/wander split matches the RNG stream");
}

/* ---- wall safety at MITE_R ----------------------------------------------- */
static void t_mite_wall(void) {
  printf("wall safety — a mite's MITE_R footprint never overlaps a wall:\n");
  sim_init(&W);
  int footbad = 0;
  for (int i = 0; i < 6000 && !footbad; i++) {
    sim_tick(&W);
    for (uint32_t m = 0; m < N_MITES; m++) if (mite_foot_in_wall(&W, m)) { footbad = 1; break; }
  }
  check(!footbad, "the radius-parameterised collision serves the smaller body too (6000 ticks)");
}

/* ---- determinism: a pure function of seed + inputs ----------------------- */
static void t_mite_determinism(void) {
  printf("determinism — same seed + inputs => identical swarm state:\n");
  sim_init(&W); sim_init(&W2);
  for (int i = 0; i < 2000; i++) { sim_tick(&W); sim_tick(&W2); }
  check(mite_hash(&W) == mite_hash(&W2), "same seed: identical state hash after 2000 ticks");

  sim_init(&W); sim_set_seed(&W, 9999);
  uint32_t h_seed = mite_hash(&W);                     /* a different scatter */
  sim_init(&W2);                                       /* default seed (1337) */
  check(h_seed != mite_hash(&W2), "a different seed scatters the swarm differently");

  sim_init(&W2); sim_set_seed(&W2, 9999);
  for (int i = 0; i < 300; i++) { sim_tick(&W); sim_tick(&W2); }
  check(mite_hash(&W) == mite_hash(&W2), "re-seeding to the same seed is reproducible across ticks");
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
  t_mite_pool_index();
  t_mite_cap();
  t_mite_gossip();
  t_mite_behaviour();
  t_mite_wall();
  t_mite_determinism();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
