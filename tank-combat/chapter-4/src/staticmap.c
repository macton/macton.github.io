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
static int is_road(const uint32_t* grid, int32_t x, int32_t y) {
  return !cell_is_wall(grid, x, y) && any_wall8(grid, x, y);
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
