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

/* Turn `cur` toward `want` by at most `rate`, the shortest way round, clamping so it
 * never overshoots — the turret's equivalent of the bodies' agent_turn step. */
static uint16_t turn_toward(uint16_t cur, uint16_t want, int rate) {
  int d = (int16_t)(want - cur);                 /* shortest signed delta (Q5.11 wraps) */
  if (d >  rate) d =  rate;
  if (d < -rate) d = -rate;
  return (uint16_t)((int)cur + d);
}

/* record a destruction burst at a mite's position (cosmetic ring; overwrite the oldest) */
static void spawn_fx(World* w, uint32_t xy) {
  w->fx_xy[w->fx_head] = xy;
  w->fx_t [w->fx_head] = FX_DURATION;
  w->fx_head = (uint8_t)((w->fx_head + 1u) % N_FX);
}

/* destroy mite m: a burst, mark it dead (respawn timer), freeze the corpse, and the
 * "death cry" — every live mite within 2x sensing range of it learns the firing tank's
 * cell (record stamped now + hunt), written into the current record buffer (read next
 * tick), so a kill turns the nearby swarm on its attacker. */
static void kill_mite(World* w, uint32_t m, uint16_t tcell, uint32_t frame) {
  uint16_t deadcell = w->mite_cell[m];
  spawn_fx(w, w->mite_xy[m]);
  w->mite_resp[m] = w->mite_respawn ? w->mite_respawn : 1;
  w->mite_in[m] = 0; w->mite_hit[m] = 0;
  int rad = 2 * (int)w->mite_sense, dcx = wc_x(deadcell), dcy = wc_y(deadcell);
  uint32_t buf = w->rec_buf;
  for (int oy = -rad; oy <= rad; oy++) for (int ox = -rad; ox <= rad; ox++) {
    uint32_t c = (uint32_t)wc_pack(wrap_wcx(dcx + ox), wrap_wcy(dcy + oy));
    uint8_t k = w->mite_cnt[c]; if (k > MITE_CAP) k = MITE_CAP;
    for (uint8_t j = 0; j < k; j++) {
      uint32_t p = w->mite_list[c * MITE_CAP + j];
      if (p == m || w->mite_resp[p]) continue;            /* skip self + the already-dead */
      w->mite_rec_cell[buf][p] = tcell; w->mite_rec_time[buf][p] = frame;
      w->mite_mode[p] = MM_HUNT; w->mite_dest[p] = tcell;
    }
  }
}

/* Fire the laser: a ray from the tank cell along the turret heading, marched cell by
 * cell (the LOS Bresenham, extended), destroying every live mite in each cell it crosses
 * until it meets a wall. Returns the last open cell (the beam's end, for drawing). */
static uint16_t laser_sweep(World* w, int tcx, int tcy, int co, int si, uint16_t tcell, uint32_t frame) {
  int bx = tcx + ((co * LASER_MAX) >> TRIG_SHIFT);        /* a far point along the beam */
  int by = tcy + ((si * LASER_MAX) >> TRIG_SHIFT);
  int dx = bx - tcx, dy = by - tcy;
  int adx = iabs(dx), ady = iabs(dy), sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
  int x = tcx, y = tcy, err = adx - ady;
  uint16_t endc = (uint16_t)wc_pack(wrap_wcx(tcx), wrap_wcy(tcy));
  for (int g = 0; g < LASER_MAX + 2; g++) {
    int e2 = 2 * err;
    if (e2 > -ady) { err -= ady; x += sx; }
    if (e2 <  adx) { err += adx; y += sy; }
    int wx = wrap_wcx(x), wy = wrap_wcy(y);
    if (cell_is_wall(w->grid, wx, wy)) break;              /* the beam stops at the wall */
    endc = (uint16_t)wc_pack(wx, wy);
    uint32_t c = (uint32_t)endc;
    uint8_t k = w->mite_cnt[c]; if (k > MITE_CAP) k = MITE_CAP;
    for (uint8_t j = 0; j < k; j++) {
      uint32_t p = w->mite_list[c * MITE_CAP + j];
      if (w->mite_resp[p]) continue;                       /* already destroyed (this or another beam) */
      kill_mite(w, p, tcell, frame);
    }
    if (x == bx && y == by) break;
  }
  return endc;
}

void tanks_fire(World* w) {
  uint32_t frame = w->frame;
  int period = w->fire_period;
  int rate = w->turret_rate;

  for (uint32_t i = 0; i < N_FX; i++) if (w->fx_t[i]) w->fx_t[i]--;   /* age the bursts */

  for (uint32_t t = 0; t < N_TANKS; t++) {
    int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
    int tcx = wrap_wcx(tx >> SUB_SHIFT), tcy = wrap_wcy(ty >> SUB_SHIFT);
    uint16_t turret = w->tank_turret[t];

    /* Target = the mite in line of sight MOST LIKELY TO BE HIT: the one closest to the
     * turret's CURRENT direction (least rotation to bring the barrel onto it), not the
     * spatially nearest — with a finite turn rate that is the soonest-hittable. Scan the
     * per-cell index over the search box; a whole cell is one line-of-sight test; the
     * cell's bearing sets its angular distance to the turret; lowest index breaks ties. */
    uint32_t target = TGT_NONE; int best_ang = 0x7FFFFFFF;
    for (int oy = -TARGET_MAX_R; oy <= TARGET_MAX_R; oy++)
      for (int ox = -TARGET_MAX_R; ox <= TARGET_MAX_R; ox++) {
        uint32_t c = (uint32_t)wc_pack(wrap_wcx(tcx + ox), wrap_wcy(tcy + oy));
        uint8_t k = w->mite_cnt[c]; if (k == 0) continue; if (k > MITE_CAP) k = MITE_CAP;
        if (!los(w->grid, tcx, tcy, tcx + ox, tcy + oy)) continue;  /* whole cell out of sight */
        int ad = (int16_t)(aim_angle(ox * SUB, oy * SUB) - turret); if (ad < 0) ad = -ad;
        uint32_t m = w->mite_list[c * MITE_CAP];                    /* lowest index in the cell */
        for (uint8_t j = 1; j < k; j++) { uint32_t mm = w->mite_list[c * MITE_CAP + j]; if (mm < m) m = mm; }
        if (ad < best_ang || (ad == best_ang && m < target)) { best_ang = ad; target = m; }
      }
    w->tank_target[t] = (uint16_t)target;

    /* desired aim: the exact bearing to the target, else relax to the body heading */
    uint16_t want;
    if (target != TGT_NONE) {
      int mx = xy_lo(w->mite_xy[target]), my = xy_hi(w->mite_xy[target]);
      int ddx = mx - tx; if (ddx >  ARENA_W_SUB / 2) ddx -= ARENA_W_SUB; if (ddx < -ARENA_W_SUB / 2) ddx += ARENA_W_SUB;
      int ddy = my - ty; if (ddy >  ARENA_H_SUB / 2) ddy -= ARENA_H_SUB; if (ddy < -ARENA_H_SUB / 2) ddy += ARENA_H_SUB;
      want = (ddx | ddy) ? aim_angle(ddx, ddy) : w->tank_ang[t];
    } else {
      want = w->tank_ang[t];
    }
    /* swing the turret toward the desired aim at its turn rate (not an instant snap) */
    w->tank_turret[t] = turn_toward(turret, want, rate);

    if (w->tank_cooldown[t] > 0) w->tank_cooldown[t]--;
    if (w->tank_tracer[t]   > 0) w->tank_tracer[t]--;

    /* fire only once the barrel has swung EXACTLY onto the target bearing (so the beam
     * runs precisely along the barrel — a line shot can't be a cone) and the cooldown is
     * ready (period 0 = off). The shot is a LASER: a beam along the barrel that destroys
     * every mite in its path until it meets a wall. */
    if (period > 0 && target != TGT_NONE && w->tank_turret[t] == want && w->tank_cooldown[t] == 0) {
      uint32_t bd = w->tank_turret[t] >> ANGLE_SHIFT;            /* beam = barrel heading */
      uint16_t tcell = (uint16_t)wc_pack(tcx, tcy);
      w->tank_shot_cell[t] = laser_sweep(w, tcx, tcy, dir_cos(bd), dir_sin(bd), tcell, frame);
      w->tank_cooldown[t]  = (uint16_t)period;
      w->tank_tracer[t]    = LASER_TICKS;                        /* draw the beam ~0.1 s */
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
