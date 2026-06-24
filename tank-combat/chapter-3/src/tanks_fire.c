#include "tanks_fire.h"
#include "sim.h"
#include "collide.h"   /* cell_is_wall */
#include "dirtab.h"    /* dir_cos / dir_sin */

static inline int iabs(int v) { return v < 0 ? -v : v; }

/* Line of sight between two cells over the toroidal grid: an integer Bresenham
 * walk along the toroidal-shortest line, blocked if any intermediate cell is a
 * wall (the endpoints — the tank cell and the mite cell — are excluded). Walks in
 * extended coordinates; cell_is_wall wraps. */
static int los(const uint32_t* g, int ax, int ay, int bx, int by) {
  ax = wrap_wcx(ax); ay = wrap_wcy(ay); bx = wrap_wcx(bx); by = wrap_wcy(by);
  int dx = bx - ax; if (dx >  BIG_W / 2) dx -= BIG_W; if (dx < -BIG_W / 2) dx += BIG_W;
  int dy = by - ay; if (dy >  BIG_H / 2) dy -= BIG_H; if (dy < -BIG_H / 2) dy += BIG_H;
  int adx = iabs(dx), ady = iabs(dy), sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
  int x = ax, y = ay, x1 = ax + dx, y1 = ay + dy, err = adx - ady;
  for (;;) {
    int e2 = 2 * err;
    if (e2 > -ady) { err -= ady; x += sx; }
    if (e2 <  adx) { err += adx; y += sy; }
    if (x == x1 && y == y1) return 1;          /* reached the target: all clear */
    if (cell_is_wall(g, x, y)) return 0;       /* an intermediate wall blocks it */
  }
}

/* The nearest of the 32 discrete headings pointing along the subcell vector
 * (dx,dy): the one whose unit vector has the largest dot product with it. */
static uint16_t aim_angle(int dx, int dy) {
  int best = -2147483647; uint32_t bd = 0;
  for (uint32_t d = 0; d < N_DIRS; d++) {
    int dot = dx * dir_cos(d) + dy * dir_sin(d);
    if (dot > best) { best = dot; bd = d; }
  }
  return (uint16_t)(bd << ANGLE_SHIFT);
}

/* Scan one cell (cx,cy) for the lowest-index mite the tank at (tcx,tcy) can see;
 * a whole cell is one line-of-sight test. Updates *best (lower index wins). */
static void scan_cell(const World* w, int tcx, int tcy, int cx, int cy, uint32_t* best) {
  uint32_t c = (uint32_t)wc_pack(wrap_wcx(cx), wrap_wcy(cy));
  uint8_t k = w->mite_cnt[c]; if (k > MITE_CAP) k = MITE_CAP;
  if (k == 0) return;
  if (!los(w->grid, tcx, tcy, cx, cy)) return;             /* whole cell out of sight */
  for (uint8_t j = 0; j < k; j++) {
    uint32_t m = w->mite_list[c * MITE_CAP + j];
    if (m < *best) *best = m;                              /* dead mites aren't indexed */
  }
}

void tanks_fire(World* w) {
  uint32_t frame = w->frame;
  int period = w->fire_period;

  for (uint32_t t = 0; t < N_TANKS; t++) {
    int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
    int tcx = wrap_wcx(tx >> SUB_SHIFT), tcy = wrap_wcy(ty >> SUB_SHIFT);

    /* nearest mite in line of sight, by rough rings outward (stop at the first
     * ring that holds a visible mite — that ring's lowest index is the target). */
    uint32_t target = TGT_NONE;
    for (int r = 0; r <= TARGET_MAX_R && target == TGT_NONE; r++) {
      uint32_t best = TGT_NONE;
      if (r == 0) {
        scan_cell(w, tcx, tcy, tcx, tcy, &best);
      } else {
        for (int ox = -r; ox <= r; ox++) { scan_cell(w, tcx, tcy, tcx + ox, tcy - r, &best);
                                           scan_cell(w, tcx, tcy, tcx + ox, tcy + r, &best); }
        for (int oy = -r + 1; oy <= r - 1; oy++) { scan_cell(w, tcx, tcy, tcx - r, tcy + oy, &best);
                                                   scan_cell(w, tcx, tcy, tcx + r, tcy + oy, &best); }
      }
      if (best != TGT_NONE) target = best;
    }
    w->tank_target[t] = (uint16_t)target;

    /* aim: at the target if any, else rest aligned with the body heading */
    if (target != TGT_NONE) {
      int mx = xy_lo(w->mite_xy[target]), my = xy_hi(w->mite_xy[target]);
      int ddx = mx - tx; if (ddx >  ARENA_W_SUB / 2) ddx -= ARENA_W_SUB; if (ddx < -ARENA_W_SUB / 2) ddx += ARENA_W_SUB;
      int ddy = my - ty; if (ddy >  ARENA_H_SUB / 2) ddy -= ARENA_H_SUB; if (ddy < -ARENA_H_SUB / 2) ddy += ARENA_H_SUB;
      w->tank_turret[t] = (ddx | ddy) ? aim_angle(ddx, ddy) : w->tank_ang[t];
    } else {
      w->tank_turret[t] = w->tank_ang[t];
    }

    if (w->tank_cooldown[t] > 0) w->tank_cooldown[t]--;
    if (w->tank_tracer[t]   > 0) w->tank_tracer[t]--;

    /* fire: one shot kills the target, on the fixed cooldown (period 0 = off) */
    if (period > 0 && target != TGT_NONE && w->tank_cooldown[t] == 0) {
      uint32_t m = target;
      uint16_t deadcell = w->mite_cell[m];
      w->mite_resp[m] = w->mite_respawn ? w->mite_respawn : 1;   /* dead until it revives */
      w->mite_in[m] = 0; w->mite_hit[m] = 0;                      /* freeze the corpse this tick */
      w->tank_cooldown[t] = (uint16_t)period;
      w->tank_shot_cell[t] = deadcell; w->tank_tracer[t] = 4;     /* a brief tracer */

      /* death cry: every mite within 2x sensing range of the dead one learns the
       * firing tank's cell — record stamped now + hunt — so the swarm turns on the
       * attacker. Written into the current record buffer (read next tick). */
      uint16_t tcell = (uint16_t)wc_pack(tcx, tcy);
      int rad = 2 * (int)w->mite_sense, dcx = wc_x(deadcell), dcy = wc_y(deadcell);
      uint32_t buf = w->rec_buf;
      for (int oy = -rad; oy <= rad; oy++) for (int ox = -rad; ox <= rad; ox++) {
        uint32_t c = (uint32_t)wc_pack(wrap_wcx(dcx + ox), wrap_wcy(dcy + oy));
        uint8_t k = w->mite_cnt[c]; if (k > MITE_CAP) k = MITE_CAP;
        for (uint8_t j = 0; j < k; j++) {
          uint32_t p = w->mite_list[c * MITE_CAP + j];
          if (p == m) continue;
          w->mite_rec_cell[buf][p] = tcell; w->mite_rec_time[buf][p] = frame;
          w->mite_mode[p] = MM_HUNT; w->mite_dest[p] = tcell;
        }
      }
    }
  }
}

/* Runs right after mites_build_index (sim_tick step 1b): the index holds only the
 * live mites, so mite_cnt[nest] is this tick's true occupancy. A revive that finds a
 * free slot is added straight into the index (count + list), so the crowding cap is
 * enforced at the moment of revival and the rest of the tick — gossip, the cap
 * reservation in mites_step, and targeting — treats the mite as an ordinary live one.
 *
 * "Free slot" must match what mites_step will count: current centres (mite_cnt) PLUS
 * mites already in transit toward the nest (they reserved a slot last tick). Counting
 * only mite_cnt would let a revive and an arriving mite both claim the last slot, so
 * we pre-tally the inbound reservations per nest — exactly mites_step's tally seed. */
void mites_respawn(World* w) {
  int cap = w->mite_cap;
  int reserved[NEST_COUNT]; for (int n = 0; n < NEST_COUNT; n++) reserved[n] = 0;
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m] || w->mite_tgt[m] == w->mite_cell[m]) continue;  /* dead or at rest */
    for (int n = 0; n < NEST_COUNT; n++) if (w->mite_tgt[m] == w->nest_cell[n]) reserved[n]++;
  }
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m] == 0) continue;                  /* alive */
    if (--w->mite_resp[m] > 0) continue;                 /* still dead */
    uint32_t n = nest_of(m); uint16_t nest = w->nest_cell[n];
    /* revive only if current centres + inbound reservations leave room, so the cap
     * holds even as those inbound mites arrive; else wait one more tick */
    if ((int)w->mite_cnt[nest] + reserved[n] < cap) {
      int wcx = wc_x(nest), wcy = wc_y(nest);
      w->mite_xy[m] = xy_pack(wcx * SUB + SUB / 2, wcy * SUB + SUB / 2);
      w->mite_ang[m] = 0; w->mite_in[m] = 0; w->mite_vxy[m] = 0; w->mite_hit[m] = 0;
      w->mite_mode[m] = MM_WANDER; w->mite_dest[m] = REC_EMPTY;
      w->mite_cell[m] = nest; w->mite_tgt[m] = nest;
      w->mite_rec_cell[0][m] = REC_EMPTY; w->mite_rec_cell[1][m] = REC_EMPTY;
      w->mite_rec_time[0][m] = 0;         w->mite_rec_time[1][m] = 0;
      w->mite_resp[m] = 0;
      /* add it to the freshly-built per-cell index (there is a free slot: cnt < cap) */
      w->mite_list[(uint32_t)nest * MITE_CAP + w->mite_cnt[nest]] = (uint16_t)m;
      w->mite_cnt[nest]++;
    } else {
      w->mite_resp[m] = 1;                                /* nest full: retry next tick */
    }
  }
}
