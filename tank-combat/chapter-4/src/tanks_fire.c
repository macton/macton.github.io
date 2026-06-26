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

/* The same at the finer 256-step TURRET resolution: the nearest of 256 headings to
 * (dx,dy), as a Q5.11 angle (low 8 bits zero). The turret aims and fires along this so it
 * tracks smoothly and points precisely; only target SELECTION still uses aim_angle (32). */
static uint16_t aim_fine(int dx, int dy) {
  int best = -2147483647; uint32_t bd = 0;
  for (uint32_t d = 0; d < N_FINE; d++) {
    int dot = dx * FINE_COS[d] + dy * FINE_COS[(d - FINE_QUARTER) & (N_FINE - 1)];
    if (dot > best) { best = dot; bd = d; }
  }
  return (uint16_t)(bd << 8);
}

/* Turn `cur` toward `want` by at most `rate`, the shortest way round, clamping so it
 * never overshoots — the turret's equivalent of the bodies' agent_turn step. */
static uint16_t turn_toward(uint16_t cur, uint16_t want, int rate) {
  int d = (int16_t)(want - cur);                 /* shortest signed delta (Q5.11 wraps) */
  if (d >  rate) d =  rate;
  if (d < -rate) d = -rate;
  return (uint16_t)((int)cur + d);
}

/* record a cosmetic burst (a mite kill or a wall impact) in the ring; overwrite the oldest */
static void spawn_fx(World* w, uint32_t xy, uint8_t kind) {
  w->fx_xy  [w->fx_head] = xy;
  w->fx_kind[w->fx_head] = kind;
  w->fx_t   [w->fx_head] = FX_DURATION;
  w->fx_head = (uint8_t)((w->fx_head + 1u) % N_FX);
}

/* jolt a struck wall cell briefly (cosmetic ring; refresh it if already shaking) */
static void shake_wall(World* w, uint16_t cell) {
  for (uint32_t i = 0; i < WALL_SHAKE_MAX; i++)
    if (w->wall_shake_t[i] && w->wall_shake_cell[i] == cell) { w->wall_shake_t[i] = WALL_SHAKE_DUR; return; }
  w->wall_shake_cell[w->wall_shake_head] = cell;
  w->wall_shake_t   [w->wall_shake_head] = WALL_SHAKE_DUR;
  w->wall_shake_head = (uint8_t)((w->wall_shake_head + 1u) % WALL_SHAKE_MAX);
}

/* destroy mite m: a burst, mark it dead (respawn timer), freeze the corpse, and the
 * "death cry" — every live mite within 2x sensing range of it learns the firing tank's
 * cell (record stamped now + hunt), written into the current record buffer (read next
 * tick), so a kill turns the nearby swarm on its attacker. */
static void kill_mite(World* w, uint32_t m, uint16_t tcell, uint32_t frame) {
  uint16_t deadcell = w->mite_cell[m];
  spawn_fx(w, w->mite_xy[m], FX_KILL);
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
static void proj_step(World* w, uint32_t b, uint32_t frame) {
  uint32_t t = b / PROJ_MAX;                              /* the firing tank owns slots t*PROJ_MAX .. +PROJ_MAX-1 */
  int px = xy_lo(w->tank_proj_xy[b]), py = xy_hi(w->tank_proj_xy[b]);
  int co = fine_cos(w->tank_proj_dir[b]), si = fine_sin(w->tank_proj_dir[b]);  /* the bolt flies the fine turret angle */
  uint16_t pt = w->tank_proj_tgt[b];                      /* the aimed mite — killed on arrival regardless of perp */

  int step = w->proj_speed;                               /* this tick's reach (subcells, page-tunable) */
  int rng_left = (int)PROJ_RANGE * SUB - (int)w->tank_proj_dist[b];
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

  if (hitwall) {                                           /* spent against the wall — splash an impact there */
    int iwx = wrap_wcx(cx), iwy = wrap_wcy(cy);            /* the wall cell struck (extended -> wrapped) */
    int impx = iwx * SUB + SUB / 2 - ((co * (SUB / 2)) >> TRIG_SHIFT);   /* pulled back to the near face */
    int impy = iwy * SUB + SUB / 2 - ((si * (SUB / 2)) >> TRIG_SHIFT);
    spawn_fx(w, xy_pack(wrap_x(impx), wrap_y(impy)), FX_IMPACT);
    shake_wall(w, (uint16_t)wc_pack(iwx, iwy));           /* and jolt the wall segment it struck */
    w->tank_proj_live[b] = 0;
    return;
  }
  w->tank_proj_xy[b]   = xy_pack(wrap_x(ex), wrap_y(ey));  /* advance the head (torus-wrapped) */
  w->tank_proj_dist[b] = (uint16_t)(w->tank_proj_dist[b] + step);
  if (w->tank_proj_dist[b] >= PROJ_RANGE * SUB) w->tank_proj_live[b] = 0;  /* flew its full range */
}

/* Would a bolt fired from tank t along (co,si) reach ANOTHER tank before a wall or its range?
 * If so the shot is held — tanks don't fire through each other. A friendly counts as "in the
 * path" when it lies ahead (along >= 0), within the bolt's reach (<= PROJ_RANGE), inside a
 * body-plus-beam-width corridor of the line, and with clear line of sight (a wall between would
 * stop the bolt first). The mites the bolt is meant for sit between, so a near friendly behind
 * the target still blocks the shot — the swarm is spared a little, but no tank is ever hit. */
static int friendly_in_path(World* w, uint32_t t, int co, int si) {
  int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
  int tcx = wrap_wcx(tx >> SUB_SHIFT), tcy = wrap_wcy(ty >> SUB_SHIFT);
  for (uint32_t u = 0; u < N_TANKS; u++) {
    if (u == t) continue;
    int vx = xy_lo(w->tank_xy[u]) - tx; if (vx >  ARENA_W_SUB / 2) vx -= ARENA_W_SUB; if (vx < -ARENA_W_SUB / 2) vx += ARENA_W_SUB;
    int vy = xy_hi(w->tank_xy[u]) - ty; if (vy >  ARENA_H_SUB / 2) vy -= ARENA_H_SUB; if (vy < -ARENA_H_SUB / 2) vy += ARENA_H_SUB;
    int along = (vx * co + vy * si) >> TRIG_SHIFT;
    if (along < 0 || along > PROJ_RANGE * SUB) continue;          /* behind us, or beyond the bolt's reach */
    int perp = (vx * si - vy * co) >> TRIG_SHIFT; if (perp < 0) perp = -perp;
    if (perp > TANK_R + PROJ_HW) continue;                        /* clear of the body-plus-beam corridor */
    int ucx = wrap_wcx(xy_lo(w->tank_xy[u]) >> SUB_SHIFT), ucy = wrap_wcy(xy_hi(w->tank_xy[u]) >> SUB_SHIFT);
    if (los(w->grid, tcx, tcy, ucx, ucy)) return 1;              /* clear line to it: a bolt would hit it */
  }
  return 0;
}

void tanks_fire(World* w) {
  uint32_t frame = w->frame;
  int period = w->fire_period;
  int rate = w->turret_rate;

  for (uint32_t i = 0; i < N_FX; i++) if (w->fx_t[i]) w->fx_t[i]--;   /* age the bursts */
  for (uint32_t i = 0; i < WALL_SHAKE_MAX; i++) if (w->wall_shake_t[i]) w->wall_shake_t[i]--;  /* and the wall shakes */

  for (uint32_t t = 0; t < N_TANKS; t++) {
    /* every bolt this tank has in flight pierces on this tick (and may expire at a wall / its
     * range), independent of where the turret now points — firing does not pin the bolt to the aim. */
    for (uint32_t s = 0; s < PROJ_MAX; s++)
      if (w->tank_proj_live[t * PROJ_MAX + s]) proj_step(w, t * PROJ_MAX + s, frame);

    int tx = xy_lo(w->tank_xy[t]), ty = xy_hi(w->tank_xy[t]);
    int tcx = wrap_wcx(tx >> SUB_SHIFT), tcy = wrap_wcy(ty >> SUB_SHIFT);
    uint16_t turret = w->tank_turret[t];

    /* run-over: a MOVING tank (the size of one grid cell) squashes any live mite under its
     * footprint — a one-cell box around its centre — found over the 3x3 cells of the index.
     * A still tank is harmless; the kill is an ordinary death (burst + death cry). */
    if (w->tank_vxy[t]) {
      uint16_t crush_cell = (uint16_t)wc_pack(tcx, tcy);
      for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
        uint32_t c = (uint32_t)wc_pack(wrap_wcx(tcx + ox), wrap_wcy(tcy + oy));
        if (!w->mite_cnt[c]) continue;
        for (uint8_t s = 0; s < MITE_CAP; s++) {
          uint32_t m = w->mite_list[c * MITE_CAP + s];
          if (m == REC_EMPTY || w->mite_resp[m]) continue;       /* empty / already dead */
          int dx = xy_lo(w->mite_xy[m]) - tx; if (dx >  ARENA_W_SUB / 2) dx -= ARENA_W_SUB; if (dx < -ARENA_W_SUB / 2) dx += ARENA_W_SUB;
          int dy = xy_hi(w->mite_xy[m]) - ty; if (dy >  ARENA_H_SUB / 2) dy -= ARENA_H_SUB; if (dy < -ARENA_H_SUB / 2) dy += ARENA_H_SUB;
          if (dx >= -SUB / 2 && dx <= SUB / 2 && dy >= -SUB / 2 && dy <= SUB / 2) kill_mite(w, m, crush_cell, frame);
        }
      }
    }

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
        uint16_t cang = aim_angle(ox * SUB, oy * SUB);             /* the cell's bearing (32-dir) */
        /* skip a candidate whose shot would pass through a friendly tank — re-target to a clear
         * one instead of locking a mite we'd only have to hold fire on. */
        uint32_t cd = (uint32_t)(cang >> ANGLE_SHIFT);
        if (friendly_in_path(w, t, dir_cos(cd), dir_sin(cd))) continue;
        int ad = (int16_t)(cang - turret); if (ad < 0) ad = -ad;
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
      want = (ddx | ddy) ? aim_fine(ddx, ddy) : w->tank_ang[t];
    } else {
      want = w->tank_ang[t];
    }
    /* swing the turret toward the desired aim at its turn rate — ALWAYS, even with a bolt in
     * flight: firing no longer locks the turret, so the moment a shot is away the barrel is
     * free to line up the next target while the bolt is still travelling. */
    w->tank_turret[t] = turn_toward(turret, want, rate);

    if (w->tank_cooldown[t] > 0) w->tank_cooldown[t]--;

    /* fire only once the barrel has swung EXACTLY onto the target bearing (so the bolt leaves
     * straight along the barrel) and the cooldown is ready (period 0 = off) — and NOT if the shot
     * would pass through a friendly tank (tanks hold fire through each other). The cadence is the
     * cooldown; a tank may have up to PROJ_MAX bolts aloft at once, so a slow bolt no longer
     * throttles the fire rate — only when ALL slots are full does firing wait. The shot is a
     * PROJECTILE: a piercing bolt from the muzzle that mows the line ahead (proj_step flies it). */
    if (period > 0 && target != TGT_NONE && w->tank_turret[t] == want && w->tank_cooldown[t] == 0
        && !friendly_in_path(w, t, fine_cos(w->tank_turret[t]), fine_sin(w->tank_turret[t]))) {
      uint32_t slot = PROJ_MAX;                             /* the first free bolt slot, if any */
      for (uint32_t s = 0; s < PROJ_MAX; s++) if (!w->tank_proj_live[t * PROJ_MAX + s]) { slot = s; break; }
      if (slot < PROJ_MAX) {
        uint32_t b = t * PROJ_MAX + slot;
        w->tank_proj_xy[b]   = w->tank_xy[t];        /* spawn at the tank, heading along the barrel */
        w->tank_proj_dir[b]  = w->tank_turret[t];
        w->tank_proj_dist[b] = 0;
        w->tank_proj_tgt[b]  = (uint16_t)target;      /* the bolt carries its target for a sure kill */
        w->tank_proj_live[b] = 1;
        w->tank_cooldown[t]  = (uint16_t)period;
      }
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
