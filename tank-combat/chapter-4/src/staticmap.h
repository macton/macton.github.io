/* staticmap.h — the STATIC TOWN, baked once from the frozen map.
 *
 * The map is static and totally known: which cells are walls, where the nests sit.
 * So the town it becomes is known too, and we bake it ONCE into instances instead of
 * re-deriving it every frame (chapter 3 re-emitted floor/wall/nest per frame; that is
 * exactly the per-frame data manipulation a static map should not need). build_static_map
 * classifies every cell — wall -> a building, an open cell touching a wall -> an
 * autotiled road, a wide-open interior -> grass, a nest cell -> a landmark — and packs
 * one Inst per cell, grouped by (screen, mesh) into a small run table. The host uploads
 * the instance buffer ONCE and, per frame, only FRUSTUM-CULLS the screen buckets and
 * draws the visible runs: no per-frame map work beyond the cull.
 *
 * It is a PURE function of (grid, nest cells): the same bake runs in wasm at init and
 * re-runs when a wall is toggled or a nest moved (the only things that change the map),
 * so the town always matches the live frozen map. The thesis holds — the sim never
 * knows it became a town; the town is a projection of the sim's walls and nests. */
#ifndef TANK_STATICMAP_H
#define TANK_STATICMAP_H

#include "render.h"          /* Inst, NEST_COUNT, COL_NEST */
#include "mesh_data.h"       /* M_PROC_COUNT — town ids are offset past the procedural meshes */
#include "map_mesh_data.h"   /* MAP_MESH_COUNT + the group bases the classify picks from */

/* Every cell yields EXACTLY one static instance (building | road | grass | landmark),
 * so the instance buffer is exactly the world's cell count and never grows. */
#define STATIC_INST_MAX  N_WORLD_CELLS
/* A run is a contiguous (screen, mesh) range; at most one per (screen, town mesh). */
#define STATIC_RUN_MAX   (N_SCREENS * MAP_MESH_COUNT)

/* one draw run: static instances [first, first+count) all share a screen and a mesh.
 * `mesh` is a UNIFIED mesh id (M_PROC_COUNT + town id), so the host draws it straight
 * through mesh_voff/mesh_vcnt against the shared mesh vertex buffer (procedural meshes
 * first, town meshes after). `screen` is the source screen (0..N_SCREENS), which gives
 * the run's world AABB for the host's frustum cull. 12 bytes, GPU-host friendly. */
typedef struct { uint16_t screen, mesh; uint32_t first, count; } StaticRun;

/* Bake the whole frozen map into the town. Classifies every world cell, writes one Inst
 * per cell into `inst` (ordered by screen then town mesh), and fills `runs` with one run
 * per non-empty (screen, mesh). Returns the instance count (== N_WORLD_CELLS); writes
 * *n_runs (<= STATIC_RUN_MAX). `grid` is the wall bitset, `nest_cell` the NEST_COUNT
 * home cells. No allocation, no sim writes. */
uint32_t build_static_map(const uint32_t* grid, const uint16_t* nest_cell,
                          Inst* inst, StaticRun* runs, uint32_t* n_runs);

/* ---- static PROPS: trees, scattered on grass-cell corners (render-only scenery) ----
 * A second static layer, baked once exactly like the town: low-poly trees (M_TREE) placed
 * at GRID-CELL CORNERS whose four surrounding cells are all grass — deep in the open, hash-
 * thinned for density, each on the ground with a slight per-corner rotation. They are pure
 * decoration: the sim never knows they exist (no collision), and the thin trunk sitting on
 * a cell boundary signals "scenery, not obstacle". Same Inst/StaticRun layout + per-screen
 * grouping as the town, so the host uploads once and frustum-culls per screen. Because trees
 * sit on corners, NOT one-per-cell, they live in their OWN buffer (the town's one-instance-
 * per-cell invariant — which the wall-shake cell->instance map relies on — stays intact). */
#define STATIC_PROP_INST_MAX  N_WORLD_CELLS          /* <= one tree per grass corner (<= cells) */
#define STATIC_PROP_RUN_MAX   N_SCREENS              /* one M_TREE run per screen */

/* Bake the trees. Scatters M_TREE instances onto all-grass corners of the frozen map, writes
 * them into `inst` ordered by screen, and fills `runs` with one run per non-empty screen.
 * Returns the instance count (<= STATIC_PROP_INST_MAX); writes *n_runs (<= STATIC_PROP_RUN_MAX).
 * A pure function of (grid, nest_cell) — re-runs with the town on a wall/nest edit. */
uint32_t build_static_props(const uint32_t* grid, const uint16_t* nest_cell,
                            Inst* inst, StaticRun* runs, uint32_t* n_runs);

#endif
