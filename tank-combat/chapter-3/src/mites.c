#include "mites.h"
#include "sim.h"
#include "collide.h"     /* cell_is_wall */
#include "edge_paths.h"  /* pg_fold, route_next_dir_pg, route_remaining_pg */

#define RM_INF 0xFFFFFFFFu

/* ---- the swarm's one PRNG ------------------------------------------------- */
/* xorshift32: a single integer stream for the whole swarm (wander + hunt/home
 * die), advanced in index order so the simulation is deterministic. */
static inline uint32_t xs32(uint32_t* s) {
  uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
/* a small editable seed -> a nonzero 32-bit state (xorshift32 must not be 0) */
static inline uint32_t seed_state(uint16_t seed) {
  uint32_t s = ((uint32_t)seed * 2654435761u) ^ 0x9E3779B9u;
  return s ? s : 1u;
}

/* a world cell -> its screen and its local (in-screen) cell index */
static inline uint32_t cell_screen(uint32_t c) { return ((uint32_t)wc_y(c) / GRID_H) * SCREENS_X + ((uint32_t)wc_x(c) / GRID_W); }
static inline uint32_t cell_local (uint32_t c) { return ((uint32_t)wc_y(c) % GRID_H) * GRID_W + ((uint32_t)wc_x(c) % GRID_W); }

/* the cardinal from cell `from` to an adjacent cell `to` (toroidal); DIR_NONE if
 * they are not 4-neighbours (never happens for a committed step). */
static uint8_t dir_to_adjacent(uint32_t from, uint32_t to) {
  int cx = wc_x(from), cy = wc_y(from);
  for (uint8_t d = 0; d < 4; d++)
    if ((uint32_t)wc_pack(wrap_wcx(cx + CARD_DCX[d]), wrap_wcy(cy + CARD_DCY[d])) == to) return d;
  return DIR_NONE;
}

/* find the route-field slot holding destination `dest`, or -1 (overflow / none) */
static int find_field(const World* w, uint16_t dest) {
  for (int s = 0; s < N_FIELDS; s++) if (w->field_dest[s] == dest) return s;
  return -1;
}

/* ---- file-static scratch (single-threaded; keeps the build allocation-free) - */
static uint16_t g_reach[N_WORLD_CELLS];   /* reachable open cells, for spawn */
static uint8_t  g_seen [N_WORLD_CELLS];   /* BFS visited mark (spawn) */
static uint16_t g_bfsq [N_WORLD_CELLS];   /* BFS queue (spawn) */
static uint8_t  g_tally[N_WORLD_CELLS];   /* per-tick cap reservation tally; <= MITE_CAP, so one byte */
static uint8_t  g_destmark[N_WORLD_CELLS];/* per-tick "destination seen" mark (distinct-dest count) */
static uint8_t  g_field_seen[N_FIELDS];   /* per-tick "cache slot still wanted" mark */

/* ---- nests: fold the resident route field for each home cell --------------- */
void mites_fold_nest(World* w, uint32_t n) {
  uint16_t c = w->nest_cell[n];
  w->field_dest[n] = c;
  pg_fold(w, w->field_pg + (uint32_t)n * N_EDGE_MAX, cell_screen(c), cell_local(c));
}
/* fold all four nests into the resident slots 0..NEST_COUNT-1 and clear the cache
 * (the cached tank-goal fields are stale after the routes change). */
void mites_fold_nests(World* w) {
  for (uint32_t n = 0; n < NEST_COUNT; n++) mites_fold_nest(w, n);
  for (uint32_t s = NEST_COUNT; s < N_FIELDS; s++) w->field_dest[s] = REC_EMPTY;
}

/* ---- spawn: deterministic scatter over the reachable open cells ----------- */
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

  /* g_tally as a per-cell occupied-sub-segment bitmask: a mite scatters into a cell
   * whose OWN sub-segment (seg_of) is still free, so at most one mite per (cell, seg)
   * from the very first tick — and it spawns parked at that sub-segment's centre. */
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) g_tally[i] = 0;
  for (uint32_t m = 0; m < N_MITES; m++) {
    uint8_t bit = (uint8_t)(1u << seg_of(m));
    uint32_t start = nreach ? xs32(&w->rng) % nreach : 0, cell = g_reach[0];
    for (uint32_t k = 0; k < nreach; k++) {
      uint32_t c = g_reach[(start + k) % nreach];
      if (!(g_tally[c] & bit)) { cell = c; break; }       /* this cell's sub-segment is free */
    }
    g_tally[cell] |= bit;

    int wcx = wc_x(cell), wcy = wc_y(cell);
    w->mite_xy[m]  = xy_pack(wcx * SUB + SUB / 2 + seg_ox(m), wcy * SUB + SUB / 2 + seg_oy(m));
    w->mite_ang[m] = (uint16_t)(CARD_DI[xs32(&w->rng) & 3] << ANGLE_SHIFT);
    w->mite_in[m] = 0; w->mite_vxy[m] = 0; w->mite_hit[m] = 0;
    w->mite_mode[m] = MM_WANDER; w->mite_dest[m] = REC_EMPTY; w->mite_resp[m] = 0;
    w->mite_cell[m] = (uint16_t)cell; w->mite_tgt[m] = (uint16_t)cell;
    w->mite_rec_cell[0][m] = REC_EMPTY; w->mite_rec_cell[1][m] = REC_EMPTY;
    w->mite_rec_time[0][m] = 0;         w->mite_rec_time[1][m] = 0;
  }
  w->rec_buf = 0;
}

/* ---- the per-cell index --------------------------------------------------- */
/* One slot per (cell, sub-segment): mite_list[c*MITE_CAP + seg] is the mite occupying
 * that sub-segment, or REC_EMPTY. mite_cnt[c] is the number of occupied sub-segments.
 * The cap keeps <= 1 mite per (cell, seg), so a mite always lands in its own slot. */
void mites_build_index(World* w) {
  for (uint32_t i = 0; i < N_WORLD_CELLS * MITE_CAP; i++) w->mite_list[i] = REC_EMPTY;
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) w->mite_cnt[i] = 0;
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;             /* dead (shot): not on the board */
    int wcx = wrap_wcx(xy_lo(w->mite_xy[m]) >> SUB_SHIFT);
    int wcy = wrap_wcy(xy_hi(w->mite_xy[m]) >> SUB_SHIFT);
    uint32_t c = (uint32_t)wc_pack(wcx, wcy);
    w->mite_cell[m] = (uint16_t)c;
    uint32_t slot = c * MITE_CAP + (uint32_t)seg_of(m);
    if (w->mite_list[slot] == REC_EMPTY) { w->mite_list[slot] = (uint16_t)m; w->mite_cnt[c]++; }
    /* else: a same-segment collision (the cap normally prevents it) — the later mite is
     * not slotted this tick; it re-resolves on the next step. */
  }
}

/* the world's tanks, as cells (a handful; scanned directly) */
static uint32_t tank_cell(const World* w, uint32_t t) {
  return (uint32_t)wc_pack(wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT),
                           wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT));
}

/* ---- records: the last-write-wins gossip (double-buffered) ----------------- */
/* Reads the previous-tick record buffer and writes the next, so propagation is
 * order-independent. Decides each mite's mode + destination:
 *   sense a tank  -> record it, HUNT it
 *   adopt a newer peer record -> roll P_HUNT: HUNT the cell, or carry it HOME
 *   arrive at a HUNT cell -> refresh (tank there) or erase + WANDER (gone)
 * A HOME or HUNT mite holds and relays its record; an empty record is WANDER. */
void mites_records(World* w) {
  uint32_t cur = w->rec_buf, nxt = cur ^ 1u;
  const uint16_t* rc_in = w->mite_rec_cell[cur];
  const uint32_t* rt_in = w->mite_rec_time[cur];
  uint16_t* rc_out = w->mite_rec_cell[nxt];
  uint32_t* rt_out = w->mite_rec_time[nxt];
  uint32_t frame = w->frame;
  int sense = w->mite_sense;

  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;                         /* dead: no gossip */
    uint32_t mc = w->mite_cell[m];
    uint16_t my_c = rc_in[m]; uint32_t my_t = rt_in[m];
    uint16_t new_c = my_c; uint32_t new_t = my_t; uint8_t mode = w->mite_mode[m];

    /* 1. Sense a tank within `sense` cells (Chebyshev): record it, stamp now, hunt.
     *    A few tanks, so scan directly; nearest (Manhattan) wins, low index ties. */
    int sensed = 0, best_md = 0; uint16_t seen_c = 0;
    for (uint32_t t = 0; t < N_TANKS; t++) {
      uint32_t tc = tank_cell(w, t);
      if (cell_chebyshev(mc, tc) > sense) continue;
      int md = cell_manhattan(mc, tc);
      if (!sensed || md < best_md) { best_md = md; seen_c = (uint16_t)tc; sensed = 1; }
    }
    if (sensed) {
      new_c = seen_c; new_t = frame; mode = MM_HUNT;
    } else {
      /* 2. Read a peer: the newest record strictly newer than mine in the 3x3
       *    (from the index). Adopt it (cell + time, incl. empty), then roll the
       *    role die — even while already hunting/homing, a newer record re-rolls.
       *    Canonical lowest-index tie-break keeps equal timestamps deterministic. */
      int found = 0; uint32_t best_pt = 0; uint16_t best_pc = REC_EMPTY; uint32_t best_pp = 0;
      int cx = wc_x(mc), cy = wc_y(mc);
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        uint32_t nc = (uint32_t)wc_pack(wrap_wcx(cx + dx), wrap_wcy(cy + dy));
        for (uint8_t s = 0; s < MITE_CAP; s++) {
          uint32_t p = w->mite_list[nc * MITE_CAP + s];
          if (p == REC_EMPTY || p == m) continue;
          uint32_t pt = rt_in[p];
          if (pt <= my_t) continue;
          if (!found || pt > best_pt || (pt == best_pt && p < best_pp)) {
            found = 1; best_pt = pt; best_pc = rc_in[p]; best_pp = p;
          }
        }
      }
      if (found) {
        new_c = best_pc; new_t = best_pt;                    /* adopt + relay the newer record */
        if (new_c == REC_EMPTY) mode = MM_WANDER;            /* nowhere to hunt -> wander */
        else mode = (xs32(&w->rng) % 100u) < (uint32_t)w->mite_phunt ? MM_HUNT : MM_HOME;
      } else if (mode == MM_HUNT && my_c != REC_EMPTY && cell_chebyshev(mc, my_c) <= 1) {
        /* 3. Arrive (hunt): reached the recorded cell. Tank there -> refresh (stamp now,
         *    keep hunting); gone -> erase (empty, stamp now), then with P=MITE_PREPORT
         *    carry that "gone" update HOME to the nest, else just wander. The
         *    empty-stamped-now record is newest, so "gone" propagates back either way. */
        int tank_here = 0;
        for (uint32_t t = 0; t < N_TANKS; t++) if (tank_cell(w, t) == my_c) { tank_here = 1; break; }
        if (tank_here) { new_c = my_c; new_t = frame; }
        else { new_c = REC_EMPTY; new_t = frame;
               mode = (xs32(&w->rng) % 100u) < (uint32_t)MITE_PREPORT ? MM_HOME : MM_WANDER; }
      } else if (mode == MM_HOME && mc == w->nest_cell[nest_of(m)]) {
        /* 3b. Arrive (home): deposit the carried gossip in the nest IF it is newer than
         *     what the nest holds (so the nest tracks the latest tank intel — a sighting
         *     or a "gone" erase) — then drop it and rejoin the wanderers. */
        uint32_t n = nest_of(m);
        if (my_t > w->nest_rec_time[n]) { w->nest_rec_cell[n] = my_c; w->nest_rec_time[n] = my_t; }
        new_c = REC_EMPTY; new_t = frame; mode = MM_WANDER;
      }
      /* 4. else keep the current record, mode, and destination (still wandering, or
       *    still in transit to a hunt/home cell it has not yet reached). */
    }

    /* resolve the destination from the final mode */
    uint16_t dest;
    if (mode == MM_HUNT)      dest = new_c;                       /* the recorded tank cell */
    else if (mode == MM_HOME) dest = w->nest_cell[nest_of(m)];   /* this mite's nest */
    else                      dest = REC_EMPTY;                   /* wander */

    rc_out[m] = new_c; rt_out[m] = new_t; w->mite_mode[m] = mode; w->mite_dest[m] = dest;
  }
  w->rec_buf = (uint8_t)nxt;                                      /* swap the record buffers */
}

/* ---- route fields: a handful of shared routes for a thousand movers -------- */
/* Collect the distinct active destinations (the NEST_COUNT resident nests + the
 * hunting mites' recorded cells), fold a pg field for each new tank-goal into a
 * free cache slot, reuse resident/cached ones, free dropped ones. Track the
 * distinct count and its peak (the peak is how N_FIELDS is sized). */
void mites_update_fields(World* w) {
  for (uint32_t s = NEST_COUNT; s < N_FIELDS; s++) g_field_seen[s] = 0;
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) g_destmark[i] = 0;

  uint32_t distinct = 0;
  for (uint32_t n = 0; n < NEST_COUNT; n++) {                     /* nests are always active */
    uint16_t c = w->nest_cell[n];
    if (!g_destmark[c]) { g_destmark[c] = 1; distinct++; }
  }
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m] || w->mite_mode[m] != MM_HUNT) continue;  /* dead, or not hunting */
    uint16_t d = w->mite_dest[m];
    if (d == REC_EMPTY || g_destmark[d]) continue;               /* a nest or an already-seen goal */
    g_destmark[d] = 1; distinct++;
    int s = find_field(w, d);
    if (s >= 0) { if (s >= (int)NEST_COUNT) g_field_seen[s] = 1; continue; }  /* already resident/cached */
    int free = -1;
    for (uint32_t fs = NEST_COUNT; fs < N_FIELDS; fs++) if (w->field_dest[fs] == REC_EMPTY) { free = (int)fs; break; }
    if (free >= 0) {                                             /* fold a new cache field */
      pg_fold(w, w->field_pg + (uint32_t)free * N_EDGE_MAX, cell_screen(d), cell_local(d));
      w->field_dest[free] = d; g_field_seen[free] = 1;
    }
    /* else: table full -> this goal overflows; its mites steer greedily (mites_step) */
  }
  for (uint32_t s = NEST_COUNT; s < N_FIELDS; s++) if (!g_field_seen[s]) w->field_dest[s] = REC_EMPTY;

  w->field_active = distinct;
  if (distinct > w->field_peak) w->field_peak = distinct;
}

/* ---- choose a mite's next cell (cap-gated) -------------------------------- */
/* Among the open, non-full neighbours: wander picks one at random; hunt/home
 * picks the one with the least field-distance to the destination (the route), or
 * — if the destination's field overflowed the table — the least toroidal distance
 * (greedy fallback). If none qualify, hold. Reserves the chosen cell (tally++). */
static uint8_t pick_step(World* w, uint32_t m, uint32_t mc, int cap) {
  int cx = wc_x(mc), cy = wc_y(mc);
  uint8_t bit = (uint8_t)(1u << seg_of(m));
  uint8_t vd[4]; uint16_t vc[4]; int nv = 0;
  for (uint8_t d = 0; d < 4; d++) {
    int nx = wrap_wcx(cx + CARD_DCX[d]), ny = wrap_wcy(cy + CARD_DCY[d]);
    if (cell_is_wall(w->grid, nx, ny)) continue;
    uint32_t nc = (uint32_t)wc_pack(nx, ny);
    if (g_tally[nc] & bit) continue;                   /* my sub-segment is taken there */
    if (popcount4(g_tally[nc]) >= cap) continue;       /* the cell already holds `cap` segments */
    vd[nv] = d; vc[nv] = (uint16_t)nc; nv++;
  }
  if (nv == 0) { w->mite_tgt[m] = (uint16_t)mc; return DIR_NONE; }   /* hold */

  uint8_t mode = w->mite_mode[m];
  uint16_t dest = w->mite_dest[m];
  int bi = 0;
  if (mode == MM_WANDER || dest == REC_EMPTY || dest == mc) {
    bi = (int)(xs32(&w->rng) % (uint32_t)nv);                        /* wander / local idle at the dest */
  } else {
    int slot = find_field(w, dest);
    if (slot >= 0) {                                                 /* navigate the shared route field */
      uint32_t ds = cell_screen(dest), dc = cell_local(dest);
      const uint16_t* pg = w->field_pg + (uint32_t)slot * N_EDGE_MAX;
      uint32_t best = RM_INF; bi = -1;
      for (int i = 0; i < nv; i++) {
        uint32_t rem = route_remaining_pg(w, pg, ds, dc, cell_screen(vc[i]), cell_local(vc[i]));
        if (rem < best) { best = rem; bi = i; }                      /* first (lowest d) wins ties */
      }
      if (bi < 0) bi = (int)(xs32(&w->rng) % (uint32_t)nv);          /* no route via any open neighbour */
    } else {                                                         /* overflow: greedy toroidal fallback */
      int best = 0x7FFFFFFF;
      for (int i = 0; i < nv; i++) {
        int dd = cell_manhattan(vc[i], dest);
        if (dd < best) { best = dd; bi = i; }
      }
    }
  }
  w->mite_tgt[m] = vc[bi];
  g_tally[vc[bi]] |= bit;
  return vd[bi];
}

/* ---- derive the input byte from (current cell -> committed target) --------- */
/* The same grid-tracking rule tanks_path uses: turn toward the cardinal, drive
 * only when the discrete heading is exactly axis-aligned, snapping at a turn. */
static void set_mite_input(World* w, uint32_t m) {
  uint32_t mc = w->mite_cell[m], tgt = w->mite_tgt[m];
  int x = xy_lo(w->mite_xy[m]), y = xy_hi(w->mite_xy[m]);
  /* this mite's anchor = its sub-segment's centre in its current cell (a quarter-cell
   * off the cell centre). It travels between anchors (a grid shifted by the segment
   * offset), so the whole swarm rides on four interleaved grids and spreads out. */
  int ax = (x >> SUB_SHIFT) * SUB + SUB / 2 + seg_ox(m);
  int ay = (y >> SUB_SHIFT) * SUB + SUB / 2 + seg_oy(m);
  if (tgt == mc) { w->mite_xy[m] = xy_pack(ax, ay); w->mite_in[m] = 0; return; }  /* at rest: settle in-segment */

  uint8_t d = dir_to_adjacent(mc, tgt);
  uint32_t target_di = CARD_DI[d];
  int ox = x - ax, oy = y - ay;
  uint32_t cur_di = (uint32_t)(w->mite_ang[m] >> ANGLE_SHIFT) & (N_DIRS - 1);

  if (target_di == cur_di) {
    w->mite_in[m] = IN_FWD;
  } else if (at_cell_centre(cur_di, ox, oy)) {
    w->mite_xy[m] = xy_pack(ax, ay);
    uint32_t delta = (target_di - cur_di) & (N_DIRS - 1);
    w->mite_in[m] = (delta <= N_DIRS / 2) ? IN_RIGHT : IN_LEFT;
  } else {
    w->mite_in[m] = IN_FWD;
  }
}

/* ---- the per-tick movement: crowding cap + steer -------------------------- */
void mites_step(World* w) {
  int cap = w->mite_cap;

  /* the cap tally is now a per-cell BITMASK of reserved sub-segments (bit s = segment s
   * taken). Seed it with each live mite's segment in its current cell, then add every
   * in-transit mite's standing reservation — its segment in its TARGET cell (a slot it
   * already owns, conserved across the traverse) — so at-rest mites this tick only fill
   * genuinely-free segments. In order, this keeps <= 1 mite per (cell, segment) and
   * <= cap occupied segments per cell after the step. */
  for (uint32_t i = 0; i < N_WORLD_CELLS; i++) g_tally[i] = 0;
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;
    uint8_t bit = (uint8_t)(1u << seg_of(m));
    g_tally[w->mite_cell[m]] |= bit;
    if (w->mite_tgt[m] != w->mite_cell[m]) g_tally[w->mite_tgt[m]] |= bit;
  }

  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m]) continue;                          /* dead: frozen */
    uint32_t mc = w->mite_cell[m];
    if (w->mite_tgt[m] == mc) pick_step(w, m, mc, cap);     /* arrived/at rest -> re-choose */
    set_mite_input(w, m);
  }
}
