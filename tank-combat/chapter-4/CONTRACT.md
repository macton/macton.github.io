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
  `render.h` / `view.h` / `mesh_data.*` and not `wasm.c`'s render exports — are
  **byte-for-byte identical to chapter 3**. `sim.c`, `mites.c`, `tanks_fire.c`,
  `tanks_path.c`, `agent_turn`/`agent_move`, `collide.c`, the path tables, the
  per-cell index, the gossip, the route fields, the RNG: unchanged.
- **The chapter-3 sim/gossip/cap/combat/determinism tests pass unchanged** — same
  checks, same counts (127 of them).
- A regression pins the contract: `scripted_run` runs a fixed seed + scripted inputs
  (auto-path + manual tanks, two wall edits, a mid-run re-seed) for `HASH_TICKS`
  ticks, and `sim_state_hash` folds the **whole** sim state — movement, routing,
  turrets, the FX ring, the swarm SoA + gossip records, the shared route fields, the
  RNG, the frame. The result must equal the golden literal `CH3_GOLDEN_HASH =
  0x5786043F`, captured by running the identical routine against chapter-3's sources.

## The projection (`view.h` + a host-built MVP)

- Each instance carries a **3-D world placement** — a cell `(wx, wy)`, a render-only
  height `wz`, a facing, a tint — and the **vertex shader** projects it with a single
  **model-view-projection matrix**: `clip = mvp * vec4(world, 1)`, where
  `mvp = perspective · lookAt(eye, target) · scale(1/SUB)`.
- The camera is a real **top-down perspective**: by default it looks down from above and
  slightly to the **south** (~30° off straight-down), so near things are larger than far
  ones and you see a sliver of their near faces. **Pitch (tilt), yaw (orbit), fov, pan,
  and zoom are all live** — the MVP is the **view uniform**, so each is just a different
  matrix, never a geometry rebuild.
- `render.c` emits **world placements**, never baked screen positions — one source of
  truth, the host owns the camera. The only render-side constant left in C is the
  translucent painter key in `view.h`. Test: the rebuild is **byte-identical** across
  frames (the projection never reaches into the instance stream).

## Depth

- **Opaque** geometry (terrain, tanks, mites, nests) uses a **z-buffer**: the
  **perspective matrix writes real depth** (`clip.z/w`); the GPU resolves occlusion, no
  CPU sort. A **depth texture** is added to the render pass — new *render* state, not
  sim state.
- **Translucent** FX (bolt streaks, destruction bursts) draw in a **second pass** after
  the opaque one, depth-test on / depth-write off, **painter-sorted back-to-front** by
  the `view.h` key. Because the camera leans south and looks down, "nearer" grows with
  `+y` (south) and `+z` (height) and is flat in `x`: `view_depth_key(wx,wy,wz) = wy + wz`.
- The key is **monotone** in `+y` and `+z` (tested), so a north thing never sorts in
  front of a south one at the same height.

## Shading

- **Flat per-face**: one directional light, `tint * (ambient + (1-ambient)·dot(n,L))`,
  computed in the shader from the face normal. For an axis-aligned block: top
  brightest, the two visible sides darker.

## Instances & visibility (`render.h`)

- One instance is a **box**, 24 bytes, all integer: `int32 wx,wy` + `int16 wz,hz,hx,hy,co,si`
  + `uint32 rgba`. `wx,wy` are **int32** because the 8×8 arena reaches 40960 subcells,
  past int16. Growth from chapter 3's 16-byte quad is the third dimension (**+wz +hz**)
  plus the widened coordinates, reusing facing (`co/si`) and tint (`rgba`).
- The **kind/mesh id is not a per-instance field** (the shader never reads it). It is
  the **draw grouping**: instances are emitted contiguously by kind, and the host
  draws one range per kind. `INST_MAX` ≈ 24000 (the whole 8×8 world in view).
- **Render scales with visibility** (tested): the whole **8×8 world is one connected
  map** (placements are world positions, all sixty-four screens in their natural layout).
  A **free camera** — drag to pan, shift/right/two-finger-drag to orbit and tilt,
  pinch/scroll to zoom — shows any part; the host
  passes the **visible world-space box** and render emits only the cells and the
  tanks/mites/nests/FX inside it. Zoomed out, that is the whole world (one block per
  cell); zoomed in, it is a handful of cells and the few mites in view — never the
  19200-cell world or the 4096-strong pool wholesale.

## Interaction overlays (render-only)

The page's click-to-select / click-to-path interaction is made legible by overlays
that are **pure presentation** — derived from the sim, or from one host-set hover
cell; none of it writes the model:

- **Hover** (`K_HOVER`): a highlight tile on the world cell under the cursor. The host
  unprojects the cursor through the **inverse MVP** onto the ground plane to a cell and
  passes it (with the visible box) to `set_view`; a cell outside the visible box emits
  nothing.
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
- No camera roll or first-person fly-through (the camera stays an up-right orbit — yaw,
  pitch/tilt, fov, pan, and zoom are all provided), no animated/rigged meshes, no
  shadows/AO/PBR, no 3-D minimap (it stays top-down). These are deferred (see
  `README.md`), noted here, not built.

## How it's tested

`./test.sh` builds natively (no wasm, no GPU) — the sim core **and** `render.c` are
plain C over the `World`. It runs every chapter-3 sim test unchanged, then the
chapter-4 render tests: the **sim-hash-equals-chapter-3** regression, the monotone
translucent depth key (`view.h`), the byte-identical render rebuild, and the
emit-for-visible bound.
