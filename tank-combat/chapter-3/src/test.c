/* test.c — native (host) tests for the chapter-3 simulation core. No wasm, no
 * render, no GPU. Covers the inherited chapter-1/2 movement + pathing (a
 * regression after the agent_turn/agent_move rename) AND the new swarm: the mite
 * pool & per-cell index, the crowding cap, the last-write-wins gossip, the
 * sense/hunt/home behaviour, the nests + shared route fields, wall safety at
 * MITE_R, and determinism.
 *
 * Build & run: ./test.sh */

#include "sim.h"
#include "collide.h"
#include "grid_paths.h"
#include "edge_paths.h"
#include "agent_move.h"
#include "mites.h"
#include "tanks_fire.h"
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
  /* match sim's place(): body east, turret resting east, no target/cooldown — so the
   * turret-turn-rate combat tests start from a known aim. */
  w->tank_turret[t] = 0; w->tank_cooldown[t] = 0; w->tank_target[t] = TGT_NONE;
  w->tank_tracer[t] = 0; w->tank_shot_cell[t] = 0;
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
    h = (h ^ ((uint32_t)w->mite_dest[m] << 3)) * 16777619u;
    h = (h ^ w->mite_rec_cell[b][m]) * 16777619u;
    h = (h ^ w->mite_rec_time[b][m]) * 16777619u;
    h = (h ^ ((uint32_t)w->mite_resp[m] << 5)) * 16777619u;   /* combat: alive/dead is state too */
  }
  return h;
}
/* the tanks' combat state, folded for the determinism check (turret/cooldown/target) */
static uint32_t tank_combat_hash(World* w) {
  uint32_t h = 2166136261u;
  for (uint32_t t = 0; t < N_TANKS; t++) {
    h = (h ^ w->tank_turret[t]) * 16777619u;
    h = (h ^ w->tank_cooldown[t]) * 16777619u;
    h = (h ^ ((uint32_t)w->tank_target[t] | ((uint32_t)w->tank_shot_cell[t] << 16))) * 16777619u;
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
    /* the index is sub-segment-slotted: mite m occupies exactly its own seg slot */
    if (W.mite_list[c * MITE_CAP + seg_of(m)] != m) list_ok = 0;
    if (cell_is_wall(W.grid, wc_x(c), wc_y(c))) open_ok = 0;
    if (big_bfs(&W, wcell(1, 7), c) < 0) reach_ok = 0;
  }
  check(list_ok, "each mite occupies its own (cell, sub-segment) slot");
  check(open_ok, "every mite spawned on an open cell");
  check(reach_ok, "every mite spawned on a cell reachable from the open ring");
}

/* ---- crowding cap (<= MITE_CAP centres per cell, every tick) -------------- */
static void t_mite_cap(void) {
  printf("crowding cap — at most MITE_CAP centres per cell, every tick:\n");
  sim_init(&W);
  int breach = 0, reached = 0, seg_collide = 0;
  for (int i = 0; i < 4000; i++) {
    sim_tick(&W);
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) {
      if (W.mite_cnt[c] > MITE_CAP) breach = 1;
      if (W.mite_cnt[c] == MITE_CAP) reached = 1;
    }
    /* the new invariant: at most one mite per (cell, sub-segment) — i.e. every live mite
     * sits in its own seg slot (a collision would leave one mite unslotted). */
    if ((i & 63) == 0) for (uint32_t m = 0; m < N_MITES; m++)
      if (!W.mite_resp[m] && W.mite_list[(uint32_t)W.mite_cell[m] * MITE_CAP + seg_of(m)] != m) seg_collide = 1;
  }
  check(!breach, "no cell ever exceeds the cap across 4000 natural ticks");
  check(reached, "cells do reach the cap (the bound is binding, not vacuous)");
  check(!seg_collide, "at most one mite per (cell, sub-segment) — every mite keeps its own slot");

  /* stress: force the whole swarm to hunt ONE cell every tick */
  sim_init(&W);
  uint16_t C = (uint16_t)wcell(10, 7);
  int breach2 = 0;
  for (int i = 0; i < 2500; i++) {
    for (uint32_t m = 0; m < N_MITES; m++) {
      W.mite_rec_cell[W.rec_buf][m] = C; W.mite_rec_time[W.rec_buf][m] = 1000000u + (uint32_t)i;
      W.mite_mode[m] = MM_HUNT; W.mite_dest[m] = C;
    }
    sim_tick(&W);
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (W.mite_cnt[c] > MITE_CAP) breach2 = 1;
  }
  check(!breach2, "cap holds under forced convergence (many mites hunting one cell)");
}

/* ---- gossip: last-write-wins, double-buffered ---------------------------- */
static void t_mite_gossip(void) {
  printf("gossip — last-write-wins record, double-buffered:\n");
  const int L = 12;                                  /* a vertical chain on the open ring col */
  sim_init(&W); gossip_setup(&W, L);
  for (int i = 0; i < L; i++) mite_reset(&W, (uint32_t)i, 1, 1 + i);
  uint16_t PC = (uint16_t)wcell(30, 30);
  set_rec(&W, (uint32_t)(L - 1), PC, 100);           /* plant at the far end */

  mites_build_index(&W); mites_records(&W);          /* one gossip step (frozen positions) */
  int holders1 = 0; for (int i = 0; i < L; i++) if (get_rec_cell(&W, (uint32_t)i) == PC) holders1++;
  check(holders1 == 2, "one step spreads exactly one hop (reads the previous tick: order-independent)");

  for (int s = 0; s < L; s++) { mites_build_index(&W); mites_records(&W); }
  int all = 1; for (int i = 0; i < L; i++) if (get_rec_cell(&W, (uint32_t)i) != PC) all = 0;
  check(all, "the planted record propagates along the whole in-range chain");

  /* newer overwrites older; older never overwrites newer */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 50);
  set_rec(&W, 1, (uint16_t)wcell(30, 30), 100);
  mites_build_index(&W); mites_records(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(30, 30) && get_rec_time(&W, 0) == 100, "a newer record overwrites an older one on contact");
  check(get_rec_cell(&W, 1) == (uint16_t)wcell(30, 30) && get_rec_time(&W, 1) == 100, "an older record never overwrites a newer one (timestamps monotone)");

  /* absence is not gossiped: a newer EMPTY record does NOT overwrite a peer's sighting */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 50);
  set_rec(&W, 1, REC_EMPTY, 100);
  mites_build_index(&W); mites_records(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(20, 20) && get_rec_time(&W, 0) == 50,
        "a newer EMPTY record does NOT overwrite a sighting (absence is not gossiped)");
}

/* ---- behaviour: sense / hunt / arrive / erase / refresh / 80-20 / interrupt - */
static void t_mite_behaviour(void) {
  printf("behaviour — sense->hunt, arrive/erase, refresh, the 80/20 hunt/home split:\n");

  /* sense a tank within range -> record its cell, stamp now, hunt */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 10, 7);                            /* tank 0 near the lone mite */
  mite_reset(&W, 0, 11, 7);
  W.frame = 500;
  mites_build_index(&W); mites_records(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(10, 7), "sensing a tank records its cell");
  check(get_rec_time(&W, 0) == 500, "the sighting is stamped this frame");
  check(W.mite_mode[0] == MM_HUNT && W.mite_dest[0] == (uint16_t)wcell(10, 7), "a self-sensed tank is always hunted");

  /* reach the recorded cell with NO tank -> erase + wander */
  sim_init(&W); gossip_setup(&W, 1);
  mite_reset(&W, 0, 10, 7);
  set_rec(&W, 0, (uint16_t)wcell(10, 7), 50); W.mite_mode[0] = MM_HUNT; W.mite_dest[0] = (uint16_t)wcell(10, 7);
  W.frame = 600;
  mites_build_index(&W); mites_records(&W);
  check(rec_is_gone(get_rec_cell(&W, 0)) && rec_cell_of(get_rec_cell(&W, 0)) == (uint16_t)wcell(10, 7) && get_rec_time(&W, 0) == 600,
        "reaching the recorded cell with no tank turns the record into a GONE-of-X (stamped now)");
  check(W.mite_mode[0] == MM_WANDER, "after finding the cell empty it stands down to wander");

  /* a GONE-of-X clears a peer HUNTING X, but never touches a peer hunting a different cell */
  sim_init(&W); gossip_setup(&W, 3); W.mite_sense = 0;          /* no sensing: pure gossip */
  mite_reset(&W, 0, 2, 7); mite_reset(&W, 1, 1, 7); mite_reset(&W, 2, 3, 7);  /* 0 sees both 1 and 2 */
  { uint16_t X = (uint16_t)wcell(40, 40), Y = (uint16_t)wcell(50, 50);
    set_rec(&W, 0, rec_gone_of(X), 200); W.mite_mode[0] = MM_WANDER;          /* 0 carries GONE-of-X */
    set_rec(&W, 1, X, 100); W.mite_mode[1] = MM_HUNT; W.mite_dest[1] = X;     /* 1 hunts X (older) */
    set_rec(&W, 2, Y, 100); W.mite_mode[2] = MM_HUNT; W.mite_dest[2] = Y;     /* 2 hunts Y (older) */
    mites_build_index(&W); mites_records(&W);
    check(rec_is_gone(get_rec_cell(&W, 1)) && W.mite_mode[1] == MM_WANDER,
          "a GONE-of-X is adopted by a mite hunting X -> it stands down");
    check(get_rec_cell(&W, 2) == Y && W.mite_mode[2] == MM_HUNT,
          "a GONE-of-X never touches a mite hunting a different cell (no cross-erase = no forgetting)"); }

  /* reach the recorded cell WITH a tank there -> refresh (sense=0 so the arrive
   * branch, not the sense branch, does it) */
  sim_init(&W); gossip_setup(&W, 1);
  W.mite_sense = 0;
  place_tank(&W, 0, 10, 8);
  mite_reset(&W, 0, 10, 7);
  set_rec(&W, 0, (uint16_t)wcell(10, 8), 50); W.mite_mode[0] = MM_HUNT; W.mite_dest[0] = (uint16_t)wcell(10, 8);
  W.frame = 700;
  mites_build_index(&W); mites_records(&W);
  check(get_rec_cell(&W, 0) == (uint16_t)wcell(10, 8) && get_rec_time(&W, 0) == 700, "reaching the recorded cell with a tank there refreshes it (stamped now)");

  /* the role die: deterministic boundaries (P_HUNT 0 -> home, 100 -> hunt) */
  for (int boundary = 0; boundary <= 1; boundary++) {
    int phunt = boundary ? 100 : 0;
    const int L = 10;
    sim_init(&W); gossip_setup(&W, L); W.mite_phunt = (uint8_t)phunt;
    for (int i = 0; i < L; i++) mite_reset(&W, (uint32_t)i, 1, 1 + i);
    set_rec(&W, (uint32_t)(L - 1), (uint16_t)wcell(30, 30), 100);
    for (int s = 0; s < L + 1; s++) { mites_build_index(&W); mites_records(&W); }
    int ok = 1; uint8_t want = boundary ? MM_HUNT : MM_HOME;
    for (int i = 0; i < L - 1; i++) if (W.mite_mode[i] != want) ok = 0;   /* 0..L-2 are adopters */
    check(ok, boundary ? "P_HUNT=100: every adoption chooses to hunt" : "P_HUNT=0: every adoption goes home");
  }

  /* the role die: statistical (~80% hunt on adoption) */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  W.mite_phunt = 80;
  mites_build_index(&W);                               /* positions frozen: build once */
  int hunts = 0, trials = 12000;
  for (int i = 0; i < trials; i++) {
    set_rec(&W, 0, (uint16_t)wcell(30, 30), 1000000u + (uint32_t)i);   /* A always newest */
    W.mite_rec_cell[W.rec_buf][1] = REC_EMPTY; W.mite_rec_time[W.rec_buf][1] = 0; /* B adopts each step */
    mites_records(&W);
    if (W.mite_mode[1] == MM_HUNT) hunts++;
  }
  int pct = hunts * 100 / trials;
  printf("  (hunt share over %d forced adoptions: %d%%)\n", trials, pct);
  check(pct >= 75 && pct <= 85, "the 80/20 hunt/home split matches the RNG stream");

  /* a newer record interrupts a HOMING mite and re-rolls it */
  sim_init(&W); gossip_setup(&W, 2); W.mite_phunt = 100;   /* re-roll deterministically -> hunt */
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  set_rec(&W, 0, (uint16_t)wcell(40, 40), 200);            /* peer 0 has the newer record */
  set_rec(&W, 1, (uint16_t)wcell(20, 20), 100);            /* mite 1 is homing on an older one */
  W.mite_mode[1] = MM_HOME; W.mite_dest[1] = W.nest_cell[nest_of(1)];
  mites_build_index(&W); mites_records(&W);
  check(W.mite_mode[1] == MM_HUNT && get_rec_cell(&W, 1) == (uint16_t)wcell(40, 40),
        "a newer record interrupts a homing mite and re-rolls (here -> hunt the new cell)");

  /* the jam detector: a hunter that cannot advance for MITE_STUCK_MAX ticks gives up and
   * wanders (so a knot of jammed mites dissolves instead of freezing forever) */
  sim_init(&W); gossip_setup(&W, 1);
  W.grid[7] |= (1u << 9) | (1u << 11);   /* box cell (10,7): force its four neighbours to walls */
  W.grid[6] |= (1u << 10);               /* (10,6) */
  W.grid[8] |= (1u << 10);               /* (10,8) */
  mite_reset(&W, 0, 10, 7);
  W.mite_mode[0] = MM_HUNT; W.mite_dest[0] = (uint16_t)wcell(30, 30); W.mite_stuck[0] = 0;
  int gaveup = 0, before = -1;
  for (int i = 0; i < MITE_STUCK_MAX + 2; i++) {
    mites_build_index(&W); mites_step(&W);
    if (i == MITE_STUCK_MAX - 2) before = (W.mite_mode[0] == MM_HUNT);   /* still hunting just before */
    if (W.mite_mode[0] == MM_WANDER) { gaveup = 1; break; }
  }
  check(before == 1, "a jammed hunter keeps hunting until the patience runs out");
  check(gaveup, "a hunter jammed for MITE_STUCK_MAX ticks gives up and wanders (knots dissolve)");
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
  /* firing is on by default, so this run already exercised combat: the turrets,
   * the kills, and the respawns must replay identically too (firing uses no RNG). */
  check(tank_combat_hash(&W) == tank_combat_hash(&W2), "same seed: identical tank combat state (turrets/kills) after 2000 ticks");

  sim_init(&W); sim_set_seed(&W, 9999);
  uint32_t h_seed = mite_hash(&W);                     /* a different scatter */
  sim_init(&W2);                                       /* default seed (1337) */
  check(h_seed != mite_hash(&W2), "a different seed scatters the swarm differently");

  sim_init(&W2); sim_set_seed(&W2, 9999);
  for (int i = 0; i < 300; i++) { sim_tick(&W); sim_tick(&W2); }
  check(mite_hash(&W) == mite_hash(&W2), "re-seeding to the same seed is reproducible across ticks");
}

/* ---- nests & shared route fields ----------------------------------------- */
static uint32_t wc_screen(uint32_t c) { return ((uint32_t)wc_y(c) / GRID_H) * SCREENS_X + ((uint32_t)wc_x(c) / GRID_W); }
static uint32_t wc_local (uint32_t c) { return ((uint32_t)wc_y(c) % GRID_H) * GRID_W + ((uint32_t)wc_x(c) % GRID_W); }

static void t_mite_nests_fields(void) {
  printf("nests & shared route fields:\n");

  /* nest_of partitions the swarm into NEST_COUNT equal groups */
  sim_init(&W);
  int cnt[NEST_COUNT]; for (int n = 0; n < NEST_COUNT; n++) cnt[n] = 0;
  int part_ok = 1;
  for (uint32_t m = 0; m < N_MITES; m++) { if (nest_of(m) != m % NEST_COUNT) part_ok = 0; cnt[nest_of(m)]++; }
  int even = 1; for (int n = 0; n < NEST_COUNT; n++) if (cnt[n] != (int)(N_MITES / NEST_COUNT)) even = 0;
  check(part_ok && even, "nest_of(i) = i % 4 partitions the swarm into four equal nests");

  /* a homing mite reaches its nest cell (mite 0's nest is (21,22) in screen (1,1);
   * start a few open cells along its row). Firing off: this checks navigation, not
   * combat — we don't want the test mite shot before it arrives. */
  sim_init(&W); gossip_setup(&W, 1); W.fire_period = 0;
  mite_reset(&W, 0, 28, 22);
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 50);             /* a record it carries home */
  W.mite_mode[0] = MM_HOME; W.mite_dest[0] = W.nest_cell[nest_of(0)];
  uint16_t home = W.nest_cell[nest_of(0)]; int reached_home = 0;
  for (int i = 0; i < 1200 && !reached_home; i++) { sim_tick(&W); if (W.mite_cell[0] == home) reached_home = 1; }
  check(reached_home, "a homing mite paths to its nest cell");
  /* on arrival it delivers the sighting and rejoins the wanderers — so it does not stay
   * stuck in MM_HOME (perpetually nest-coloured) with a stale record. */
  int became_wander = 0;
  for (int i = 0; i < 60 && !became_wander; i++) { sim_tick(&W); if (W.mite_mode[0] == MM_WANDER) became_wander = 1; }
  check(became_wander, "reaching its nest reverts the mite to wander (delivered, not stuck homing)");
  check(get_rec_cell(&W, 0) == REC_EMPTY, "the carried record is cleared on arriving home");

  /* a hunting mite reaches its recorded cell (a tank sits there, so it converges).
   * Firing off: tank 1 sits ON the goal, and would otherwise shoot the mite before
   * it arrives — here we test the navigation, not the combat. */
  sim_init(&W); gossip_setup(&W, 1); W.fire_period = 0;
  place_tank(&W, 1, 2, 7);                                 /* the sighting target (open corridor) */
  mite_reset(&W, 0, 5, 7);
  set_rec(&W, 0, (uint16_t)wcell(2, 7), 50); W.mite_mode[0] = MM_HUNT; W.mite_dest[0] = (uint16_t)wcell(2, 7);
  int reached_goal = 0;
  for (int i = 0; i < 1200 && !reached_goal; i++) { sim_tick(&W); if (cell_chebyshev(W.mite_cell[0], wcell(2, 7)) <= 1) reached_goal = 1; }
  check(reached_goal, "a hunting mite paths to within one cell of its recorded cell");

  /* a mite's shared-field route equals the tank pathing ground truth for the same
   * destination (the field IS the tank route, keyed by destination not by tank) */
  sim_init(&W);
  uint16_t D = (uint16_t)wcell(41, 37);                    /* an open ring cell of screen (2,2) */
  sim_set_dest(&W, 1, 41, 37);                             /* tank 1's own route to D */
  static uint16_t fld[N_EDGE_MAX];
  pg_fold(&W, fld, wc_screen(D), wc_local(D));
  int cmp = 0, mism = 0;
  for (int wcx = 1; wcx <= 78; wcx += 3) for (int wcy = 1; wcy <= 58; wcy += 3) {
    if (cell_is_wall(W.grid, wcx, wcy)) continue;
    uint32_t c = wcell(wcx, wcy), sc = wc_screen(c), lc = wc_local(c);
    uint8_t td = route_next_dir(&W, 1, sc, lc);
    uint8_t fd = route_next_dir_pg(&W, fld, wc_screen(D), wc_local(D), sc, lc);
    cmp++; if (td != fd) mism++;
  }
  check(cmp > 0 && mism == 0, "the shared field's route equals the tank pathing ground truth");

  /* the distinct-destination peak is measured and stays within N_FIELDS */
  sim_init(&W);
  int corners[4][2] = {{1,7},{78,7},{1,52},{78,52}};
  for (int t = 0; t < N_TANKS; t++) sim_set_dest(&W, t, corners[(t+1)%4][0], corners[(t+1)%4][1]);
  for (int i = 0; i < 6000; i++) {
    for (int t = 0; t < N_TANKS; t++) if (W.pstatus[t] == PS_ARRIVED || W.pstatus[t] == PS_NOPATH)
      sim_set_dest(&W, t, corners[(i/97+t)%4][0], corners[(i/97+t)%4][1]);
    sim_tick(&W);
  }
  printf("  (measured distinct-destination peak: %u of N_FIELDS=%d)\n", W.field_peak, N_FIELDS);
  check(W.field_peak > NEST_COUNT && W.field_peak < N_FIELDS, "the measured peak is binding and within N_FIELDS (sized from it)");

  /* forced overflow: more distinct destinations than N_FIELDS -> greedy fallback,
   * without breaking the cap or determinism */
  sim_init(&W); sim_init(&W2);
  int M = 200, overflowed = 0, breach = 0;
  for (int i = 0; i < 300; i++) {
    for (uint32_t m = 0; m < (uint32_t)M; m++) {
      uint16_t d = (uint16_t)((m * 17u) % N_WORLD_CELLS); uint32_t t = 2000000u + (uint32_t)i;
      W.mite_rec_cell[W.rec_buf][m]  = d; W.mite_rec_time[W.rec_buf][m]  = t; W.mite_mode[m]  = MM_HUNT;
      W2.mite_rec_cell[W2.rec_buf][m] = d; W2.mite_rec_time[W2.rec_buf][m] = t; W2.mite_mode[m] = MM_HUNT;
    }
    sim_tick(&W); sim_tick(&W2);
    if (W.field_active > (uint32_t)N_FIELDS) overflowed = 1;
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (W.mite_cnt[c] > MITE_CAP) breach = 1;
  }
  check(overflowed, "forced overflow: distinct destinations exceed N_FIELDS");
  check(!breach, "the cap still holds under field overflow (greedy fallback)");
  check(mite_hash(&W) == mite_hash(&W2), "the swarm stays deterministic under overflow");
}

/* ---- combat: the tanks shoot the swarm ----------------------------------- */
static void t_tanks_fire(void) {
  printf("combat — laser aim/line-kill/stop-at-wall, death cry, nest respawn:\n");

  /* a tank lasers the mite it can hit: the target is destroyed, a burst is spawned */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 2, 7); mite_reset(&W, 0, 5, 7);
  W.fire_period = 30; W.mite_respawn = 300;
  sim_tick(&W);
  check(W.tank_target[0] == 0, "the turret acquires the mite in line of sight");
  check(W.mite_resp[0] > 0, "the laser destroys the targeted mite");
  check(W.tank_cooldown[0] > 0, "the tank goes on cooldown after firing");
  int anyfx = 0; for (uint32_t i = 0; i < N_FX; i++) if (W.fx_t[i]) anyfx = 1;
  check(anyfx, "a destruction burst is spawned for the destroyed mite");
  sim_tick(&W);   /* the next index rebuild drops the corpse */
  check(W.mite_cnt[wcell(5, 7)] == 0, "a dead mite leaves the per-cell index (not drawn, not targetable)");

  /* the laser is a thin LINE: mites on the beam die, mites off it (spread into other
   * sub-segments) survive. Drive build_index + tanks_fire directly with hand-placed
   * positions so the test isn't perturbed by movement/snap this tick. */
  sim_init(&W); gossip_setup(&W, 3);
  place_tank(&W, 0, 1, 7);                                /* turret east, beam along row-7 centre */
  mite_reset(&W, 0, 4, 7); mite_reset(&W, 1, 6, 7); mite_reset(&W, 2, 5, 7);
  W.mite_xy[0] = xy_pack(4 * SUB + SUB / 2, 7 * SUB + SUB / 2);        /* on the line (perp 0) */
  W.mite_xy[1] = xy_pack(6 * SUB + SUB / 2, 7 * SUB + SUB / 2);        /* on the line */
  W.mite_xy[2] = xy_pack(5 * SUB + SUB / 2, 7 * SUB + SUB / 2 + 96);   /* off the line (perp 96 > BEAM_HW) */
  W.fire_period = 30; W.mite_respawn = 300;
  mites_build_index(&W); tanks_fire(&W);
  check(W.mite_resp[0] > 0 && W.mite_resp[1] > 0, "the laser destroys mites on its line");
  check(W.mite_resp[2] == 0, "a mite off the beam line survives — the thin beam misses it (the spread dodges)");

  /* the beam stops at a wall: a collinear mite beyond the wall survives */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 1, 7);
  mite_reset(&W, 0, 5, 7);  W.mite_xy[0] = xy_pack(5 * SUB + SUB / 2, 7 * SUB + SUB / 2);   /* before the block (col 8) */
  mite_reset(&W, 1, 13, 7); W.mite_xy[1] = xy_pack(13 * SUB + SUB / 2, 7 * SUB + SUB / 2);  /* beyond it (cols 8..11 walls) */
  W.fire_period = 30; W.mite_respawn = 300;
  mites_build_index(&W); tanks_fire(&W);
  check(W.mite_resp[0] > 0, "the laser destroys the mite before the wall");
  check(W.mite_resp[1] == 0, "a mite beyond the wall on the same line survives (the beam stops at the wall)");

  /* line of sight is required: a mite behind the central wall block is NOT targeted */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 5, 7); mite_reset(&W, 0, 15, 7);      /* cols 8..11 (walls) lie between them */
  W.fire_period = 30;
  sim_tick(&W);
  check(W.tank_target[0] == TGT_NONE, "a mite behind a wall is not targeted (line of sight required)");
  check(W.mite_resp[0] == 0, "the wall-shadowed mite survives");

  /* turret aims even when firing is off (fire_period 0): independent of the body, and
   * it SWINGS at a turn rate rather than snapping. Body east, mite due north, mite pinned. */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 2, 7);                                            /* body + turret east */
  mite_reset(&W, 0, 2, 4);                                            /* mite due north */
  W.fire_period = 0; W.mite_speed = 0;                                /* aim only; pin the mite */
  sim_tick(&W);
  check(W.tank_target[0] == 0, "fire off: the turret still acquires a target in sight");
  check((W.tank_turret[0] >> ANGLE_SHIFT) != CARD_DI[DIR_N], "the turret does not snap instantly — it has a turn rate");
  for (int i = 0; i < 12; i++) sim_tick(&W);                          /* let it swing onto the target */
  check(W.mite_resp[0] == 0, "fire off: no mite is killed");
  check((W.tank_turret[0] >> ANGLE_SHIFT) == CARD_DI[DIR_N], "the turret swings to aim north at a mite due north");
  check((W.tank_ang[0] >> ANGLE_SHIFT) == 0, "the body heading is unchanged (turret aims separately)");

  /* targeting picks the mite closest to the turret's CURRENT direction (most likely to
   * be hit), not the spatially nearest: turret east, a near mite due north vs a farther
   * mite due east -> the east (aligned) one wins even though it is farther and higher index. */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 2, 7);                                            /* turret east */
  mite_reset(&W, 0, 2, 5);                                            /* nearer (2 cells), due north */
  mite_reset(&W, 1, 6, 7);                                            /* farther (4 cells), due east */
  W.fire_period = 0;
  sim_tick(&W);
  check(W.tank_target[0] == 1, "targets the mite closest to the turret direction (east), not the nearest (north)");

  /* the turn rate GATES firing: a target 90 deg off the turret is not shot on tick 1;
   * the turret must swing onto it first (instant aim would have killed immediately). */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 2, 7);                                            /* turret east */
  mite_reset(&W, 0, 2, 4);                                            /* 90 deg off, due north */
  W.fire_period = 30; W.mite_respawn = 300; W.mite_speed = 0;         /* pin the mite */
  sim_tick(&W);
  check(W.tank_target[0] == 0, "acquires the off-axis target");
  check(W.mite_resp[0] == 0, "does not fire on tick 1 — the turret must swing on first (turn rate gates firing)");
  int swung = 0; for (int i = 0; i < 20 && !swung; i++) { sim_tick(&W); if (W.mite_resp[0]) swung = 1; }
  check(swung, "fires once the turret has swung onto the target");

  /* the turret is LOCKED while the laser is live: it cannot turn toward a new target
   * until the beam fades, then it is free again. */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 2, 7);                                            /* turret east */
  mite_reset(&W, 0, 5, 7);                                            /* due east -> fire east at once */
  W.fire_period = 30; W.mite_respawn = 300; W.mite_speed = 0;
  sim_tick(&W);
  check(W.tank_tracer[0] > 0, "the beam is live just after firing");
  uint16_t locked = W.tank_turret[0];
  mite_reset(&W, 1, 2, 4);                                            /* a target due north now appears */
  int held = 1;
  for (int i = 0; i < LASER_TICKS - 1; i++) { sim_tick(&W); if (W.tank_turret[0] != locked) held = 0; }
  check(held && W.tank_turret[0] == locked, "the turret cannot turn while the laser is active");
  int resumed = 0;
  for (int i = 0; i < 30 && !resumed; i++) { sim_tick(&W); if (W.tank_turret[0] != locked) resumed = 1; }
  check(resumed, "once the beam fades the turret is free to turn again");

  /* the death cry: a kill teaches nearby mites the firing tank's cell + hunt it */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 2, 7);
  mite_reset(&W, 0, 5, 7);                                /* the target (closest, lowest index) */
  mite_reset(&W, 1, 5, 8);                                /* a peer within 2x sensing range (=4) */
  W.mite_sense = 2; W.fire_period = 30; W.mite_respawn = 300;
  sim_tick(&W);
  check(W.mite_resp[0] > 0, "the closest in-sight mite is the one shot");
  check(get_rec_cell(&W, 1) == (uint16_t)wcell(2, 7), "death cry: a nearby mite learns the firing tank's cell");
  check(W.mite_mode[1] == MM_HUNT && W.mite_dest[1] == (uint16_t)wcell(2, 7),
        "death cry: the nearby mite turns to hunt the attacker");

  /* respawn: a killed mite revives at its nest after the timeout, once (cap permitting) */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 2, 7); mite_reset(&W, 0, 5, 7);
  W.fire_period = 30; W.mite_respawn = 10;
  int died = 0; for (int i = 0; i < 5 && !died; i++) { sim_tick(&W); if (W.mite_resp[0]) died = 1; }
  check(died, "the mite dies when shot (short respawn set)");
  /* move the tank far away so the mite isn't instantly re-killed the moment it
   * revives at its nest (a tank camping a nest legitimately suppresses it). */
  place_tank(&W, 0, 41, 31);
  uint16_t nest0 = W.nest_cell[nest_of(0)];
  int revived = 0; for (int i = 0; i < 60 && !revived; i++) { sim_tick(&W); if (!W.mite_resp[0]) revived = 1; }
  check(revived, "a dead mite revives after the respawn timeout");
  check(cell_chebyshev(W.mite_cell[0], nest0) <= MITE_REVIVE_R, "the revived mite reappears at (or next to) its nest cell");
  check(get_rec_cell(&W, 0) == REC_EMPTY, "a revived mite's memory (its record) is cleared");
  check(W.mite_mode[0] == MM_WANDER, "a revived mite starts wandering (no stale hunt/home)");

  /* revival is PARALLEL across a nest's sub-segments, not serialized: with the whole swarm
   * dead and reviving, a nest cell fills more than one sub-segment. (If seg_of were tied to
   * nest_of, every nest mite would want the same segment and only one could revive at once.) */
  sim_init(&W); W.fire_period = 0;
  for (uint32_t m = 0; m < N_MITES; m++) W.mite_resp[m] = 3;
  for (int i = 0; i < 30; i++) sim_tick(&W);
  int maxsegs = 0;
  for (int n = 0; n < NEST_COUNT; n++) {
    int occ = 0; for (int s = 0; s < MITE_CAP; s++) if (W.mite_list[(uint32_t)W.nest_cell[n] * MITE_CAP + s] != REC_EMPTY) occ++;
    if (occ > maxsegs) maxsegs = occ;
  }
  check(maxsegs >= 2, "a nest revives mites into multiple sub-segments in parallel (seg decoupled from nest)");

  /* revival falls back to the nest's NEIGHBOURHOOD when the nest cell is full (the fix for
   * the respawn jam: hunters camped on the nest cell must not starve revival) */
  sim_init(&W); gossip_setup(&W, 0); W.fire_period = 0;       /* all mites parked far away */
  { uint16_t nz = W.nest_cell[0];
    uint32_t fill[4] = {0, 4, 8, 12};                          /* nest 0, sub-segments 0..3 */
    for (int k = 0; k < 4; k++) { uint32_t fm = fill[k]; W.mite_resp[fm] = 0; W.mite_tgt[fm] = nz;
      W.mite_xy[fm] = xy_pack(wc_x(nz) * SUB + SUB/2 + seg_ox(fm), wc_y(nz) * SUB + SUB/2 + seg_oy(fm)); }
    W.mite_resp[16] = 1;                                       /* nest 0, sub-segment 0 (taken) — due to revive */
    mites_build_index(&W);
    int full = (W.mite_cnt[nz] == MITE_CAP);
    mites_respawn(&W);
    check(full && W.mite_resp[16] == 0, "a due mite revives even when its nest cell's sub-segment is full");
    check(W.mite_cell[16] != nz && cell_chebyshev(W.mite_cell[16], nz) <= MITE_REVIVE_R,
          "it revives into a free slot in the nest's neighbourhood, not the full nest cell"); }

  /* respawn respects the crowding cap: fill a nest, kill an extra owner of it, and
   * confirm reviving never pushes the nest cell over MITE_CAP */
  sim_init(&W);
  int breach = 0;
  for (int i = 0; i < 3000; i++) { sim_tick(&W);
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (W.mite_cnt[c] > MITE_CAP) breach = 1; }
  check(!breach, "the crowding cap holds across 3000 ticks WITH firing (kills + nest respawns)");
}

/* ---- nest gossip: mites update the nest; revived mites adopt it ------------ */
static void t_nest_gossip(void) {
  printf("nest gossip — mites deposit intel at the nest; revived mites adopt it:\n");
  uint32_t n = nest_of(0);

  /* a homing mite deposits its (newer) gossip in the nest */
  sim_init(&W); gossip_setup(&W, 1);
  uint16_t nest = W.nest_cell[n];
  mite_reset(&W, 0, wc_x(nest), wc_y(nest));
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 500); W.mite_mode[0] = MM_HOME; W.mite_dest[0] = nest;
  W.nest_rec_cell[n] = REC_EMPTY; W.nest_rec_time[n] = 0; W.frame = 600;
  mites_build_index(&W); mites_records(&W);
  check(W.nest_rec_cell[n] == (uint16_t)wcell(20, 20) && W.nest_rec_time[n] == 500,
        "a homing mite deposits its newer gossip in the nest");

  /* older gossip does not overwrite a newer nest record */
  sim_init(&W); gossip_setup(&W, 1);
  mite_reset(&W, 0, wc_x(nest), wc_y(nest));
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 500); W.mite_mode[0] = MM_HOME; W.mite_dest[0] = nest;
  W.nest_rec_cell[n] = (uint16_t)wcell(30, 30); W.nest_rec_time[n] = 1000; W.frame = 600;
  mites_build_index(&W); mites_records(&W);
  check(W.nest_rec_cell[n] == (uint16_t)wcell(30, 30) && W.nest_rec_time[n] == 1000,
        "older gossip does not overwrite the nest's newer record");

  /* a courier carrying an EMPTY record never erases the nest's sighting (absence isn't deposited) */
  sim_init(&W); gossip_setup(&W, 1); W.nest_ttl = 0;        /* isolate from the timeout */
  mite_reset(&W, 0, wc_x(nest), wc_y(nest));
  set_rec(&W, 0, REC_EMPTY, 2000); W.mite_mode[0] = MM_HOME; W.mite_dest[0] = nest;
  W.nest_rec_cell[n] = (uint16_t)wcell(30, 30); W.nest_rec_time[n] = 1000; W.frame = 1100;
  mites_build_index(&W); mites_records(&W);
  check(W.nest_rec_cell[n] == (uint16_t)wcell(30, 30) && W.nest_rec_time[n] == 1000,
        "a courier carrying an empty record never erases the nest's sighting");

  /* the nest's memory times out: a sighting older than nest_ttl expires on its own */
  sim_init(&W); gossip_setup(&W, 1); W.nest_ttl = 600;
  W.nest_rec_cell[n] = (uint16_t)wcell(40, 40); W.nest_rec_time[n] = 100; W.frame = 800;  /* age 700 > 600 */
  mites_build_index(&W); mites_records(&W);
  check(W.nest_rec_cell[n] == REC_EMPTY, "a nest sighting older than nest_ttl expires (the nest forgets a stale tank)");
  sim_init(&W); gossip_setup(&W, 1); W.nest_ttl = 600;
  W.nest_rec_cell[n] = (uint16_t)wcell(40, 40); W.nest_rec_time[n] = 500; W.frame = 800;  /* age 300 < 600 */
  mites_build_index(&W); mites_records(&W);
  check(W.nest_rec_cell[n] == (uint16_t)wcell(40, 40), "a nest sighting within nest_ttl is kept");

  /* a revived mite adopts the nest's gossip and hunts the last-known tank cell */
  sim_init(&W); gossip_setup(&W, 1); W.fire_period = 0;
  W.nest_rec_cell[n] = (uint16_t)wcell(40, 40); W.nest_rec_time[n] = 700;
  W.mite_resp[0] = 1;                                      /* dead; revives this tick */
  sim_tick(&W);
  check(!W.mite_resp[0] && get_rec_cell(&W, 0) == (uint16_t)wcell(40, 40),
        "a revived mite adopts its nest's gossip");
  check(W.mite_mode[0] == MM_HUNT, "a revived mite hunts the nest's last-known tank position");

  /* a revived mite at a nest that knows nothing just wanders */
  sim_init(&W); gossip_setup(&W, 1); W.fire_period = 0;
  W.nest_rec_cell[n] = REC_EMPTY; W.nest_rec_time[n] = 0;
  W.mite_resp[0] = 1;
  sim_tick(&W);
  check(!W.mite_resp[0] && get_rec_cell(&W, 0) == REC_EMPTY && W.mite_mode[0] == MM_WANDER,
        "a revived mite at an empty nest just wanders");
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
  t_mite_nests_fields();
  t_tanks_fire();
  t_nest_gossip();
  t_mite_wall();
  t_mite_determinism();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
