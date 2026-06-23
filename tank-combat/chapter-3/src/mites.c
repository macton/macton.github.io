#include "mites.h"
#include "sim.h"
#include "collide.h"   /* cell_is_wall */

/* ---- the swarm's one PRNG ------------------------------------------------- */
/* xorshift32: a single integer stream for the whole swarm (wander + seek die),
 * advanced in index order so the simulation is deterministic and replayable. */
static inline uint32_t xs32(uint32_t* s) {
  uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
/* a small editable seed -> a nonzero 32-bit state (xorshift32 must not be 0) */
static inline uint32_t seed_state(uint16_t seed) {
  uint32_t s = ((uint32_t)seed * 2654435761u) ^ 0x9E3779B9u;
  return s ? s : 1u;
}

/* the cardinal from cell `from` to an adjacent cell `to` (toroidal); DIR_NONE if
 * they are not 4-neighbours (never happens for a committed step). */
static uint8_t dir_to_adjacent(uint32_t from, uint32_t to) {
  int cx = wc_x(from), cy = wc_y(from);
  for (uint8_t d = 0; d < 4; d++)
    if ((uint32_t)wc_pack(wrap_wcx(cx + CARD_DCX[d]), wrap_wcy(cy + CARD_DCY[d])) == to) return d;
  return DIR_NONE;
}

/* ---- spawn: deterministic scatter over the reachable open cells ----------- */
/* BFS/scatter scratch. File-static (single-threaded module), so the build stays
 * allocation-free — the same pattern as grid_paths.c's BFS buffers. */
static uint16_t g_reach[N_WORLD_CELLS];   /* the reachable open cells, flat */
static uint8_t  g_seen [N_WORLD_CELLS];   /* BFS visited mark */
static uint16_t g_bfsq [N_WORLD_CELLS];   /* BFS queue */
static uint8_t  g_tally[N_WORLD_CELLS];   /* per-tick cap reservation tally; <= MITE_CAP, so one byte */

/* Flood the open cells reachable from a known-open seed cell into g_reach;
 * return the count. World cell (1,7) is screen 0's ring, always open. */
static uint32_t collect_reachable(const uint32_t* grid) {
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) g_seen[i] = 0;
  uint32_t head = 0, tail = 0, n = 0, src = wc_pack(1, 7);
  g_seen[src] = 1; g_bfsq[tail++] = (uint16_t)src;
  while (head < tail) {
    uint32_t c = g_bfsq[head++];
    g_reach[n++] = (uint16_t)c;
    int cx = wc_x(c), cy = wc_y(c);
    for (int d = 0; d < 4; d++) {
      int nx = wrap_wcx(cx + CARD_DCX[d]), ny = wrap_wcy(cy + CARD_DCY[d]);
      uint32_t nc = (uint32_t)wc_pack(nx, ny);
      if (g_seen[nc] || cell_is_wall(grid, nx, ny)) continue;
      g_seen[nc] = 1; g_bfsq[tail++] = (uint16_t)nc;
    }
  }
  return n;
}

void mites_spawn(World* w) {
  w->rng = seed_state(w->mite_seed);
  uint32_t nreach = collect_reachable(w->grid);

  /* reuse mite_cnt as the running placement count; spawn respects MITE_CAP (the
   * structural max), so no spawn cell starts with more than 4. */
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) w->mite_cnt[i] = 0;
  for (uint32_t m = 0; m < N_MITES; m++) {
    /* pick a reachable cell with room, linear-probing from a random start
     * (capacity MITE_CAP*nreach >> N_MITES, so this always lands). */
    uint32_t start = nreach ? xs32(&w->rng) % nreach : 0, cell = g_reach[0];
    for (uint32_t k = 0; k < nreach; k++) {
      uint32_t c = g_reach[(start + k) % nreach];
      if (w->mite_cnt[c] < MITE_CAP) { cell = c; break; }
    }
    w->mite_cnt[cell]++;

    int wcx = wc_x(cell), wcy = wc_y(cell);
    w->mite_xy[m]  = xy_pack(wcx * SUB + SUB / 2, wcy * SUB + SUB / 2);
    w->mite_ang[m] = (uint16_t)(CARD_DI[xs32(&w->rng) & 3] << ANGLE_SHIFT);
    w->mite_in[m] = 0; w->mite_vxy[m] = 0; w->mite_hit[m] = 0;
    w->mite_mode[m] = MM_WANDER;
    w->mite_cell[m] = (uint16_t)cell; w->mite_tgt[m] = (uint16_t)cell;
    w->mite_rec_cell[0][m] = REC_EMPTY; w->mite_rec_cell[1][m] = REC_EMPTY;
    w->mite_rec_time[0][m] = 0;         w->mite_rec_time[1][m] = 0;
  }
  w->rec_buf = 0;
}

/* ---- the per-cell index --------------------------------------------------- */
void mites_build_index(World* w) {
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) w->mite_cnt[i] = 0;
  for (uint32_t m = 0; m < N_MITES; m++) {
    int wcx = wrap_wcx(xy_lo(w->mite_xy[m]) >> SUB_SHIFT);
    int wcy = wrap_wcy(xy_hi(w->mite_xy[m]) >> SUB_SHIFT);
    uint32_t c = (uint32_t)wc_pack(wcx, wcy);
    w->mite_cell[m] = (uint16_t)c;
    uint8_t k = w->mite_cnt[c];
    if (k < MITE_CAP) w->mite_list[c * MITE_CAP + k] = (uint16_t)m;
    w->mite_cnt[c] = (uint8_t)(k + 1);   /* TRUE occupancy: an over-cap cell stays detectable */
  }
}

/* ---- the world's tanks, as cells (a handful; scanned directly) ------------ */
static uint32_t tank_cell(const World* w, uint32_t t) {
  return (uint32_t)wc_pack(wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT),
                           wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT));
}

/* ---- choose a mite's next cell (cap-gated) -------------------------------- */
/* For a mite at rest in cell `mc`: pick an open, non-full adjacent cell — wander
 * = random among them; seek = the one most reducing the toroidal distance to
 * rec_c (canonical N,E,S,W tie-break). Reserve it (tally++) and set mite_tgt.
 * If none qualifies, hold (tgt == mc, no reservation). Returns the cardinal or
 * DIR_NONE. `cap` is the running tally limit (occupants + committed inbound). */
static uint8_t pick_step(World* w, uint32_t m, uint32_t mc, uint8_t mode, uint16_t rec_c, int cap) {
  int cx = wc_x(mc), cy = wc_y(mc);
  uint8_t vd[4]; uint16_t vc[4]; int nv = 0;
  for (uint8_t d = 0; d < 4; d++) {
    int nx = wrap_wcx(cx + CARD_DCX[d]), ny = wrap_wcy(cy + CARD_DCY[d]);
    if (cell_is_wall(w->grid, nx, ny)) continue;             /* walls block */
    uint32_t nc = (uint32_t)wc_pack(nx, ny);
    if (g_tally[nc] >= cap) continue;                        /* already full */
    vd[nv] = d; vc[nv] = (uint16_t)nc; nv++;
  }
  if (nv == 0) { w->mite_tgt[m] = (uint16_t)mc; return DIR_NONE; }   /* hold */

  int bi = 0;
  if (mode == MM_SEEK && rec_c != REC_EMPTY) {
    int best = 0x7FFFFFFF;
    for (int i = 0; i < nv; i++) {                           /* greedy, first (lowest d) wins ties */
      int dd = cell_manhattan(vc[i], rec_c);
      if (dd < best) { best = dd; bi = i; }
    }
  } else {
    bi = (int)(xs32(&w->rng) % (uint32_t)nv);                /* wander: random open non-full */
  }
  w->mite_tgt[m] = vc[bi];
  g_tally[vc[bi]]++;                                         /* reserve the slot */
  return vd[bi];
}

/* ---- derive the input byte from (current cell -> committed target) --------- */
/* The same grid-tracking rule tanks_path uses: turn toward the cardinal, drive
 * only when the discrete heading is exactly axis-aligned, snapping to the centre
 * at a turn — so the mite tracks cells and clears the one-cell inter-screen gaps. */
static void set_mite_input(World* w, uint32_t m) {
  uint32_t mc = w->mite_cell[m], tgt = w->mite_tgt[m];
  if (tgt == mc) { w->mite_in[m] = 0; return; }              /* hold: emit no drive */

  uint8_t d = dir_to_adjacent(mc, tgt);
  uint32_t target_di = CARD_DI[d];
  int x = xy_lo(w->mite_xy[m]), y = xy_hi(w->mite_xy[m]);
  int cc_x = (x >> SUB_SHIFT) * SUB + SUB / 2, cc_y = (y >> SUB_SHIFT) * SUB + SUB / 2;
  int ox = x - cc_x, oy = y - cc_y;
  uint32_t cur_di = (uint32_t)(w->mite_ang[m] >> ANGLE_SHIFT) & (N_DIRS - 1);

  if (target_di == cur_di) {
    w->mite_in[m] = IN_FWD;                                  /* aligned: drive straight */
  } else if (at_cell_centre(cur_di, ox, oy)) {
    w->mite_xy[m] = xy_pack(cc_x, cc_y);                     /* snap, then rotate in place */
    uint32_t delta = (target_di - cur_di) & (N_DIRS - 1);
    w->mite_in[m] = (delta <= N_DIRS / 2) ? IN_RIGHT : IN_LEFT;
  } else {
    w->mite_in[m] = IN_FWD;                                  /* roll to the centre before turning */
  }
}

/* ---- the per-tick mite transform ------------------------------------------ */
void mites_step(World* w) {
  uint32_t cur = w->rec_buf, nxt = cur ^ 1u;
  const uint16_t* rc_in = w->mite_rec_cell[cur];
  const uint32_t* rt_in = w->mite_rec_time[cur];
  uint16_t* rc_out = w->mite_rec_cell[nxt];
  uint32_t* rt_out = w->mite_rec_time[nxt];
  uint32_t frame = w->frame;
  int sense = w->mite_sense;
  int cap = w->mite_cap;

  /* PHASE A — records (read the previous-tick buffer, so order-independent). */
  for (uint32_t m = 0; m < N_MITES; m++) {
    uint32_t mc = w->mite_cell[m];
    uint16_t my_c = rc_in[m]; uint32_t my_t = rt_in[m];
    uint16_t new_c = my_c; uint32_t new_t = my_t; uint8_t mode = w->mite_mode[m];

    /* 1. Sense a tank within `sense` cells (Chebyshev): record it, stamp now, seek.
     *    A few tanks, so scan directly; nearest (Manhattan) wins, low index ties. */
    int sensed = 0, best_md = 0; uint16_t seen_c = 0;
    for (uint32_t t = 0; t < N_TANKS; t++) {
      uint32_t tc = tank_cell(w, t);
      if (cell_chebyshev(mc, tc) > sense) continue;
      int md = cell_manhattan(mc, tc);
      if (!sensed || md < best_md) { best_md = md; seen_c = (uint16_t)tc; sensed = 1; }
    }
    if (sensed) {
      new_c = seen_c; new_t = frame; mode = MM_SEEK;
    } else {
      /* 2. Read a peer: the newest record strictly newer than mine in the 3x3
       *    neighbourhood (from the index). Adopt it (cell + time, incl. empty);
       *    canonical lowest-index tie-break keeps equal timestamps deterministic. */
      int found = 0; uint32_t best_pt = 0; uint16_t best_pc = REC_EMPTY; uint32_t best_pp = 0;
      int cx = wc_x(mc), cy = wc_y(mc);
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        uint32_t nc = (uint32_t)wc_pack(wrap_wcx(cx + dx), wrap_wcy(cy + dy));
        uint8_t k = w->mite_cnt[nc]; if (k > MITE_CAP) k = MITE_CAP;
        for (uint8_t j = 0; j < k; j++) {
          uint32_t p = w->mite_list[nc * MITE_CAP + j];
          if (p == m) continue;
          uint32_t pt = rt_in[p];
          if (pt <= my_t) continue;                          /* not newer than mine */
          if (!found || pt > best_pt || (pt == best_pt && p < best_pp)) {
            found = 1; best_pt = pt; best_pc = rc_in[p]; best_pp = p;
          }
        }
      }
      if (found) {
        new_c = best_pc; new_t = best_pt;                    /* adopt the newer record */
        mode = (xs32(&w->rng) % 100u) < (uint32_t)w->mite_pseek ? MM_SEEK : MM_WANDER;
      } else if (mode == MM_SEEK && my_c != REC_EMPTY && cell_chebyshev(mc, my_c) <= 1) {
        /* 3. Arrive and check: reached within one cell of the recorded cell and
         *    no newer peer. If a tank is actually there, refresh (stamp now);
         *    otherwise erase (empty, stamp now) and wander — "it's gone" is the
         *    newest record, so it propagates back by the same read rule. */
        int tank_here = 0;
        for (uint32_t t = 0; t < N_TANKS; t++) if (tank_cell(w, t) == my_c) { tank_here = 1; break; }
        if (tank_here) { new_c = my_c; new_t = frame; }
        else { new_c = REC_EMPTY; new_t = frame; mode = MM_WANDER; }
      }
      /* 4. else keep the current record, mode, and target. */
    }
    rc_out[m] = new_c; rt_out[m] = new_t; w->mite_mode[m] = mode;
  }

  /* PHASE B — the cap tally: current occupants + already-committed inbound. Seed
   * it with the index occupancy, then re-add every in-transit mite's standing
   * reservation FIRST (a slot it already owns, conserved across the traverse), so
   * at-rest mites this tick only fill what is genuinely free. This in-order
   * reservation keeps <= cap centres per cell every tick (proven inductively:
   * next occ[C] <= occ[C] + inbound[C] = tally[C] <= cap). */
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) g_tally[i] = w->mite_cnt[i];
  for (uint32_t m = 0; m < N_MITES; m++)
    if (w->mite_tgt[m] != w->mite_cell[m]) g_tally[w->mite_tgt[m]]++;

  /* PHASE C — movement: at-rest mites choose a new cap-gated step (in index
   * order, so the n-th mite sees earlier commitments); all mites then steer. */
  for (uint32_t m = 0; m < N_MITES; m++) {
    uint32_t mc = w->mite_cell[m];
    if (w->mite_tgt[m] == mc)                                /* arrived/at rest -> re-choose */
      pick_step(w, m, mc, w->mite_mode[m], rc_out[m], cap);
    set_mite_input(w, m);
  }

  w->rec_buf = (uint8_t)nxt;                                 /* swap the record buffers */
}
