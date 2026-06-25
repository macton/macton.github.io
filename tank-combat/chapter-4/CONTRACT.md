# Chapter 4 contract — the view is a projection

The central promise of this chapter is a single falsifiable claim:

> **The view is a projection: the simulation does not know it is being drawn.** For
> the same seed and the same inputs, chapter 4 produces the **identical sim
> state-hash as chapter 3, tick for tick.** Everything that changed is the render
> half; the model did not move.

This is pinned by a test. If the hash diverges, presentation has leaked into the
model and the chapter has failed its own thesis.

## The frozen simulation (the thesis)

- The simulation sources — everything under `src/` that is not `render.c` /
  `render.h` / `iso.h` / `mesh_data.*` and not `wasm.c`'s render exports — are
  **byte-for-byte identical to chapter 3**. `sim.c`, `mites.c`, `tanks_fire.c`,
  `tanks_path.c`, `agent_turn`/`agent_move`, `collide.c`, the path tables, the
  per-cell index, the gossip, the route fields, the RNG: unchanged.
- **The chapter-3 sim/gossip/cap/combat/determinism tests pass unchanged** — same
  checks, same counts (115 of them).
- A regression pins the contract: `scripted_run` runs a fixed seed + scripted inputs
  (auto-path + manual tanks, two wall edits, a mid-run re-seed) for `HASH_TICKS`
  ticks, and `sim_state_hash` folds the **whole** sim state — movement, routing,
  turrets, the FX ring, the swarm SoA + gossip records, the shared route fields, the
  RNG, the frame. The result must equal the golden literal `CH3_GOLDEN_HASH =
  0xFB9DAC47`, captured by running the identical routine against chapter-3's sources.

## The projection (`iso.h`)

- A **pure function**, the only new math:
  `screenX = (wx - wy) * ISO_X`, `screenY = (wx + wy) * ISO_Y - wz * ISO_Z`.
- **Dimetric 2:1**: `ISO_X : ISO_Y == 2 : 1` (a `_Static_assert` pins it). The view
  is **orthographic** (no perspective).
- It lives in the **shader** (the GPU reads `ISO_X/Y/Z` from the view uniform, fed by
  wasm exports — one source of truth) and in the header for the native test.
  `render.c` emits **world placements**, never baked screen positions, so the camera
  is a uniform (zoom + follow offset), not a rebuild. Test: `iso_project` is linear,
  so a camera pan is a constant offset added to every placement — it distorts nothing.

## Depth

- **Opaque** geometry (terrain, tanks, mites, nests) uses a **z-buffer**: each vertex's
  depth is `wx + wy + wz` mapped into NDC; the GPU resolves occlusion, no CPU sort. A
  **depth texture** is added to the render pass — new *render* state, not sim state.
- The depth key is **monotone along the view diagonal** (tested): stepping south,
  east, or up strictly increases nearness, so a north-west thing never sorts in front
  of a south-east one.
- **Translucent** FX (laser glow, destruction bursts) draw in a **second pass** after
  the opaque one, depth-test on / depth-write off, **painter-sorted back-to-front** by
  `wx + wy + wz`.

## Shading

- **Flat per-face**: one directional light, `tint * (ambient + (1-ambient)·dot(n,L))`,
  computed in the shader from the face normal. For an axis-aligned block: top
  brightest, the two visible sides darker.

## Instances & visibility (`render.h`)

- One instance is a **box**, 20 bytes, all integer: `int16 wx,wy,wz,hz,hx,hy,co,si` +
  `uint32 rgba`. Growth from chapter 3's 16-byte quad is exactly **+wz +hz** — the
  third dimension — reusing facing (`co/si`) and tint (`rgba`).
- The **kind/mesh id is not a per-instance field** (the shader never reads it). It is
  the **draw grouping**: instances are emitted contiguously by kind, and the host
  draws one range per kind. `INST_MAX` ≈ 6500 (the whole world in view).
- **Render scales with visibility** (tested): the whole **4×4 world is one connected
  map** (placements are world positions, all sixteen screens in their natural layout).
  A **free camera** — drag to pan, pinch/scroll to zoom — shows any part; the host
  passes the **visible world-space box** and render emits only the cells and the
  tanks/mites/nests/FX inside it. Zoomed out, that is the whole world (one block per
  cell); zoomed in, it is a handful of cells and the few mites in view — never the
  4800-cell world or the 1000-strong pool wholesale.

## Interaction overlays (render-only)

The page's click-to-select / click-to-path interaction is made legible by overlays
that are **pure presentation** — derived from the sim, or from one host-set hover
cell; none of it writes the model:

- **Hover** (`K_HOVER`): a highlight tile on the world cell under the cursor. The host
  inverts the iso projection to a cell and passes it (with the visible box) to
  `set_view`; a cell outside the visible box emits nothing.
- **Selection** (`K_RING`): the selected tank's mode — a bright base ring + a tall
  state spike, **green for auto-path, yellow for manual**; UNSELECTED tanks draw none.
- **Destination** (`K_DEST`): a beacon, in the tank's colour, at each routing tank's
  destination cell (read from `pdest_*`).
- **Path** (`K_PATH`): a tile on each cell a routing tank will cross, traced from the
  same path tables the tank follows (`path_trace`) — the route across the connected
  screens, capped at `PATH_MAX`.

## Assets are late-bound data

- The **first pass** needs no external art: every kind binds the placeholder unit
  **cube**, shaped by the instance's half-extents (slab, wall, body, mite, pylon).
- The **second pass** binds a baked low-poly **mesh per kind** via the `MESH_FOR_KIND`
  table — the instance data is unchanged; only the bound mesh differs. Meshes are
  **host-baked once** (`tools/gen_meshes.c` → `src/mesh_data.c`, committed) and
  uploaded once at startup. The page's `low-poly art` toggle flips between the two —
  a render-only change touching neither the projection nor the sim. See `ASSETS.md`.

## Boundaries / non-promises

- `z` is **render-only**: the world stays a 2-D grid; the simulation has no height.
- No perspective camera or free orbit (the iso angle is fixed; a zoom is provided), no
  animated/rigged meshes, no shadows/AO/PBR, no 3-D minimap (it stays top-down). These
  are deferred (see `README.md`), noted here, not built.

## How it's tested

`./test.sh` builds natively (no wasm, no GPU) — the sim core **and** `render.c` are
plain C over the `World`. It runs every chapter-3 sim test unchanged, then the
chapter-4 render tests: the **sim-hash-equals-chapter-3** regression, the dimetric
projection, the monotone depth key, the linear (uniform) camera, and the
emit-for-visible bound.
