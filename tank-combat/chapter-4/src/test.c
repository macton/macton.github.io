/* test.c — native (host) tests. No wasm, no GPU. Covers the inherited chapter-1/2
 * movement + pathing AND the chapter-3 swarm: the mite pool & per-cell index, the
 * crowding cap, the last-write-wins gossip, the sense/hunt/home behaviour, the
 * nests + shared route fields, wall safety at MITE_R, and determinism. These run
 * UNCHANGED in chapter 4 — the simulation is frozen.
 *
 * CHAPTER 4 adds the render half's tests (no GPU needed — render.c is plain C over
 * the World): the chapter's THESIS, that the sim is byte-identical to chapter 3 (a
 * golden state-hash); the translucent depth key; that the camera is a host-side
 * uniform (the same view rebuilds identical bytes); and that render scales with
 * VISIBILITY, not the world or the pool.
 *
 * Build & run: ./test.sh */

#include "sim.h"
#include "collide.h"
#include "grid_paths.h"
#include "edge_paths.h"
#include "agent_move.h"
#include "mites.h"
#include "tanks_fire.h"
#include "render.h"
#include "staticmap.h"
#include "view.h"
#include <stdio.h>
#include <string.h>

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
  for (uint32_t s = 0; s < PROJ_MAX; s++) { uint32_t b = t * PROJ_MAX + s;
    w->tank_proj_live[b] = 0; w->tank_proj_xy[b] = 0; w->tank_proj_dir[b] = 0;
    w->tank_proj_dist[b] = 0; w->tank_proj_tgt[b] = TGT_NONE; }
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
    h = (h ^ w->tank_target[t]) * 16777619u;
    for (uint32_t s = 0; s < PROJ_MAX; s++) { uint32_t b = t * PROJ_MAX + s;
      h = (h ^ ((uint32_t)w->tank_proj_live[b] | ((uint32_t)w->tank_proj_tgt[b] << 16))) * 16777619u;
      h = (h ^ w->tank_proj_xy[b]) * 16777619u;
      h = (h ^ ((uint32_t)w->tank_proj_dir[b] | ((uint32_t)w->tank_proj_dist[b] << 16))) * 16777619u;
    }
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

  /* a newer EMPTY record is a readable value that propagates */
  sim_init(&W); gossip_setup(&W, 2);
  mite_reset(&W, 0, 1, 5); mite_reset(&W, 1, 1, 6);
  set_rec(&W, 0, (uint16_t)wcell(20, 20), 50);
  set_rec(&W, 1, REC_EMPTY, 100);
  mites_build_index(&W); mites_records(&W);
  check(get_rec_cell(&W, 0) == REC_EMPTY && get_rec_time(&W, 0) == 100, "a newer EMPTY record propagates (overwrites an older cell)");
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
  check(get_rec_cell(&W, 0) == REC_EMPTY && get_rec_time(&W, 0) == 600, "reaching the recorded cell with no tank erases the record (stamped now)");
  check(W.mite_mode[0] == MM_WANDER, "after erasing, the mite reverts to wander");

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

  /* nest_of partitions the swarm into NEST_COUNT balanced groups (floor or ceil of
   * N_MITES/NEST_COUNT each — N_MITES need not divide evenly by the nest count) */
  sim_init(&W);
  int cnt[NEST_COUNT]; for (int n = 0; n < NEST_COUNT; n++) cnt[n] = 0;
  int part_ok = 1;
  for (uint32_t m = 0; m < N_MITES; m++) { if (nest_of(m) != m % NEST_COUNT) part_ok = 0; cnt[nest_of(m)]++; }
  int lo = (int)(N_MITES / NEST_COUNT), balanced = 1;
  for (int n = 0; n < NEST_COUNT; n++) if (cnt[n] != lo && cnt[n] != lo + 1) balanced = 0;
  check(part_ok && balanced, "nest_of(i) = i % NEST_COUNT partitions the swarm into balanced nests");

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
/* open a single world cell (clear its wall bit) — to carve an unambiguous test pocket */
static void clear_wall(World* w, int wx, int wy) {
  wx = wrap_wcx(wx); wy = wrap_wcy(wy);
  uint32_t scr = (uint32_t)((wy / GRID_H) * SCREENS_X + wx / GRID_W);
  w->grid[scr * GRID_H + (uint32_t)(wy % GRID_H)] &= ~(1u << (uint32_t)(wx % GRID_W));
}

static void t_tanks_fire(void) {
  printf("combat — bolt aim/pierce/stop-at-wall, freed turret, death cry, nest respawn:\n");

  /* a tank fires a piercing BOLT at the mite it can hit: it launches the moment the aim is on
   * target, then travels to the mite and destroys it (the kill is NOT instant — the bolt has to
   * arrive), spawning a burst. Pin the mite under the bolt's line so it flies straight in. */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 2, 7); mite_reset(&W, 0, 5, 7);
  W.fire_period = 30; W.mite_respawn = 300; W.mite_speed = 0;
  sim_tick(&W);
  check(W.tank_target[0] == 0, "the turret acquires the mite in line of sight");
  check(W.tank_cooldown[0] > 0, "the tank goes on cooldown the moment it fires");
  check(W.tank_proj_live[0], "a bolt is launched (in flight just after firing)");
  int hit = 0; for (int i = 0; i < 6 && !hit; i++) { sim_tick(&W); if (W.mite_resp[0]) hit = 1; }
  check(hit, "the travelling bolt reaches and destroys the targeted mite");
  int anyfx = 0; for (uint32_t i = 0; i < N_FX; i++) if (W.fx_t[i]) anyfx = 1;
  check(anyfx, "a destruction burst is spawned for the destroyed mite");
  sim_tick(&W);   /* the next index rebuild drops the corpse */
  check(W.mite_cnt[wcell(5, 7)] == 0, "a dead mite leaves the per-cell index (not drawn, not targetable)");

  /* the bolt is a thin LINE that PIERCES: it destroys every mite on its line — it does NOT stop
   * on the first — while mites off the line (spread into other sub-segments) survive. Drive
   * build_index then repeated tanks_fire with hand-placed positions (no movement to perturb it)
   * and let the bolt fly the length of the row. */
  sim_init(&W); gossip_setup(&W, 3);
  place_tank(&W, 0, 1, 7);                                /* turret east, bolt along row-7 centre */
  mite_reset(&W, 0, 4, 7); mite_reset(&W, 1, 6, 7); mite_reset(&W, 2, 5, 7);
  W.mite_xy[0] = xy_pack(4 * SUB + SUB / 2, 7 * SUB + SUB / 2);        /* on the line (perp 0) */
  W.mite_xy[1] = xy_pack(6 * SUB + SUB / 2, 7 * SUB + SUB / 2);        /* on the line, BEYOND mite 0 */
  W.mite_xy[2] = xy_pack(5 * SUB + SUB / 2, 7 * SUB + SUB / 2 + 96);   /* off the line (perp 96 > PROJ_HW) */
  W.fire_period = 30; W.mite_respawn = 300;
  mites_build_index(&W);
  for (int i = 0; i < 12; i++) tanks_fire(&W);            /* fire, then fly the bolt down the row */
  check(W.mite_resp[0] > 0 && W.mite_resp[1] > 0, "the bolt pierces the first mite and destroys the next on its line");
  check(W.mite_resp[2] == 0, "a mite off the bolt's line survives — the thin bolt misses it (the spread dodges)");

  /* the bolt stops at a wall: a collinear mite beyond the wall survives */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 1, 7);
  mite_reset(&W, 0, 5, 7);  W.mite_xy[0] = xy_pack(5 * SUB + SUB / 2, 7 * SUB + SUB / 2);   /* before the block (col 8) */
  mite_reset(&W, 1, 13, 7); W.mite_xy[1] = xy_pack(13 * SUB + SUB / 2, 7 * SUB + SUB / 2);  /* beyond it (cols 8..11 walls) */
  W.fire_period = 30; W.mite_respawn = 300;
  mites_build_index(&W);
  for (int i = 0; i < 10; i++) tanks_fire(&W);
  check(W.mite_resp[0] > 0, "the bolt destroys the mite before the wall");
  check(W.mite_resp[1] == 0, "a mite beyond the wall on the same line survives (the bolt stops at the wall)");

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
  mite_reset(&W, 0, 2, 4);                                            /* mite ~due north (parks a sub-segment off centre) */
  W.fire_period = 0; W.mite_speed = 0;                                /* aim only; pin the mite */
  sim_tick(&W);
  check(W.tank_target[0] == 0, "fire off: the turret still acquires a target in sight");
  check((W.tank_turret[0] >> ANGLE_SHIFT) != CARD_DI[DIR_N], "the turret does not snap instantly — it has a turn rate");
  for (int i = 0; i < 12; i++) sim_tick(&W);                          /* let it swing onto the target */
  check(W.mite_resp[0] == 0, "fire off: no mite is killed");
  int dN = (int16_t)(W.tank_turret[0] - (uint16_t)((uint32_t)CARD_DI[DIR_N] << ANGLE_SHIFT));
  check((dN < 0 ? -dN : dN) <= 2048, "the turret swings to ~aim north at a near-north mite (fine aim resolves the sub-segment offset)");
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
  check(W.mite_resp[0] == 0 && !W.tank_proj_live[0], "does not fire on tick 1 — the turret must swing on first (turn rate gates firing)");
  int swung = 0; for (int i = 0; i < 20 && !swung; i++) { sim_tick(&W); if (W.mite_resp[0]) swung = 1; }
  check(swung, "fires once the turret has swung onto the target");

  /* the turret is FREE the instant a bolt is away — firing no longer locks it (the old beam
   * pinned the turret to the shot for its whole lifetime). Fire at a mite, then retire that
   * target and drop a new one off-axis: the barrel swings onto the newcomer WHILE the first
   * bolt is still travelling. */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 2, 7);                                            /* turret east */
  mite_reset(&W, 0, 5, 7);                                            /* due east -> fire east at once */
  W.fire_period = 30; W.mite_respawn = 300; W.mite_speed = 0;
  sim_tick(&W);
  check(W.tank_proj_live[0], "a bolt is in flight just after firing");
  uint16_t fired = W.tank_turret[0];                                  /* the aim it fired along */
  W.mite_resp[0] = 300;                                               /* retire that target (the bolt flies on) */
  mite_reset(&W, 1, 2, 3);                                            /* a new target appears to the north */
  int turned_live = 0;
  for (int i = 0; i < 8 && !turned_live; i++) { sim_tick(&W);
    if (W.tank_proj_live[0] && W.tank_turret[0] != fired) turned_live = 1; }
  check(turned_live, "the turret swings toward a new target while the bolt is still travelling (no lock)");

  /* multiple bolts aloft: a fast fire period + a slow bolt let a tank keep several shots
   * travelling at once, capped at PROJ_MAX. The bolt is so slow it piles up near the muzzle
   * without reaching the (in-sight, pinned) target inside the window, so firing never stalls
   * on the target dying — only on the slot cap. */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 1, 7);
  mite_reset(&W, 0, 7, 7);                                /* in sight east, before the col-8 wall */
  W.fire_period = 3; W.proj_speed = 16; W.mite_speed = 0; W.mite_respawn = 300;
  int maxlive = 0;
  for (int i = 0; i < 24; i++) { sim_tick(&W);
    int live = 0; for (int s = 0; s < PROJ_MAX; s++) if (W.tank_proj_live[s]) live++;
    if (live > maxlive) maxlive = live; }
  check(maxlive >= 2, "a tank keeps several bolts in flight at once (fast fire + slow bolt)");
  check(maxlive <= PROJ_MAX, "simultaneous bolts are capped at PROJ_MAX");

  /* tanks hold fire through each other: a shot that would pass through another tank is not taken.
   * Two tanks face a mite parked between them (each in the other's line), so neither fires and the
   * mite lives; clear one aside and the other shoots. (Direct tanks_fire + exact positions, so the
   * aim is axis-clean and the corridor check is exercised precisely.) */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 1, 7);                                  /* shooter, turret east */
  place_tank(&W, 1, 7, 7);                                  /* friendly beyond the mite */
  W.tank_turret[1] = (uint16_t)((uint32_t)CARD_DI[DIR_W] << ANGLE_SHIFT);   /* aimed back west, at the mite */
  mite_reset(&W, 0, 4, 7); W.mite_xy[0] = xy_pack(4 * SUB + SUB / 2, 7 * SUB + SUB / 2);  /* between, on the line */
  W.fire_period = 30; W.mite_respawn = 300;
  mites_build_index(&W);
  for (int i = 0; i < 10; i++) tanks_fire(&W);
  check(W.tank_target[0] == TGT_NONE, "the shooter won't lock a mite whose only shot is blocked by the friendly");
  check(!W.tank_proj_live[0] && !W.tank_proj_live[1 * PROJ_MAX] && W.mite_resp[0] == 0,
        "neither tank fires through the other — the mite between them survives");
  place_tank(&W, 1, 7, 2);                                  /* move the friendly out of the line */
  tanks_fire(&W);
  check(W.tank_proj_live[0], "with the friendly clear, the shooter takes the shot");

  /* re-targeting: when the most-hittable mite's shot is blocked by a friendly, the turret picks a
   * different, unblocked mite instead of locking the blocked one. */
  sim_init(&W); gossip_setup(&W, 2);
  for (int c = 2; c <= 8; c++) clear_wall(&W, c, 7);       /* open the east row */
  for (int r = 4; r <= 7; r++) clear_wall(&W, 5, r);       /* open a north column at col 5 */
  place_tank(&W, 0, 5, 7);                                  /* shooter, turret east */
  place_tank(&W, 1, 7, 7);                                  /* a friendly 2 cells east, in the line */
  mite_reset(&W, 0, 8, 7);                                  /* mite A: east, past the friendly (blocked) */
  mite_reset(&W, 1, 5, 4);                                  /* mite B: north, clear */
  W.fire_period = 30; W.mite_speed = 0;
  mites_build_index(&W); tanks_fire(&W);
  check(W.tank_target[0] == 1, "the turret skips the blocked east mite and targets the clear north one");
  place_tank(&W, 1, 7, 2);                                  /* clear the east line */
  tanks_fire(&W);
  check(W.tank_target[0] == 0, "with the line clear, the east mite (least rotation) is targeted again");

  /* run-over: a MOVING tank squashes a mite under its (cell-sized) footprint; a STILL tank
   * doesn't, and a mite a cell away (outside the footprint) survives either way. */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 5, 7);
  mite_reset(&W, 0, 5, 7);                                  /* a mite directly under the tank */
  mite_reset(&W, 1, 6, 7);                                  /* a mite one cell over (outside the footprint) */
  W.fire_period = 0;                                        /* no firing — isolate the run-over */
  mites_build_index(&W); tanks_fire(&W);                    /* tank still (vxy 0 from place_tank) */
  check(W.mite_resp[0] == 0, "a still tank does not crush the mite under it");
  W.tank_vxy[0] = xy_pack(16, 0);                           /* now the tank is moving */
  mites_build_index(&W); tanks_fire(&W);
  check(W.mite_resp[0] > 0, "a moving tank runs over the mite under it");
  check(W.mite_resp[1] == 0, "a mite a cell away is outside the tank's footprint and survives");

  /* a bolt splashing on a wall spawns an impact effect (FX_IMPACT) at the wall. */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 5, 7);                                  /* turret east; cols 8..11 are walls */
  mite_reset(&W, 0, 6, 7);                                  /* a target east, so it fires toward the wall */
  W.fire_period = 30; W.mite_respawn = 300; W.mite_speed = 0;
  int impact = 0, shake = 0;
  for (int i = 0; i < 30 && !(impact && shake); i++) { sim_tick(&W);
    for (uint32_t f = 0; f < N_FX; f++) if (W.fx_t[f] && W.fx_kind[f] == FX_IMPACT) impact = 1;
    for (uint32_t e = 0; e < WALL_SHAKE_MAX; e++) if (W.wall_shake_t[e]) shake = 1; }
  check(impact, "a bolt striking a wall spawns an impact effect");
  check(shake, "a bolt striking a wall shakes that wall segment");

  /* the death cry: a kill teaches nearby mites the firing tank's cell + hunt it. Pin the mites
   * so the bolt connects, and step until it lands. */
  sim_init(&W); gossip_setup(&W, 2);
  place_tank(&W, 0, 2, 7);
  mite_reset(&W, 0, 5, 7);                                /* the target (closest, lowest index) */
  mite_reset(&W, 1, 5, 8);                                /* a peer within 2x sensing range (=4) */
  W.mite_sense = 2; W.fire_period = 30; W.mite_respawn = 300; W.mite_speed = 0;
  int cry = 0; for (int i = 0; i < 6 && !cry; i++) { sim_tick(&W); if (W.mite_resp[0]) cry = 1; }
  check(cry, "the closest in-sight mite is the one the bolt destroys");
  check(get_rec_cell(&W, 1) == (uint16_t)wcell(2, 7), "death cry: a nearby mite learns the firing tank's cell");
  check(W.mite_mode[1] == MM_HUNT && W.mite_dest[1] == (uint16_t)wcell(2, 7),
        "death cry: the nearby mite turns to hunt the attacker");

  /* respawn: a killed mite revives at its nest after the timeout, once (cap permitting) */
  sim_init(&W); gossip_setup(&W, 1);
  place_tank(&W, 0, 2, 7); mite_reset(&W, 0, 5, 7);
  W.fire_period = 30; W.mite_respawn = 10; W.mite_speed = 0;   /* pin so the bolt connects */
  int died = 0; for (int i = 0; i < 6 && !died; i++) { sim_tick(&W); if (W.mite_resp[0]) died = 1; }
  check(died, "the mite dies when shot (short respawn set)");
  /* move the tank far away so the mite isn't instantly re-killed the moment it
   * revives at its nest (a tank camping a nest legitimately suppresses it). */
  place_tank(&W, 0, 41, 31);
  uint16_t nest0 = W.nest_cell[nest_of(0)];
  int revived = 0; for (int i = 0; i < 60 && !revived; i++) { sim_tick(&W); if (!W.mite_resp[0]) revived = 1; }
  check(revived, "a dead mite revives after the respawn timeout");
  check(W.mite_cell[0] == nest0, "the revived mite reappears at its nest cell");
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

  /* respawn respects the crowding cap: fill a nest, kill an extra owner of it, and
   * confirm reviving never pushes the nest cell over MITE_CAP */
  sim_init(&W);
  int breach = 0;
  for (int i = 0; i < 3000; i++) { sim_tick(&W);
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (W.mite_cnt[c] > MITE_CAP) breach = 1; }
  check(!breach, "the crowding cap holds across 3000 ticks WITH firing (kills + nest respawns)");
}

/* ============================================================================
 * CHAPTER 4 — the view is a projection; the simulation does not know it is drawn.
 * ==========================================================================*/

/* The chapter's CONTRACT, pinned as a literal: a fixed seed + scripted inputs run
 * for K ticks must produce the SAME full sim state as chapter 3. The sim sources are
 * kept byte-identical to chapter 3 (re-synced as chapter 3 gained the piercing-bolt
 * combat, mite flocking, multi-bolt + wall sparks, the finer turret, and the 8x8 world
 * of 4096 mites), so this hash is exactly what chapter 3's sources produce. If it ever
 * moves, presentation leaked into the model and the chapter has failed its own thesis. */
#define HASH_TICKS        1500
#define CH3_GOLDEN_HASH   0x5786043Fu

/* fold the WHOLE deterministic sim state — movement, routing, turrets, the FX ring,
 * the swarm SoA + gossip records, the shared route fields, the RNG, the frame. */
static uint32_t sim_state_hash(const World* w) {
  uint32_t h = 2166136261u, b = w->rec_buf;
#define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
  for (uint32_t t = 0; t < N_TANKS; t++) {
    MIX(w->tank_xy[t]); MIX(w->tank_ang[t]); MIX(w->tank_vxy[t]); MIX(w->tank_hit[t]);
    MIX(w->tank_turret[t]); MIX(w->tank_cooldown[t]); MIX(w->tank_target[t]);
    for (uint32_t s = 0; s < PROJ_MAX; s++) { uint32_t pb = t * PROJ_MAX + s;
      MIX(w->tank_proj_live[pb]); MIX(w->tank_proj_xy[pb]); MIX(w->tank_proj_dir[pb]);
      MIX(w->tank_proj_dist[pb]); MIX(w->tank_proj_tgt[pb]); }
    MIX(w->tstate[t]); MIX(w->phas[t]); MIX(w->pstatus[t]); MIX(w->pgoal[t]);
    MIX(w->pdest_screen[t]); MIX(w->pdest_cell[t]);
  }
  for (uint32_t i = 0; i < N_FX; i++) { MIX(w->fx_xy[i]); MIX(w->fx_t[i]); MIX(w->fx_kind[i]); }
  MIX(w->fx_head);
  for (uint32_t e = 0; e < WALL_SHAKE_MAX; e++) { MIX(w->wall_shake_cell[e]); MIX(w->wall_shake_t[e]); }
  MIX(w->wall_shake_head);
  for (uint32_t m = 0; m < N_MITES; m++) {
    MIX(w->mite_xy[m]); MIX(w->mite_ang[m]); MIX(w->mite_vxy[m]); MIX(w->mite_hit[m]);
    MIX(w->mite_mode[m]); MIX(w->mite_dest[m]); MIX(w->mite_cell[m]); MIX(w->mite_tgt[m]);
    MIX(w->mite_resp[m]); MIX(w->mite_rec_cell[b][m]); MIX(w->mite_rec_time[b][m]);
  }
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) MIX(w->mite_cnt[c]);
  for (uint32_t f = 0; f < N_FIELDS; f++) MIX(w->field_dest[f]);
  MIX(w->field_active); MIX(w->field_peak);
  MIX(w->rng); MIX(w->frame);
#undef MIX
  return h;
}

/* a fixed, reproducible script exercising auto-path + manual tanks, wall edits, a
 * re-seed, and the swarm + combat. No randomness outside the sim's own RNG. (Kept
 * byte-identical to tools/golden so the golden hash stays the chapter-3 value.) */
static void scripted_run(World* w) {
  sim_init(w);                       /* default seed 1337 */
  sim_cycle_tank(w, 0);              /* tank0 -> AUTOPATH (selected) */
  sim_set_dest(w, 0, 52, 41);        /* route it toward screen (2,2) */
  sim_cycle_tank(w, 2);              /* tank2 -> AUTOPATH; tank0 -> UNSELECTED but keeps its path */
  sim_set_dest(w, 2, 8, 50);         /* route tank2 toward screen (0,3) */
  sim_cycle_tank(w, 1);              /* select tank1 -> AUTOPATH */
  sim_cycle_tank(w, 1);              /* tank1 -> MANUAL (driven by tank_in) */
  sim_toggle_wall(w, 22, 17);        /* edit a wall (rebuild path tables) */
  for (int i = 0; i < HASH_TICKS; i++) {
    uint8_t bits = 0;                /* deterministic manual-input pattern on tank1 */
    if ((i % 8) < 5)    bits |= IN_FWD;
    if ((i % 16) >= 11) bits |= IN_BACK;
    if ((i % 6) < 2)    bits |= IN_LEFT;
    if ((i % 10) >= 7)  bits |= IN_RIGHT;
    w->tank_in[1] = bits;
    if (i == 400) sim_toggle_wall(w, 12, 9);  /* a second wall edit mid-run */
    if (i == 700) sim_set_seed(w, 4242);      /* re-scatter the swarm mid-run */
    if (i == 900) sim_set_dest(w, 0, 18, 7);  /* re-route tank0 */
    sim_tick(w);
  }
}

static void t_sim_is_chapter3(void) {
  printf("the thesis — the sim is byte-identical to chapter 3 (state hash equal):\n");
  scripted_run(&W);
  uint32_t h = sim_state_hash(&W);
  check(h == CH3_GOLDEN_HASH, "sim state-hash after K scripted ticks EQUALS chapter 3's golden hash");
  scripted_run(&W2);
  check(sim_state_hash(&W2) == h, "the scripted run is itself reproducible");
}

static void t_depth_key(void) {
  printf("translucent depth key — nearer the top-down, south-leaning camera == larger:\n");
  int south = 1, up = 1, flatx = 1;
  for (int wx = 0; wx < 5 * SUB; wx += SUB)
    for (int wy = 0; wy < 5 * SUB; wy += SUB) {
      int32_t d = view_depth_key(wx, wy, 0);
      if (view_depth_key(wx, wy + SUB, 0) <= d) south = 0;    /* south is nearer the camera */
      if (view_depth_key(wx, wy, SUB)     <= d) up = 0;       /* taller is nearer the camera */
      if (view_depth_key(wx + SUB, wy, 0) != d) flatx = 0;    /* the camera tilts only in y/z */
    }
  check(south, "stepping south (+y) strictly increases the depth key");
  check(up, "raising height (+z) strictly increases the depth key");
  check(flatx, "moving east/west (x) leaves the depth key unchanged (no x tilt)");
}

static Inst g_a[INST_MAX], g_b[INST_MAX];   /* file-static: too big for the stack */
static void t_render_deterministic(void) {
  printf("the camera is a uniform — render.c emits world placements, not screen positions:\n");
  /* the projection is now a host-side perspective matrix (a uniform), so a pan/zoom
   * never re-bakes a placement: the same visible box rebuilds the identical bytes. */
  sim_init(&W);
  DrawList da, db;
  uint32_t na = build_view(&W, g_a, &da, 0, 0, ARENA_W_SUB - 1, ARENA_H_SUB - 1, REC_EMPTY);
  uint32_t nb = build_view(&W, g_b, &db, 0, 0, ARENA_W_SUB - 1, ARENA_H_SUB - 1, REC_EMPTY);
  int same = (na == nb);
  for (uint32_t i = 0; i < na && same; i++) if (memcmp(&g_a[i], &g_b[i], sizeof(Inst))) same = 0;
  check(same, "the same view rebuilds byte-identical placements (the projection is a host uniform)");
}

static void t_render_visibility(void) {
  printf("render scales with what the camera SHOWS, not the world or the pool:\n");
  sim_init(&W);
  DrawList dw;
  uint32_t nW = build_view(&W, g_a, &dw, 0, 0, ARENA_W_SUB - 1, ARENA_H_SUB - 1, REC_EMPTY);  /* whole world */
  /* the terrain + nests LEFT the per-frame path — they are the static town now, baked once
   * (build_static_map / t_static_map). So even the whole-world DYNAMIC view is only the
   * swarm + tanks + overlays, far below the 4800 cells it used to re-emit every frame. */
  check(nW < (uint32_t)N_WORLD_CELLS, "the dynamic view no longer re-emits the terrain (well under one-per-cell)");
  check(nW <= INST_MAX, "the whole-world view fits the capacity bound");

  /* A box centre (e.g. a barrel or a bolt) may poke a hair past an edge, but never wrap:
   * the int16-cast push() bug is guarded thoroughly on the static map (it spans the east
   * columns, wx>32767); here we just confirm the dynamic placements stay in-band. */
  int coords_sane = 1;
  for (uint32_t i = 0; i < nW; i++)
    if (g_a[i].wx < -SUB || g_a[i].wx > ARENA_W_SUB + SUB ||
        g_a[i].wy < -SUB || g_a[i].wy > ARENA_H_SUB + SUB) { coords_sane = 0; break; }
  check(coords_sane, "every dynamic placement keeps its true world coordinate (no int16 wrap)");

  DrawList dz; uint32_t nZ = build_view(&W, g_b, &dz, 0, 0, GRID_W * SUB - 1, GRID_H * SUB - 1, REC_EMPTY); /* one screen */
  check(dz.opaque[K_MITE] < N_MITES, "zoomed in, only the mites in the visible box are emitted (<< the pool)");
  check(nZ <= nW, "zoomed in emits no more instances than the whole world");
}

/* the STATIC TOWN bake: a pure projection of the frozen map (build_static_map). It must
 * place exactly one instance per cell, partition cleanly into per-(screen,mesh) runs the
 * host can frustum-cull, keep the east columns' int32 coordinates, tint each nest's
 * landmark, and re-bake deterministically when the map changes. */
static Inst      g_si[STATIC_INST_MAX], g_si2[STATIC_INST_MAX];   /* file-static: too big for the stack */
static StaticRun g_sr[STATIC_RUN_MAX], g_sr2[STATIC_RUN_MAX];
static void t_static_map(void) {
  printf("the STATIC TOWN is a pure, once-baked projection of the frozen map:\n");
  sim_init(&W);
  uint32_t nr = 0, ni = build_static_map(W.grid, W.nest_cell, g_si, g_sr, &nr);
  check(ni == (uint32_t)N_WORLD_CELLS, "every cell becomes exactly one static instance (one per world cell)");
  check(nr > 0 && nr <= (uint32_t)STATIC_RUN_MAX, "the run table is non-empty and within bound");

  /* the runs partition the instance buffer: contiguous from 0, ordered, non-empty, each
   * with a valid source screen and a valid TOWN mesh id (offset past the procedural set). */
  int part = (nr == 0 || g_sr[0].first == 0); uint32_t cover = 0;
  for (uint32_t r = 0; r < nr; r++) {
    cover += g_sr[r].count;
    if (g_sr[r].count == 0) part = 0;
    if (r + 1 < nr && g_sr[r].first + g_sr[r].count != g_sr[r + 1].first) part = 0;
    if (g_sr[r].screen >= (uint16_t)N_SCREENS) part = 0;
    if (g_sr[r].mesh < (uint16_t)M_PROC_COUNT ||
        g_sr[r].mesh >= (uint16_t)(M_PROC_COUNT + MAP_MESH_COUNT)) part = 0;
  }
  check(part, "runs are contiguous, ordered, non-empty, with valid screen + town mesh ids");
  check(cover == ni, "the runs cover every static instance exactly once");

  /* each run's instances sit inside that run's source-screen world AABB, so the host can
   * cull whole screens — and every coordinate (wx up to ~40832, past int16) survives. */
  int boxed = 1, shape_ok = 1; int32_t maxwx = 0;
  for (uint32_t r = 0; r < nr; r++) {
    int32_t sx = g_sr[r].screen % SCREENS_X, sy = g_sr[r].screen / SCREENS_X;
    int32_t x0 = sx * GRID_W * SUB, x1 = x0 + GRID_W * SUB, y0 = sy * GRID_H * SUB, y1 = y0 + GRID_H * SUB;
    for (uint32_t i = g_sr[r].first; i < g_sr[r].first + g_sr[r].count; i++) {
      if (g_si[i].wx < x0 || g_si[i].wx >= x1 || g_si[i].wy < y0 || g_si[i].wy >= y1) boxed = 0;
      if (g_si[i].wx > maxwx) maxwx = g_si[i].wx;
      if (g_si[i].hx != SUB / 2 || g_si[i].hy != SUB / 2 || g_si[i].hz <= 0) shape_ok = 0;
    }
  }
  check(boxed, "every static instance sits inside its run's screen (clean frustum buckets)");
  check(shape_ok && maxwx > 32767, "static coords span the arena east-to-west (no int16 wrap), cell-sized footprints");

  /* the treatment: each nest -> exactly one landmark tinted in its hue; and the town has
   * roads, buildings, and grass (the wall/open/interior split actually fires). */
  uint32_t LM = (uint32_t)(M_PROC_COUNT + MAP_LANDMARK_BASE), GR = (uint32_t)(M_PROC_COUNT + MAP_GRASS_BASE);
  uint32_t RD0 = (uint32_t)(M_PROC_COUNT + MAP_ROAD_BASE), RD1 = RD0 + MAP_ROAD_N;
  uint32_t BD0 = (uint32_t)(M_PROC_COUNT + MAP_BUILD_BASE), BD1 = BD0 + MAP_BUILD_N;
  uint32_t nlm = 0, nroad = 0, nbuild = 0, ngrass = 0; int lm_coloured = 1;
  for (uint32_t r = 0; r < nr; r++) {
    uint32_t m = g_sr[r].mesh, c = g_sr[r].count;
    if (m == LM) { nlm += c; for (uint32_t i = g_sr[r].first; i < g_sr[r].first + c; i++)
                                if ((g_si[i].rgba & 0x00FFFFFFu) == 0) lm_coloured = 0; }
    else if (m == GR) ngrass += c;
    else if (m >= RD0 && m < RD1) nroad += c;
    else if (m >= BD0 && m < BD1) nbuild += c;
  }
  check(nlm == (uint32_t)NEST_COUNT && lm_coloured, "each nest becomes one landmark, tinted in its hue (COL_NEST)");
  check(nroad > 0 && nbuild > 0 && ngrass > 0, "the town has roads, buildings, and grass");

  /* the bake is a PURE function of (grid, nests): same map -> byte-identical town. */
  uint32_t nr2 = 0, ni2 = build_static_map(W.grid, W.nest_cell, g_si2, g_sr2, &nr2);
  int det = (ni2 == ni && nr2 == nr &&
             memcmp(g_si2, g_si, ni * sizeof(Inst)) == 0 && memcmp(g_sr2, g_sr, nr * sizeof(StaticRun)) == 0);
  check(det, "the bake is a pure function — same map rebuilds the byte-identical town");

  /* editing the map re-bakes it: still one instance per cell, but the town changed. */
  sim_toggle_wall(&W, 53, 41);   /* flip an interior cell (not a nest centre) */
  uint32_t nr3 = 0, ni3 = build_static_map(W.grid, W.nest_cell, g_si2, g_sr2, &nr3);
  check(ni3 == (uint32_t)N_WORLD_CELLS, "after a wall edit the town re-bakes, still one instance per cell");
  check(memcmp(g_si2, g_si, ni * sizeof(Inst)) != 0, "the wall edit actually changes the baked town");
}

/* the static PROPS bake (build_static_props): trees scattered on grass corners. Render-only
 * scenery — the sim never sees them — so the test pins WHERE they may sit (cell corners, on
 * open non-nest ground), that they partition into per-screen M_TREE runs, and that the bake
 * is pure, exactly the disciplines the static town is held to. */
static Inst      g_pi[STATIC_PROP_INST_MAX], g_pi2[STATIC_PROP_INST_MAX];
static StaticRun g_pr[STATIC_PROP_RUN_MAX], g_pr2[STATIC_PROP_RUN_MAX];
static int8_t    g_isnest[N_WORLD_CELLS];
static void t_static_props(void) {
  printf("the static PROPS (trees) are render-only scenery scattered on grass corners:\n");
  sim_init(&W);
  uint32_t npr = 0, npi = build_static_props(W.grid, W.nest_cell, g_pi, g_pr, &npr);
  check(npi > 0 && npi <= (uint32_t)STATIC_PROP_INST_MAX, "trees are placed, within the prop cap");
  check(npr > 0 && npr <= (uint32_t)STATIC_PROP_RUN_MAX, "the prop run table is non-empty and within bound");

  /* the runs partition the prop buffer: contiguous from 0, ordered, non-empty, all M_TREE. */
  int part = (npr == 0 || g_pr[0].first == 0); uint32_t cover = 0;
  for (uint32_t r = 0; r < npr; r++) {
    cover += g_pr[r].count;
    if (g_pr[r].count == 0) part = 0;
    if (r + 1 < npr && g_pr[r].first + g_pr[r].count != g_pr[r + 1].first) part = 0;
    if (g_pr[r].screen >= (uint16_t)N_SCREENS) part = 0;
    if (g_pr[r].mesh != (uint16_t)M_TREE) part = 0;
  }
  check(part, "prop runs are contiguous, ordered, non-empty, all M_TREE, valid screen");
  check(cover == npi, "the prop runs cover every tree exactly once");

  /* a nest-cell membership, for the placement check below */
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) g_isnest[c] = 0;
  for (uint32_t n = 0; n < NEST_COUNT; n++) g_isnest[W.nest_cell[n]] = 1;

  /* WHERE a tree may sit: at a grid-cell CORNER (wx,wy on the cell grid — not the town's
   * cell-centre placement), grounded (wz==hz>0), square-footprinted, white-tinted (it carries
   * its own brown/green); inside its run's screen; and its four surrounding cells all OPEN and
   * non-nest, so a tree never lands on a building or a landmark. */
  int corner = 1, ground = 1, open4 = 1, boxed = 1;
  for (uint32_t r = 0; r < npr; r++) {
    int32_t sx = g_pr[r].screen % SCREENS_X, sy = g_pr[r].screen / SCREENS_X;
    int32_t x0 = sx * GRID_W * SUB, x1 = x0 + GRID_W * SUB, y0 = sy * GRID_H * SUB, y1 = y0 + GRID_H * SUB;
    for (uint32_t i = g_pr[r].first; i < g_pr[r].first + g_pr[r].count; i++) {
      const Inst* t = &g_pi[i];
      if (t->wx % SUB != 0 || t->wy % SUB != 0) corner = 0;
      if (t->wz != t->hz || t->hz <= 0 || t->hx <= 0 || t->hx != t->hy || t->rgba != 0xFFFFFFFFu) ground = 0;
      if (t->wx < x0 || t->wx > x1 || t->wy < y0 || t->wy > y1) boxed = 0;   /* corner sits on/inside the screen box */
      int32_t cx = t->wx / SUB, cy = t->wy / SUB;
      const int dxs[4] = { -1, 0, -1, 0 }, dys[4] = { -1, -1, 0, 0 };
      for (int k = 0; k < 4; k++) {
        int32_t ax = cx + dxs[k], ay = cy + dys[k];
        if (cell_is_wall(W.grid, ax, ay)) open4 = 0;
        int32_t wx = ((ax % BIG_W) + BIG_W) % BIG_W, wy = ((ay % BIG_H) + BIG_H) % BIG_H;
        if (g_isnest[wc_pack(wx, wy)]) open4 = 0;
      }
    }
  }
  check(corner, "every tree sits at a grid-cell corner (on the cell grid, not the cell centre)");
  check(ground && boxed, "every tree is grounded + square-footprinted + white-tinted, inside its screen");
  check(open4, "every tree's four cells are open and non-nest (never on a building or landmark)");

  /* a pure function of the map: same map -> byte-identical trees. */
  uint32_t npr2 = 0, npi2 = build_static_props(W.grid, W.nest_cell, g_pi2, g_pr2, &npr2);
  check(npi2 == npi && npr2 == npr &&
        memcmp(g_pi2, g_pi, npi * sizeof(Inst)) == 0 && memcmp(g_pr2, g_pr, npr * sizeof(StaticRun)) == 0,
        "the prop bake is a pure function — same map rebuilds byte-identical trees");
}

static void t_render_overlays(void) {
  printf("interaction overlays are render-only (derived from the sim + one host hover cell):\n");
  sim_init(&W);
  const int32_t WX1 = ARENA_W_SUB - 1, WY1 = ARENA_H_SUB - 1;   /* the whole world in view */
  DrawList d0; build_view(&W, g_a, &d0, 0, 0, WX1, WY1, REC_EMPTY);
  check(d0.opaque[K_RING] == 0, "no tank selected => no selection mark");
  check(d0.opaque[K_DEST] == 0, "no destination => no beacon");
  check(d0.translucent == 0, "nothing translucent at frame 0 (no FX, path, or hover)");

  DrawList dh; build_view(&W, g_a, &dh, 0, 0, WX1, WY1, wc_pack(5, 5));
  check(dh.translucent == 1, "a hover cell in view emits exactly one highlight tile");
  DrawList dho; build_view(&W, g_a, &dho, 0, 0, GRID_W * SUB - 1, GRID_H * SUB - 1, wc_pack(70, 55));
  check(dho.translucent == 0, "a hover cell outside the visible box emits no tile");

  sim_cycle_tank(&W, 0);                 /* tank0 -> AUTOPATH (selected) */
  sim_set_dest(&W, 0, 12, 9);            /* a reachable cell on screen (0,0) */
  for (int i = 0; i < 3; i++) sim_tick(&W);   /* let pstatus become ROUTING so the path traces */
  DrawList dr; uint32_t n = build_view(&W, g_a, &dr, 0, 0, WX1, WY1, REC_EMPTY);
  check(dr.opaque[K_RING] == 2, "the selected tank shows its highlight (a base ring + a tall spike)");
  check(dr.opaque[K_DEST] == 1, "its destination shows a beacon");
  check(dr.translucent > 0, "the routed path is drawn (path tiles in the translucent pass)");
  check(n <= INST_MAX, "with overlays the instance count still fits the cap");
}

/* the deferred point lights: a light VOLUME per visible mite + FX, built each frame, culled
 * to the visible box — so the host draws only the lights it can see, additively. */
static void t_lights(void) {
  printf("the deferred point lights track the swarm + FX (one volume per visible mite / effect):\n");
  sim_init(&W);
  for (int i = 0; i < 60; i++) sim_tick(&W);
  uint32_t nL = build_lights(&W, g_a, 0, 0, ARENA_W_SUB - 1, ARENA_H_SUB - 1);   /* whole world in view */
  check(nL > 0 && nL <= (uint32_t)LIGHT_MAX, "lights are emitted and fit the cap");
  uint32_t alive = 0; for (uint32_t m = 0; m < N_MITES; m++) if (!W.mite_resp[m]) alive++;
  check(nL >= alive, "at least one light per alive mite (the whole swarm in view)");

  int shape = 1, lit = 1;
  for (uint32_t i = 0; i < nL; i++) {
    if (g_a[i].hx <= 0 || g_a[i].hy <= 0 || g_a[i].hz <= 0) shape = 0;            /* a positive-radius volume */
    if (g_a[i].wx < 0 || g_a[i].wx >= ARENA_W_SUB || g_a[i].wy < 0 || g_a[i].wy >= ARENA_H_SUB) shape = 0;
    if ((g_a[i].rgba >> 24) == 0) lit = 0;                                        /* intensity packed in alpha */
  }
  check(shape, "every light is a positive-radius volume at a valid world position");
  check(lit, "every light carries an intensity (packed in alpha)");

  uint32_t nZ = build_lights(&W, g_b, 0, 0, GRID_W * SUB - 1, GRID_H * SUB - 1);  /* one screen */
  check(nZ <= nL, "zoomed in emits no more lights than the whole world (culled to the box)");
  uint32_t nL2 = build_lights(&W, g_b, 0, 0, ARENA_W_SUB - 1, ARENA_H_SUB - 1);
  check(nL2 == nL && memcmp(g_b, g_a, nL * sizeof(Inst)) == 0, "build_lights is a pure function of the world");
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
  t_mite_wall();
  t_mite_determinism();
  /* chapter 4 — the render half (no GPU): the thesis, the depth key, the camera */
  t_sim_is_chapter3();
  t_depth_key();
  t_render_deterministic();
  t_render_visibility();
  t_static_map();
  t_static_props();
  t_render_overlays();
  t_lights();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
