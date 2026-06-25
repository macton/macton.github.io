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
    for (uint8_t s = 0; s < MITE_CAP; s++) {
      uint32_t p = w->mite_list[c * MITE_CAP + s];
      if (p == REC_EMPTY || p == m || w->mite_resp[p]) continue;   /* empty / self / already-dead */
      w->mite_rec_cell[buf][p] = tcell; w->mite_rec_time[buf][p] = frame;
      w->mite_mode[p] = MM_HUNT; w->mite_dest[p] = tcell;
    }
  }
}

/* Advance tank t's live bolt one tick: march it up to PROJ_SPEED subcells along its fixed
 * heading and, in every cell of that swept segment, destroy the live mites whose ACTUAL
 * position lies within PROJ_HW of the bolt's line (perpendicular distance) AND within this
 * tick's forward window (along in [0, step]). It PIERCES — a kill never stops it — so a
 * single bolt mows a whole line. It stops, and the bolt expires (live = 0), at the first
 * wall in the segment or once it has flown PROJ_RANGE cells in total. Because mites sit in
 * sub-segments a quarter-cell off centre, a thin bolt misses the ones off the line — they
 * dodge. The kill window tiles contiguously tick to tick (this tick's [0,step] from the new
 * head abuts last tick's), so every point on the path is swept exactly once. */
static void proj_step(World* w, uint32_t t, uint32_t frame) {
  int px = xy_lo(w->tank_proj_xy[t]), py = xy_hi(w->tank_proj_xy[t]);
  uint32_t pd = w->tank_proj_dir[t] >> ANGLE_SHIFT;
  int co = dir_cos(pd), si = dir_sin(pd);
  uint16_t pt = w->tank_proj_tgt[t];                      /* the aimed mite — killed on arrival regardless of perp */

  int step = PROJ_SPEED;                                   /* this tick's reach (subcells) */
  int rng_left = (int)PROJ_RANGE * SUB - (int)w->tank_proj_dist[t];
  if (step > rng_left) step = rng_left;                    /* the last partial step before it expires */

  /* the firing tank's CURRENT cell — what a kill teaches the nearby swarm (the death cry) */
  int tcx = wrap_wcx(xy_lo(w->tank_xy[t]) >> SUB_SHIFT), tcy = wrap_wcy(xy_hi(w->tank_xy[t]) >> SUB_SHIFT);
  uint16_t tcell = (uint16_t)wc_pack(tcx, tcy);

  int ex = px + ((co * step) >> TRIG_SHIFT), ey = py + ((si * step) >> TRIG_SHIFT);   /* segment end */
  int scx = px >> SUB_SHIFT, scy = py >> SUB_SHIFT;        /* start cell (extended coords) */
  int ecx = ex >> SUB_SHIFT, ecy = ey >> SUB_SHIFT;        /* end cell */
  int dcx = ecx - scx, dcy = ecy - scy;
  int adx = iabs(dcx), ady = iabs(dcy), sx = dcx < 0 ? -1 : 1, sy = dcy < 0 ? -1 : 1;
  int cx = scx, cy = scy, err = adx - ady, hitwall = 0;

  for (int g = 0; g <= adx + ady; g++) {                   /* every cell on the segment, start..end */
    int wx = wrap_wcx(cx), wy = wrap_wcy(cy);
    if (cell_is_wall(w->grid, wx, wy)) { hitwall = 1; break; }   /* the bolt stops at the wall */
    uint32_t c = (uint32_t)wc_pack(wx, wy);
    if (w->mite_cnt[c]) {
      for (uint8_t s = 0; s < MITE_CAP; s++) {
        uint32_t p = w->mite_list[c * MITE_CAP + s];
        if (p == REC_EMPTY || w->mite_resp[p]) continue;   /* empty / already destroyed */
        int vx = xy_lo(w->mite_xy[p]) - px; if (vx >  ARENA_W_SUB / 2) vx -= ARENA_W_SUB; if (vx < -ARENA_W_SUB / 2) vx += ARENA_W_SUB;
        int vy = xy_hi(w->mite_xy[p]) - py; if (vy >  ARENA_H_SUB / 2) vy -= ARENA_H_SUB; if (vy < -ARENA_H_SUB / 2) vy += ARENA_H_SUB;
        int along = (vx * co + vy * si) >> TRIG_SHIFT;      /* distance along the bolt's heading */
        int perp  = (vx * si - vy * co) >> TRIG_SHIFT; if (perp < 0) perp = -perp;
        /* mow everything within the thin half-width; the aimed mite (in a cell on the bolt's path)
         * dies whatever its sub-segment offset, so a locked turret reliably kills what it shot at. */
        if (along >= 0 && along <= step && (perp <= PROJ_HW || p == pt)) kill_mite(w, p, tcell, frame);
      }
    }
    if (cx == ecx && cy == ecy) break;
    int e2 = 2 * err;
    if (e2 > -ady) { err -= ady; cx += sx; }
    if (e2 <  adx) { err += adx; cy += sy; }
  }

  if (hitwall) { w->tank_proj_live[t] = 0; return; }       /* spent against the wall */
  w->tank_proj_xy[t]   = xy_pack(wrap_x(ex), wrap_y(ey));  /* advance the head (torus-wrapped) */
  w->tank_proj_dist[t] = (uint16_t)(w->tank_proj_dist[t] + step);
  if (w->tank_proj_dist[t] >= PROJ_RANGE * SUB) w->tank_proj_live[t] = 0;  /* flew its full range */
}

void tanks_fire(World* w) {
  uint32_t frame = w->frame;
  int period = w->fire_period;
  int rate = w->turret_rate;

  for (uint32_t i = 0; i < N_FX; i++) if (w->fx_t[i]) w->fx_t[i]--;   /* age the bursts */

  for (uint32_t t = 0; t < N_TANKS; t++) {
    /* a bolt already in flight pierces on this tick (and may expire at a wall / its range),
     * independent of where the turret now points — firing does not pin the bolt to the aim. */
    if (w->tank_proj_live[t]) proj_step(w, t, frame);

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
        if (w->mite_cnt[c] == 0) continue;                         /* no mites in this cell */
        if (!los(w->grid, tcx, tcy, tcx + ox, tcy + oy)) continue;  /* whole cell out of sight */
        int ad = (int16_t)(aim_angle(ox * SUB, oy * SUB) - turret); if (ad < 0) ad = -ad;
        uint32_t m = TGT_NONE;                                      /* lowest index over the segment slots */
        for (uint8_t s = 0; s < MITE_CAP; s++) { uint32_t mm = w->mite_list[c * MITE_CAP + s]; if (mm < m) m = mm; }
        if (m != TGT_NONE && (ad < best_ang || (ad == best_ang && m < target))) { best_ang = ad; target = m; }
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
    /* swing the turret toward the desired aim at its turn rate — ALWAYS, even with a bolt in
     * flight: firing no longer locks the turret, so the moment a shot is away the barrel is
     * free to line up the next target while the bolt is still travelling. */
    w->tank_turret[t] = turn_toward(turret, want, rate);

    if (w->tank_cooldown[t] > 0) w->tank_cooldown[t]--;

    /* fire only once the barrel has swung EXACTLY onto the target bearing (so the bolt leaves
     * straight along the barrel), the cooldown is ready (period 0 = off), and our previous bolt
     * has expired (at most one in flight at a time). The shot is a PROJECTILE: a piercing bolt
     * launched from the muzzle that mows the line ahead until it hits a wall (proj_step flies it). */
    if (period > 0 && target != TGT_NONE && w->tank_turret[t] == want
        && w->tank_cooldown[t] == 0 && !w->tank_proj_live[t]) {
      w->tank_proj_xy[t]   = w->tank_xy[t];          /* spawn at the tank, heading along the barrel */
      w->tank_proj_dir[t]  = w->tank_turret[t];
      w->tank_proj_dist[t] = 0;
      w->tank_proj_tgt[t]  = (uint16_t)target;        /* the bolt carries its target for a sure kill */
      w->tank_proj_live[t] = 1;
      w->tank_cooldown[t]  = (uint16_t)period;
    }
  }
}

/* Runs right after mites_build_index (sim_tick step 1b): the index holds the live mites,
 * one slot per (nest cell, sub-segment). A mite revives only if its OWN sub-segment at its
 * nest is free — counting current occupants AND mites already in transit toward the nest
 * (a slot they reserved last tick) — and the cell holds fewer than `mite_cap` segments. It
 * is then slotted straight into the index, so the cap holds and the rest of the tick treats
 * it as an ordinary live mite. Otherwise it waits one more tick. */
void mites_respawn(World* w) {
  int cap = w->mite_cap;
  /* per-nest occupied-sub-segment bitmask: current occupants (from the index) + inbound */
  uint8_t occ[NEST_COUNT];
  for (int n = 0; n < NEST_COUNT; n++) {
    uint32_t nc = (uint32_t)w->nest_cell[n]; uint8_t b = 0;
    for (int s = 0; s < MITE_CAP; s++) if (w->mite_list[nc * MITE_CAP + s] != REC_EMPTY) b |= (uint8_t)(1u << s);
    occ[n] = b;
  }
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m] || w->mite_tgt[m] == w->mite_cell[m]) continue;  /* dead or at rest */
    for (int n = 0; n < NEST_COUNT; n++) if (w->mite_tgt[m] == w->nest_cell[n]) occ[n] |= (uint8_t)(1u << seg_of(m));
  }
  for (uint32_t m = 0; m < N_MITES; m++) {
    if (w->mite_resp[m] == 0) continue;                  /* alive */
    if (--w->mite_resp[m] > 0) continue;                 /* still dead */
    uint32_t n = nest_of(m); uint16_t nest = w->nest_cell[n];
    uint8_t bit = (uint8_t)(1u << seg_of(m));
    /* revive only if its sub-segment is free and the cell isn't already `cap`-full */
    if (!(occ[n] & bit) && popcount4(occ[n]) < cap) {
      int wcx = wc_x(nest), wcy = wc_y(nest);
      w->mite_xy[m] = xy_pack(wcx * SUB + SUB / 2 + seg_ox(m), wcy * SUB + SUB / 2 + seg_oy(m));
      w->mite_ang[m] = 0; w->mite_in[m] = 0; w->mite_vxy[m] = 0; w->mite_hit[m] = 0;
      w->mite_mode[m] = MM_WANDER; w->mite_dest[m] = REC_EMPTY;
      w->mite_cell[m] = nest; w->mite_tgt[m] = nest;
      w->mite_rec_cell[0][m] = REC_EMPTY; w->mite_rec_cell[1][m] = REC_EMPTY;
      w->mite_rec_time[0][m] = 0;         w->mite_rec_time[1][m] = 0;
      w->mite_resp[m] = 0;
      w->mite_list[(uint32_t)nest * MITE_CAP + seg_of(m)] = (uint16_t)m;   /* into its sub-segment slot */
      w->mite_cnt[nest]++; occ[n] |= bit;
    } else {
      w->mite_resp[m] = 1;                                /* nest sub-segment busy: retry next tick */
    }
  }
}
