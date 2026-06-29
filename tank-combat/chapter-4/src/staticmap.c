/* staticmap.c — bake the frozen map into a static town (see staticmap.h).
 *
 * The treatment, validated against the real frozen map before wiring:
 *   nest cell            -> a LANDMARK, tinted in that nest's hue (keeps nest identity)
 *   wall cell            -> a BUILDING (hash-picked variant + 90-deg rotation, for variety)
 *   open cell touching a wall  -> a ROAD, AUTOTILED to its road neighbours (streets hug
 *                                 the building blocks, following the maze corridors)
 *   wide-open interior   -> GRASS
 * Town meshes carry their own palette colour, so the instance tint is white (the shader
 * does meshColour * tint) — except the landmark, tinted per nest. Wall queries wrap
 * toroidally (cell_is_wall), matching the sim's actual topology, so the whole world
 * tiles seamlessly with no edge special-cases. */
#include "staticmap.h"
#include "collide.h"   /* cell_is_wall (toroidal) */

/* footprint half-extent: the mesh is normalised to a unit [-1,1] footprint, so SUB/2
 * scales it to exactly one cell. Heights come from MAP_MESH_HZ (the baked half-height);
 * wz == hz puts the base on the ground (z=0). */
#define CELL_HALF (SUB / 2)

/* rotation -> Q14 cos/sin, for the four cardinal road/building orientations */
static const int16_t Q14_COS[4] = { 16384, 0, -16384, 0 };   /* 0, 90, 180, 270 deg */
static const int16_t Q14_SIN[4] = { 0, 16384, 0, -16384 };

/* the same integer cell hash the preview used: a stable per-cell pseudo-random for the
 * building variant + rotation. uint32 multiply wraps mod 2^32, matching the JS (x*k)^... */
static uint32_t cell_hash(int32_t x, int32_t y) {
  return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
}

/* a road cell = an OPEN cell with at least one wall in its 8-neighbourhood (so streets
 * run alongside the building blocks; interiors with no adjacent wall become grass). */
static int any_wall8(const uint32_t* grid, int32_t x, int32_t y) {
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
      if ((dx || dy) && cell_is_wall(grid, x + dx, y + dy)) return 1;
  return 0;
}
/* wrap a (possibly out-of-range) cell coordinate into the toroidal world */
static int wrap_c(int v, int n) { v %= n; return v < 0 ? v + n : v; }

/* The RENDERED road map, baked once. A cell renders as road iff it is open, not a nest, and has
 * a wall in its 8-neighbourhood. A building cluster stranded in the grass otherwise becomes a
 * CLOSED RING of road touching no other street — a "traffic island" you can't drive off of.
 * compute_road_map() lays that base ring down, then BRIDGES every isolated ring to the main
 * network by carving the shortest grass corridor to it. Render-only: a corridor cell just
 * classifies as road; the sim never sees a road, a ring, or a bridge. is_road() reads this map,
 * so the autotile and the grass/tree split all follow the connected result. */
static uint8_t s_road[N_WORLD_CELLS];     /* 1 = renders as road (base ring + connectivity bridges) */
static int32_t s_comp[N_WORLD_CELLS];     /* connected-component label per road cell (-1 = none) */
static int32_t s_bfs[N_WORLD_CELLS];      /* BFS queue (cell indices) */
static int32_t s_prev[N_WORLD_CELLS];     /* BFS parent for path reconstruction (-1 source, -2 unseen) */
static uint8_t s_conn[N_WORLD_CELLS];     /* 1 = road cell proven connected to the main network */

static int is_road(const uint32_t* grid, int32_t x, int32_t y) {
  (void)grid; return s_road[wc_pack(wrap_c(x, BIG_W), wrap_c(y, BIG_H))];
}

/* Bake s_road: the base ring, then bridge every isolated ring to the main road network so no
 * circular road section is left unconnected. A pure function of (grid, nest_at); rebuilt with
 * the town on a wall/nest edit. */
static void compute_road_map(const uint32_t* grid, const int16_t* nest_at) {
  static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
  /* base ring: open, non-nest, a wall in the 8-neighbourhood */
  for (int y = 0; y < BIG_H; y++)
    for (int x = 0; x < BIG_W; x++) {
      uint32_t c = wc_pack(x, y);
      s_road[c] = (!cell_is_wall(grid, x, y) && nest_at[c] < 0 && any_wall8(grid, x, y)) ? 1 : 0;
    }
  /* label the 4-connected road components (toroidal); the largest is the main street network */
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) s_comp[c] = -1;
  int ncomp = 0, mainc = -1, mainsz = -1;
  for (int y = 0; y < BIG_H; y++)
    for (int x = 0; x < BIG_W; x++) {
      uint32_t c0 = wc_pack(x, y);
      if (!s_road[c0] || s_comp[c0] >= 0) continue;
      int head = 0, tail = 0, sz = 0; s_bfs[tail++] = (int32_t)c0; s_comp[c0] = ncomp;
      while (head < tail) {
        int c = s_bfs[head++]; sz++; int cx = c % BIG_W, cy = c / BIG_W;
        for (int k = 0; k < 4; k++) {
          uint32_t nc = wc_pack(wrap_c(cx + DX[k], BIG_W), wrap_c(cy + DY[k], BIG_H));
          if (s_road[nc] && s_comp[nc] < 0) { s_comp[nc] = ncomp; s_bfs[tail++] = (int32_t)nc; }
        }
      }
      if (sz > mainsz) { mainsz = sz; mainc = ncomp; }
      ncomp++;
    }
  if (ncomp <= 1) return;   /* already one network — nothing stranded */
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) s_conn[c] = (s_comp[c] == mainc) ? 1 : 0;
  /* connect each non-main ring to the network by the shortest GRASS corridor: a multi-source BFS
   * from the ring's cells, stepping only through grass (open, non-nest, non-road) until it meets
   * a connected road cell, then carve that path into road. Islands already merged (e.g. a prior
   * bridge ran past them) come pre-connected, so the loop converges to one network. */
  for (int comp = 0; comp < ncomp; comp++) {
    if (comp == mainc) continue;
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) s_prev[c] = -2;
    int head = 0, tail = 0, found = -1, already = 0;
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++)
      if (s_road[c] && s_comp[c] == comp) { if (s_conn[c]) already = 1; s_prev[c] = -1; s_bfs[tail++] = (int32_t)c; }
    if (already) continue;   /* this ring was already linked in by an earlier bridge */
    while (head < tail && found < 0) {
      int c = s_bfs[head++]; int cx = c % BIG_W, cy = c / BIG_W;
      for (int k = 0; k < 4; k++) {
        int nx = wrap_c(cx + DX[k], BIG_W), ny = wrap_c(cy + DY[k], BIG_H);
        uint32_t nc = wc_pack(nx, ny);
        if (s_prev[nc] != -2) continue;                                  /* already seen */
        if (cell_is_wall(grid, nx, ny) || nest_at[nc] >= 0) continue;    /* never carve through a building/landmark */
        if (s_road[nc] && !s_conn[nc]) continue;                         /* don't route through another island */
        s_prev[nc] = c;
        if (s_conn[nc]) { found = (int)nc; break; }                      /* reached the network */
        s_bfs[tail++] = (int32_t)nc;                                     /* grass: keep walking */
      }
    }
    if (found < 0) continue;   /* sealed off by walls — handled in the cleanup pass below */
    for (int c = found; c >= 0; c = s_prev[c]) { s_road[c] = 1; s_conn[c] = 1; }   /* carve the corridor */
    for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (s_comp[c] == comp) s_conn[c] = 1;  /* the ring is connected now */
  }
  /* any ring we still couldn't reach is sealed INSIDE a building block (no grass to bridge
   * through, and we won't carve a road across a wall — the sim has a building there). Rather
   * than leave a stranded patch of asphalt, drop it back to grass (an open courtyard), so every
   * remaining road cell belongs to the one connected network. */
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) if (s_road[c] && !s_conn[c]) s_road[c] = 0;
}

/* road autotile. Neighbour mask: N=1 E=2 S=4 W=8 (bit set if that neighbour is road).
 * Each road mesh has a CANONICAL connection mask MEASURED FROM THE ART at bake time
 * (MAP_ROAD_CANON, derived from where each tile's asphalt reaches an edge — hand-guessing
 * it is exactly how the streets first came out 180 degrees wrong); rotate it (+90 deg world
 * = N->E->S->W) until it matches the cell's mask, and that k*90 is the tile's rotation. The
 * ROAD group is baked square,end,straight,bend,intersection,crossroad. */
static uint8_t rot90_mask(uint8_t m) {
  uint8_t r = 0;
  if (m & 1) r |= 2;   /* N -> E */
  if (m & 2) r |= 4;   /* E -> S */
  if (m & 4) r |= 8;   /* S -> W */
  if (m & 8) r |= 1;   /* W -> N */
  return r;
}
/* pick the road mesh (town id) + rotation index (0..3) for the cell's neighbours */
static int road_tile(const uint32_t* grid, int32_t x, int32_t y, int* rot_idx) {
  uint8_t m = 0;
  if (is_road(grid, x, y - 1)) m |= 1;   /* N */
  if (is_road(grid, x + 1, y)) m |= 2;   /* E */
  if (is_road(grid, x, y + 1)) m |= 4;   /* S */
  if (is_road(grid, x - 1, y)) m |= 8;   /* W */
  int cnt = ((m & 1) ? 1 : 0) + ((m & 2) ? 1 : 0) + ((m & 4) ? 1 : 0) + ((m & 8) ? 1 : 0);
  int idx = cnt >= 4 ? 5                              /* crossroad */
          : cnt == 3 ? 4                              /* intersection */
          : cnt == 2 ? ((m == 10 || m == 5) ? 2 : 3) /* straight (E|W or N|S) : bend */
          : cnt == 1 ? 1                              /* end */
                     : 0;                             /* square (isolated) */
  uint8_t cm = MAP_ROAD_CANON[idx];
  for (int k = 0; k < 4; k++) { if (cm == m) { *rot_idx = k; return MAP_ROAD_BASE + idx; }
                                cm = rot90_mask(cm); }
  *rot_idx = 0;
  return MAP_ROAD_BASE + idx;
}

/* classify one world cell -> (town mesh id, rotation index, tint). nest_at[cell] is the
 * nest index there, or -1. */
static void classify(const uint32_t* grid, const int16_t* nest_at, int32_t x, int32_t y,
                     int* mesh, int* rot_idx, uint32_t* rgba) {
  int16_t nest = nest_at[wc_pack(x, y)];
  *rgba = 0xFFFFFFFFu;   /* white: town meshes carry their own colour */
  *rot_idx = 0;
  if (nest >= 0) {                                  /* nest -> landmark, tinted by hue */
    *mesh = MAP_LANDMARK_BASE; *rgba = COL_NEST[nest];
  } else if (!cell_is_wall(grid, x, y)) {           /* open -> road or grass */
    *mesh = is_road(grid, x, y) ? road_tile(grid, x, y, rot_idx) : MAP_GRASS_BASE;
  } else {                                          /* wall -> a building */
    uint32_t h = cell_hash(x, y);
    *mesh = MAP_BUILD_BASE + (int)(h % MAP_BUILD_N);
    *rot_idx = (int)((h >> 3) % 4u);
  }
}

/* per-screen classification scratch (file-static: keeps it off the stack, reused each
 * screen). One entry per cell-in-screen. */
static int      s_mesh[GRID_W * GRID_H];
static int      s_rot[GRID_W * GRID_H];
static uint32_t s_rgba[GRID_W * GRID_H];
static int16_t  s_nest_at[N_WORLD_CELLS];   /* reverse map: nest index per cell, or -1 */

uint32_t build_static_map(const uint32_t* grid, const uint16_t* nest_cell,
                          Inst* inst, StaticRun* runs, uint32_t* n_runs) {
  /* reverse the nest list into a per-cell lookup for the classify */
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) s_nest_at[c] = -1;
  for (uint32_t n = 0; n < NEST_COUNT; n++) s_nest_at[nest_cell[n]] = (int16_t)n;
  compute_road_map(grid, s_nest_at);   /* bake the connected road map (base ring + island bridges) */

  uint32_t ni = 0, nr = 0;
  /* walk screen by screen so instances bucket naturally by (screen, mesh): the host
   * frustum-culls whole screens, then draws each visible screen's per-mesh runs. */
  for (int sy = 0; sy < SCREENS_Y; sy++)
    for (int sx = 0; sx < SCREENS_X; sx++) {
      int s = sy * SCREENS_X + sx, bx = sx * GRID_W, by = sy * GRID_H;
      /* classify the screen's cells once */
      for (int ly = 0; ly < GRID_H; ly++)
        for (int lx = 0; lx < GRID_W; lx++) {
          int i = ly * GRID_W + lx;
          classify(grid, s_nest_at, bx + lx, by + ly, &s_mesh[i], &s_rot[i], &s_rgba[i]);
        }
      /* emit one contiguous run per BASE town mesh used in this screen (LOD0 ids only — the LOD2
       * massing past MAP_LOD2_OFFSET and the separate imposter table are swapped in by the host) */
      for (int m = 0; m < MAP_LOD2_OFFSET; m++) {
        uint32_t first = ni;
        int hz = MAP_MESH_HZ[m];
        /* lay GRASS flush with the street: the Kenney grass tile bakes as a raised block (hz ~26),
         * which sat well above the flat road (hz ~3) and buried the path-overlay tiles. Squash it
         * to the road's height so grass and street share a surface and the path blocks read on top. */
        if (m == MAP_GRASS_BASE) hz = MAP_MESH_HZ[MAP_ROAD_BASE];
        for (int ly = 0; ly < GRID_H; ly++)
          for (int lx = 0; lx < GRID_W; lx++) {
            int i = ly * GRID_W + lx;
            if (s_mesh[i] != m) continue;
            Inst* o = &inst[ni++];
            o->wx = (bx + lx) * SUB + SUB / 2; o->wy = (by + ly) * SUB + SUB / 2;
            o->wz = (int16_t)hz; o->hz = (int16_t)hz;
            o->hx = CELL_HALF;   o->hy = CELL_HALF;
            o->co = Q14_COS[s_rot[i]]; o->si = Q14_SIN[s_rot[i]]; o->rgba = s_rgba[i];
          }
        if (ni > first) {
          runs[nr].screen = (uint16_t)s; runs[nr].mesh = (uint16_t)(M_PROC_COUNT + m);
          runs[nr].first = first; runs[nr].count = ni - first; nr++;
        }
      }
    }
  *n_runs = nr;
  return ni;
}

/* ---- static PROPS: trees on grass corners (render-only scenery; see staticmap.h) ----
 * A grass cell is the town's "wide-open interior": open (not a wall), not a road (no wall in
 * its 8-neighbourhood), and not a nest. cell_is_wall / is_road already wrap toroidally; the
 * nest lookup wraps explicitly so the scatter tiles seamlessly like the town. */
#define TREE_DENSITY  22    /* % of eligible (all-grass) corners that grow a tree */

static int is_grass(const uint32_t* grid, const int16_t* nest_at, int32_t x, int32_t y) {
  if (nest_at[wc_pack(wrap_c(x, BIG_W), wrap_c(y, BIG_H))] >= 0) return 0;   /* nest -> landmark */
  if (cell_is_wall(grid, x, y)) return 0;                                    /* wall -> building */
  if (is_road(grid, x, y)) return 0;                                         /* by a wall -> road */
  return 1;                                                                  /* open interior -> grass */
}
/* a corner (cx,cy) — the shared vertex of cells (cx-1,cy-1),(cx,cy-1),(cx-1,cy),(cx,cy), at
 * world (cx*SUB, cy*SUB) — grows a tree iff all four cells are grass and the hash passes. */
static int corner_has_tree(const uint32_t* grid, const int16_t* nest_at, int32_t cx, int32_t cy) {
  if (!is_grass(grid, nest_at, cx - 1, cy - 1) || !is_grass(grid, nest_at, cx, cy - 1) ||
      !is_grass(grid, nest_at, cx - 1, cy)     || !is_grass(grid, nest_at, cx, cy)) return 0;
  return (int)(cell_hash(cx, cy) % 100u) < TREE_DENSITY;
}

uint32_t build_static_props(const uint32_t* grid, const uint16_t* nest_cell,
                            Inst* inst, StaticRun* runs, uint32_t* n_runs) {
  for (uint32_t c = 0; c < N_WORLD_CELLS; c++) s_nest_at[c] = -1;
  for (uint32_t n = 0; n < NEST_COUNT; n++) s_nest_at[nest_cell[n]] = (int16_t)n;
  compute_road_map(grid, s_nest_at);   /* same road map the town bakes, so trees avoid the bridges too */

  uint32_t ni = 0, nr = 0;
  /* walk screen by screen so trees bucket by screen (all share M_TREE); a corner is owned
   * by the screen of its lower-right cell (cx,cy), which is where the corner's world position
   * sits inside the screen AABB the host culls against. */
  for (int sy = 0; sy < SCREENS_Y; sy++)
    for (int sx = 0; sx < SCREENS_X; sx++) {
      int s = sy * SCREENS_X + sx, bx = sx * GRID_W, by = sy * GRID_H;
      uint32_t first = ni;
      for (int ly = 0; ly < GRID_H; ly++)
        for (int lx = 0; lx < GRID_W; lx++) {
          int32_t cx = bx + lx, cy = by + ly;
          if (!corner_has_tree(grid, s_nest_at, cx, cy)) continue;
          /* hash-driven size variation (no rotation — the canopy is square-symmetric). The
           * tree spans the unit box z[-1,1]; wz==hz drops its base onto the ground (z=0). */
          uint32_t h = cell_hash(cx, cy);
          int v = (int)((h >> 11) % 64u);          /* 0..63 */
          int16_t hz = (int16_t)(176 + v * 2);     /* half-height  176..302 (height 352..604) */
          int16_t hr = (int16_t)(84 + v);          /* footprint half 84..147 (thin trunk = 0.13*hr) */
          Inst* o = &inst[ni++];
          o->wx = cx * SUB; o->wy = cy * SUB;      /* the corner (cell min-corner), not the centre */
          o->wz = hz; o->hz = hz; o->hx = hr; o->hy = hr;
          o->co = Q14_COS[0]; o->si = Q14_SIN[0]; o->rgba = 0xFFFFFFFFu;   /* white: tree carries its own colours */
        }
      if (ni > first) {
        runs[nr].screen = (uint16_t)s; runs[nr].mesh = (uint16_t)M_TREE;
        runs[nr].first = first; runs[nr].count = ni - first; nr++;
      }
    }
  *n_runs = nr;
  return ni;
}
