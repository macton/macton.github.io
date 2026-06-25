# Prompt — Create Chapter 4: The View (isometric 3D)

You are building **chapter 4** of the interactive book that teaches *data-oriented
design* by growing one small game, one feature at a time. Chapter 1 is a two-tank
Atari-*Combat* game; chapter 2 grew the world to a 4×4 toroidal grid of screens with
self-pathing tanks; chapter 3 added an enemy faction — a thousand gossiping **mites**
that swarm the tanks, with combat. Chapter 4 changes **nothing about the
simulation** and **everything about the picture**: the same world, the same tanks,
the same thousand mites, redrawn as an **isometric 3D** scene.

Each chapter is a self-contained web page: the running game on top, a written
walk-through below, and the game's live data shown and editable *in the context of
the text*.

## The lesson — the render is a projection; the simulation does not know it is drawn

Chapters 1–3 earned the payoff this chapter spends: a strict **sim / render / wasm
split with a one-way dependency** (render reads the sim; the sim never reads the
render). Chapter 4 is the proof. We replace the top-down 2D view with a full
**isometric 3D** view — new projection, depth, shading, camera, and art — and we do
it **without editing a single line of the simulation**. `sim.c`, `mites.c`,
`tanks_fire.c`, `tanks_path.c`, `agent_turn`/`agent_move`, the per-cell index, the
gossip, the route fields, the RNG — all **byte-for-byte identical to chapter 3**. The
only things that change are the **render half** (`render.c`, the WGSL shader, the view
uniform, `app.js`) and a folder of **art assets**.

That is the whole point, stated as a falsifiable claim and pinned by a test: *for the
same seed and the same inputs, chapter 4 produces the identical sim state-hash as
chapter 3, tick for tick.* If the hash diverges, presentation leaked into the model
and the chapter has failed its own thesis. The DOD rules this chapter exercises are
**one-way dependency**, **presentation-only data stays out of the sim**, **match the
work to the frequency of change** (the geometry of the world changes on a wall edit —
rare; the projection runs every frame — constant; the assets load once), and **render
scales with what is visible, not with the population**.

A second, smaller lesson rides along: assets are **late-bound data**. We ship a first
pass with **zero external art** — the scene is legible from procedural primitives
alone — then swap in real low-poly models in a second pass that touches only the
asset table and the kind→mesh binding. The picture is data you replace; the pipeline
that draws it does not move.

## Starting point — copy chapter 3 verbatim, then freeze the sim

1. Copy `tank-combat/chapter-3/` to `tank-combat/chapter-4/` verbatim and get it
   building/running first (`build.sh`, `test.sh`) before changing anything. Chapter 4
   is an *increment* on chapter 3, not a rewrite.
2. **Freeze the simulation.** The sim sources — everything under `src/` that is not
   `render.c`/`render.h`/`wasm.c`'s render exports, plus `app.js`'s draw path — are
   **closed for edits** in this chapter. Treat any need to touch them as a design
   smell to escalate, not a license to refactor. The chapter-3 native sim/gossip/cap/
   combat/determinism tests must keep passing **unchanged** (same checks, same
   counts). Add a regression that the **state-hash after K ticks equals chapter 3's**
   for a fixed seed + scripted inputs — copy chapter 3's hash into the test as the
   golden value. This is the chapter's contract.
3. **Strip and rewrite the page's article text** for the new concepts (the projection,
   depth, shading, the asset pipeline), keeping the page *structure*: the canvas, the
   controls, the follow camera + screen picker, the **top-down minimap** (keep it
   top-down — see below), the live debug-widget anchors, the version badge, the
   styling.
4. Add a chapter-4 entry to the book's table of contents (`tank-combat/index.html`)
   and give chapter 4 its own `README.md` and `CONTRACT.md`.

## What chapter 4 adds — an isometric 3D renderer over the same data

Everything below lives in the **render half**. None of it is allowed to feed back into
the sim.

### The projection — dimetric 2:1, computed in the shader

The world is the chapter-2/3 `BIG_W × BIG_H` toroidal grid of cells in subcell units
(`SUB` per cell). Chapter 3's renderer placed each instance at a 2D screen position and
drew an axis-aligned quad. Chapter 4 keeps the **instance-per-thing** model but gives
each instance a **3-D world placement** — a cell `(wx, wy)` in subcells, a **height
`wz`** (a render-only attribute; the sim has no z), a **facing** (reuse the body /
turret angle the sim already stores), a **kind** (which mesh/sprite), and a **tint** —
and lets the **vertex shader** do the isometric projection:

    screenX = (wx - wy) * ISO_X
    screenY = (wx + wy) * ISO_Y - wz * ISO_Z

Use the classic **dimetric 2:1** ratio (`ISO_X : ISO_Y = 2 : 1`), so tiles read as the
familiar game-isometric diamond; `ISO_Z` is the world-units-to-screen height scale.
This is the **only** new math, and it is a pure function — put the iso basis, the
height scale, the ortho zoom, and the follow offset in the **view uniform** (the
camera), exactly where chapter 3 kept its 2D `scale`/`offset`. Panning, following, the
slide, and the picker stay **presentation-only** and keep working untouched. Keep the
projection in the **shader** (and the iso constants in a header so both the host and
any native test can read them) — do **not** bake screen positions into `render.c`;
`render.c` emits **world placements**, the shader projects them. That keeps the camera
a uniform, not a rebuild.

Provide a true **orthographic** camera (no perspective foreshortening) — isometric is
an orthographic projection viewed along a diagonal. Two pure functions are worth
isolating and testing: `iso_project(wx, wy, wz) → (screenX, screenY)` and the **depth
key** below.

### Depth — a z-buffer for the meshes, the painter's key for anything translucent

Opaque geometry (terrain blocks, tanks, mites, nests) draws with a **depth buffer**:
give each projected vertex a depth `z = (wx + wy + wz)` mapped into NDC, enable
depth-test/-write, and let the GPU resolve occlusion — no per-instance CPU sort. (Add
a depth texture to the render pass; chapter 3 had none.) Anything **translucent or
additive** (the laser glow, the destruction burst) draws in a **second pass after**
the opaque pass with depth-test on but depth-write off, sorted back-to-front by the
**painter's key** `wx + wy + wz` so blending is ordered. Pin the depth key as a pure
function and test that it is monotonic along the view diagonal (a thing one cell
"north-west" never sorts in front of a thing one cell "south-east").

### Shading — flat low-poly faces, no asset required

The first pass must already look like a 3-D scene, from primitives alone. Get that for
free with **flat per-face shading**: one directional light direction (a constant), and
each face tinted by `dot(faceNormal, light)` clamped to a small ambient floor. For an
axis-aligned box that is three brightnesses — **top brightest, the two visible sides
darker** — which is exactly the cue that reads "isometric block." Compute it in the
shader from the face normal; no normals texture, no per-vertex lighting. Keep it
integer-friendly and cheap; this runs for every visible instance every frame.

### First pass — procedural primitives, zero external art

Ship a complete, legible isometric scene built **only** from a unit cube (and maybe a
unit quad), instanced, projected, depth-tested, flat-shaded. Map the sim's data to
placements:

- **Terrain.** For each **visible** cell, emit a block: an **open floor** is a thin
  slab (`wz ≈ 0`, low height); a **wall** cell is a **full-height cube** (`wz = 1`).
  Tint floors and walls distinctly; the face shading does the rest. (Only the visible
  screen's cells, plus a one-cell margin so tall walls at the far edge don't pop —
  render still scales with visibility, not with the 4800-cell world.)
- **Tanks (4).** A body box + a turret box + a thin barrel, placed at the tank's
  subcell position, the body oriented by `tank_ang` and the turret/barrel by
  `tank_turret` — the sim already stores both. Keep the per-tank identity colours.
- **Mites (1000).** A **tiny cube** (or a low pyramid) per **visible** mite, sitting on
  the floor, tinted by mode (wander / hunt / homing-by-nest) exactly as chapter 3 tints
  them. Instanced — a thousand mites is one instanced draw of one small mesh, the same
  "where there's one, there's many" batch as before.
- **Nests.** A short coloured **pylon/marker** per nest (the 15 nests, one per screen
  but the tank start), in the nest's colour.
- **Combat FX.** The **laser** is a thin elongated box from the muzzle to the wall-hit
  cell, drawn in the translucent pass; the **destruction burst** is an expanding cube
  (or billboarded quad) that fades over its lifetime — both read straight from the
  chapter-3 combat/FX state, no new sim data.

This first pass is **shippable on its own** — it is genuinely isometric 3-D, just
primitive — and it is the thing the asset pass later dresses.

### Second pass — real low-poly art, late-bound

Swap the primitive meshes for real models **without touching the projection, the
depth, the shading model, or the sim**. The only new machinery is: load a small set of
meshes/atlases once at startup (a vertex+index buffer per kind, or a texture atlas for
sprite tiles), and bind **kind → mesh** when emitting instances. The instance data
(placement, facing, tint) is unchanged; a glTF tank simply replaces the body-box, a
Kenney tile replaces the slab. Keep an `ASSETS.md` recording each asset's source, URL,
licence, and where it is used.

Two viable art directions — pick one and state why; both keep the renderer above
intact:

- **(A) Low-poly 3-D meshes** (recommended; matches "isometric 3D" literally). Parse a
  minimal **glTF**/`.obj` into a vertex+index buffer at load; instance it with the same
  placement/facing/tint. Tanks, nests, and even mites become little models; terrain
  blocks can stay procedural or become tile-blocks. Rotation is free (any facing).
- **(B) 2.5-D isometric sprites.** Billboard a textured quad per instance and sample a
  **pre-rendered isometric tile/sprite atlas**; tanks use an 8- or 16-direction sheet
  keyed by facing. Lighter, "classic iso" look; needs the painter's sort for the
  transparent sprites and loses free rotation.

### Sourcing the art — itch.io, prefer CC0

The user wants a short, **curated** list of assets they can actually get from
<https://itch.io/game-assets>, to drop into the second pass. Prefer **CC0** (public
domain, no attribution) so the swap is zero-friction; where a pack only allows "free +
credit," note it and keep the credit in `ASSETS.md`. Confirm the licence on each page
before using it — terms change.

**Recommended (verified at time of writing):**

| asset | creator | licence | format | use in ch.4 |
|------|---------|---------|--------|-------------|
| [Isometric Blocks](https://kenney-assets.itch.io/isometric-blocks) (130+ tiles) | Kenney | **CC0** | PNG tiles + sheets | terrain tiles (sprite path B) |
| [Isometric Prototypes Tiles](https://kenney-assets.itch.io/isometric-prototypes-tiles) (walls/floors/objects, 8-dir character) | Kenney | **CC0** | PNG | floors/walls + a stand-in unit |
| [Kenney 3-D kits](https://kenney.nl/assets) (Tower-Defense Kit, Tanks, City/Top-down kits) | Kenney | **CC0** | glTF/OBJ | tank + nest + prop meshes (mesh path A) |
| [FREE Stylized Tank](https://mreliptik.itch.io/free-lowpoly-tank-3d-model) (+ [4-tank pack](https://mreliptik.itch.io/4-low-poly-stylized-tanks)) | MrEliptik | free, **credit requested** | **glTF**/FBX | the four tank meshes (path A) |
| [Quaternius low-poly packs](https://quaternius.com/) (animals, nature, ultimate kits) | Quaternius | **CC0** | glTF/OBJ/FBX | mite "creature" meshes, nests, props |

**Browse for more (filtered):**
[isometric + tanks](https://itch.io/game-assets/tag-isometric/tag-tanks) ·
[CC0 + isometric](https://itch.io/game-assets/assets-cc0/tag-isometric) ·
[CC0 + free + low-poly](https://itch.io/game-assets/assets-cc0/free/tag-low-poly) ·
[free + low-poly + tanks](https://itch.io/game-assets/free/tag-low-poly/tag-tanks).

Practical guidance to put in the prompt's spirit: keep the imported set **small and
sized to the need** (one tank mesh re-tinted four ways beats four near-identical
downloads; one mite mesh; a handful of tile kinds). Convert/strip at **build time**
into the engine's own compact vertex buffers (host-baked, like the escape table) so the
runtime loads packed integer/float-lite data, not a glTF parser in the hot path — and
so the wasm/page stays allocation-light. Pre-quantise tile atlases to power-of-two.
Don't ship multi-megabyte source art in the repo; commit the **baked** buffers and
record provenance in `ASSETS.md`.

### Keep the minimap top-down — two projections of one model

Leave the **minimap top-down** (a literal map) while the main view is isometric. The
page then shows the **same sim data drawn two ways at once** — the chapter's lesson made
visible. Don't iso-skew the minimap; legibility-as-a-map is its job.

## Shared/structural guidance

- **Render placements come from the sim SoA, every frame, for visible things only.**
  `render.c` reads `tank_xy`/`tank_ang`/`tank_turret`, `mite_xy`/`mite_ang`/`mite_mode`/
  `mite_resp`, the wall `grid`, the nests, and the FX ring — exactly the chapter-3
  inputs — and emits the 3-D instance buffer. It gains **height + facing + kind** per
  instance; it gains **no new sim state**.
- **The instance layout grows; state it.** Chapter 3's instance was `{cx, cy, hx, hy,
  co, si, rgba}`. Chapter 4's is that plus a **z/height**, a **kind/mesh id**, and
  (mesh path) the facing already encoded in `co/si`. Keep it packed; report the new
  stride and the raised instance-buffer cap (visible cells × up-to-`MITE_CAP` mites +
  tanks + nests + FX, now possibly two layers of geometry per cell).
- **The depth buffer is new render state**, not sim state — a texture in the render
  pass.
- **Assets load once** (frequency of change: once) — the opposite end of the spectrum
  from the per-frame projection. Don't re-upload meshes per frame; upload at init,
  instance per frame.
- **No floating-point creep into the sim.** The projection/shading may use floats in the
  shader (the GPU is float-native, as in chapter 3's WGSL), but the **sim stays integer
  fixed-point** and untouched. The boundary is `render.c`/WGSL.

## Binding guidance — read first, follow strictly

These are requirements, not suggestions.

1. **`data-oriented-design.md`** — Mike Acton's rules (raw:
   `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`).
   Especially relevant here: solve the problem you have (draw the existing data, do not
   re-model the world in 3-D); the simplest machine that turns input into output (one
   cube, instanced and projected, before any asset); exploit the constraint (the world
   is a grid — terrain is one instanced block per cell, depth is `x+y+z`).
2. **`AGENTS.md`** at the repo root — every rule applies. Chapter 4 is the textbook case
   for **one-way dependency / sim never depends on render**, **presentation-only data
   stays out of `World`** (height, mesh id, light, camera are render-side), **match the
   work to the frequency of change** (assets load once, geometry rebuilds on wall edit,
   projection runs per frame), **render scales with what's visible**, and **host-bake
   pure data** (convert art to compact vertex buffers at build time, as chapter 1 bakes
   the escape table). If anything here is ambiguous, prefer the interpretation that keeps
   the sim frozen.
3. **Chapter 3's sources** — the render path you are replacing and the sim you must
   leave alone. Read `render.c`, `wasm.c`'s render exports, and `app.js`'s draw/upload
   path; identify the exact seam between "sim" and "render" and cut only on the render
   side.

## What to deliver

- **Chapter-4 render core** over the frozen chapter-3 sim: the isometric projection +
  depth key (pure functions in a shared header), the rebuilt `render.c` emitting 3-D
  instances for **visible** terrain/tanks/mites/nests/FX, the new WGSL (iso projection,
  ortho camera uniform, depth, flat face-shading, an opaque pass + a translucent FX
  pass), and the asset pipeline (placeholder primitives first; a host-bake step + a
  small loader for the real meshes/atlas second). One-way dependency preserved.
- **Native tests** (no browser/GPU):
  - **Sim is untouched (the thesis):** every chapter-3 sim/gossip/cap/combat/
    determinism check still passes **unchanged**, and the **state-hash after K ticks
    equals chapter 3's golden hash** for a fixed seed + scripted inputs.
  - **Projection:** `iso_project` maps known cells to known screen offsets; it is the
    pure dimetric transform; the **depth key is monotonic** along the view diagonal
    (occlusion order is correct); the camera uniform is the only thing that moves the
    picture (panning changes no placement).
  - **Visibility:** the instance count scales with the visible region, not with
    `N_MITES` / `N_WORLD_CELLS` (emit-for-visible holds).
- **The WebGPU page:** the world + tanks + thousand mites, drawn isometric-3-D in the
  main view, with the **top-down minimap** beside it (same data, two projections), the
  follow camera + picker working, and the chapter-3 live debug widgets retained
  (pool, occupancy, belief field, nests, distinct-destinations/`N_FIELDS`, seed +
  tunables). A small **render-only control** is welcome (e.g. camera zoom, or a
  placeholder↔assets toggle) — clearly labelled presentation-only.
- `build.sh`/`test.sh` (extended for the host-bake + projection tests), a visible
  cache-busted version, `ASSETS.md` (provenance + licences), `README.md`, and a
  `CONTRACT.md` whose central promise is *the view is a projection: the sim hash is
  identical to chapter 3*, tested.

## Behaviour to pin (so it's reproducible)

- **The simulation is byte-identical to chapter 3** for the same seed + inputs (state
  hash equal); presentation never feeds back.
- **The projection is a pure function** (dimetric 2:1) living in the shader/header; the
  camera is a uniform; depth is resolved by a z-buffer (opaque) with the painter's key
  `x+y+z` for the translucent FX pass.
- **Render scales with what is visible** — instances are emitted for the visible
  screen's cells/units (+ a one-cell margin), not for the whole world or the whole pool.
- **Assets are late-bound data** — the first pass needs none; the second pass swaps
  meshes/atlas via a kind→mesh table and a host-baked buffer, touching neither the
  projection nor the sim.
- **Presentation-only data stays out of `World`** — height, mesh id, light, camera,
  depth texture are all render-side.

## Memory budget (state it; static, no dynamic allocation in the sim)

The **sim** memory is unchanged from chapter 3 (state it as inherited). New **render**
memory to size and report: the raised instance buffer (new stride × max visible
instances, now up to two geometry layers per visible cell + mites + tanks + nests +
FX), the **depth texture** (viewport-sized), and the **baked asset buffers** (vertex +
index per kind, or the tile atlas) — loaded once. Keep the baked art small and sized to
the need; confirm the wasm linear memory and the GPU buffers hold, and state the new
instance-buffer cap.

## Definition of done

- Chapter 4 builds freestanding to wasm and natively; `test.sh` passes, including the
  **unchanged** chapter-3 sim tests, the **sim-hash-equals-chapter-3** regression, and
  the new projection/visibility tests.
- The page shows the world, the tanks, and the thousand mites in **isometric 3-D**,
  depth-correct and flat-shaded, with the **top-down minimap** beside it — the same data
  drawn two ways — and the chapter-3 debug widgets live in context.
- The **first pass renders with zero external assets** (primitives only); the **second
  pass** swaps in CC0/low-poly art via the asset table, with provenance + licences in
  `ASSETS.md`, and **neither pass touches the sim**.
- You can state, for each render structure, what it costs, how often it is built vs read
  (assets once, geometry on wall edit, projection per frame), and why it is render-side —
  and the page makes the chapter's lesson legible: **the simulation does not know it is
  being drawn; the view is a projection you can replace wholesale.**

## Deferred (not built — no speculative generality)

Perspective camera / free 3-D orbit (keep the fixed iso angle; a zoom is fine);
animated/rigged models or skeletal animation (static low-poly meshes, oriented by the
sim's existing facing); shadows, ambient occlusion, normal/PBR materials (flat
face-shading only); height/elevation in the **simulation** (z is render-only — the world
stays a 2-D grid); terrain autotiling / corner-aware tilesets; a 3-D minimap (it stays
top-down); per-mite distinct models or LOD (one instanced mesh for the swarm). Each is
its own future step; note it here, not in the code.
