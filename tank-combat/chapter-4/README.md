# Tank Combat — Chapter 4: The View (top-down perspective)

Chapter 4 of *Data-Oriented Design, by picture* (the book index is one level up). It
takes [chapter 3](../chapter-3/)'s world — the 8×8 toroidal grid, the four self-routing
tanks, the 4096 gossiping mites, the combat — and **redraws it in top-down perspective 3-D**,
changing **nothing about the simulation**. The same seed and inputs produce the same
play, down to a byte: the chapter's whole point is that a clean **one-way
sim/render dependency** lets you replace the *view* wholesale while the *model* sits
still.

**The point of the project is to demonstrate a data-oriented approach.** Here the
lesson is the payoff chapters 1–3 earned: *the render is a projection; the simulation
does not know it is drawn.* The DOD rules this chapter exercises are **one-way
dependency**, **presentation-only data stays out of `World`** (height, mesh id, the
light, the camera, the depth texture are all render-side), **match the work to the
frequency of change** (assets load once, geometry rebuilds on a wall edit, the
projection runs every frame), and **render scales with what is visible, not with the
population**.

Chapter 4 is an *increment* on chapter 3: it reuses the whole sim core verbatim (the
fixed-point types, the bitset grid, the movement transforms, the baked trig/escape
tables, the two-level path tables, the swarm, the combat, the determinism), and
rewrites only the render half.

Built following Mike Acton's data-oriented design rules:
[**data-oriented-design.md**](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md).

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge). The main
view is **top-down perspective 3-D**; the minimap stays **flat top-down** — the same data drawn two
ways at once. The four tanks are still yours: **tap a tank** to cycle it *auto-path*
(tap a cell to route it there) → *manual* (`W`/`S`/`A`/`D` or the pad) → *unselected*.
**Render-only** controls wrap the canvas (clearly labelled presentation): a **low-poly
art** toggle (placeholder cubes ↔ baked meshes) and **zoom / pitch / fov** sliders below,
and a **toolbar** above with the screen badge, **per-tank follow-cam** buttons (each
selects that tank), and a **free**-view button; **F** frames the selected tank. Drag to
pan; **shift-, right-, or two-finger-drag** to **orbit and tilt**; pinch or scroll to
zoom — and while following, those adjust the follow rather than dropping it.

## The thesis, stated as a test

For a fixed seed + scripted inputs run for *K* ticks, the **full sim state-hash equals
chapter 3's** — golden value `0x5786043F`, captured from chapter-3's own sources and
pinned in `test.c`. Every chapter-3 sim test still passes unchanged. That is the
contract; see [`CONTRACT.md`](CONTRACT.md).

## What changed (the render half only)

### The projection — a top-down perspective camera, in the shader

The world is the same `BIG_W × BIG_H` grid of cells in subcells. Each instance now
carries a **3-D world placement** — a cell `(wx, wy)`, a render-only **height `wz`**,
a **facing** (the body/turret angle the sim already stores), and a **tint** — and the
**vertex shader** projects it with a host-built **model-view-projection matrix**:

```
clip = mvp * vec4(world, 1)     // mvp = perspective · lookAt(eye, target) · scale(1/SUB)
```

The camera looks down on the world from above and slightly to the **south** (a fixed
~30° tilt off straight-down), so you see the tops of things and a sliver of their near
faces, and the **perspective** gives real depth — near walls larger than far. `render.c`
emits placements, never baked screen positions — the MVP is the **view uniform**, so a
pan/zoom is just a different matrix, not a rebuild. Depth is the matrix's own
`clip.z/w`; the only render-side constant left in C is the translucent painter key
(`view.h`).

### Depth — a z-buffer for the meshes, a painter's key for the glow

Opaque geometry draws with a **depth buffer** (a new texture in the pass): the
**perspective matrix writes real depth** (`clip.z/w`), the GPU resolves occlusion, no
CPU sort. Translucent FX (bolt streaks, bursts) draw in a **second pass** — depth-test on,
write off, **painter-sorted back-to-front** by a small key: since the camera leans south
and looks down, "nearer" grows with `+y` (south) and `+z` (height) — `view.h`, tested.

### Shading — flat per-face, no asset required

One directional light; each face is `tint * (ambient + (1-ambient)·dot(n, light))`,
computed in the shader from the face normal. An axis-aligned block reads top-bright,
sides-darker — the depth cue, for free.

### Assets are late-bound data

The **first pass** is built entirely from a **unit cube**, instanced and shaped by each
instance's half-extents (a thin slab floor, a full cube wall, a body/turret/barrel, a
tiny mite cube, a tall nest pylon). The **second pass** binds a baked low-poly **mesh
per kind** through the `MESH_FOR_KIND` table — *the instance data is unchanged; only
the bound mesh differs*. Meshes are **host-baked once** (`tools/gen_meshes.c` →
`src/mesh_data.c`, committed, compiled in, uploaded once at startup). The page toggle
flips between the two passes live. See [`ASSETS.md`](ASSETS.md).

A third, render-only layer dresses the grass: **low-poly trees** (`M_TREE`), baked once by
`build_static_props` onto grid-cell **corners** whose four cells are all grass — uploaded once,
frustum-culled per screen, shadow-casting, in their own buffer. They are pure scenery the sim
never sees (no collision); the **thin trunk** on a cell boundary reads as decoration, not an
obstacle. Same bake-once / scales-with-visibility discipline as the town.

A **render-profile** panel (presentation-only) sits under the view: CPU section timing, per-pass
GPU time via `timestamp-query` where the platform exposes it, draw counts, live **layer toggles**
(town / trees / point lights), and a **run-diagnostics** sweep that A/B's each layer and the
internal render scale by real frame time and copies a text report. Alongside it are switchable
**shadow-quality** modes: the town shadow-map filter (2×2 PCF → rotated-Poisson PCF → revectorised
silhouette AA, after Macedo et al., CGF 2019) and a screen-space contact-shadow path that renders
to a buffer and separably bilateral-blurs it. The default is **rotated-Poisson PCF + blurred SSS**
(the smoothest, chosen on-device); the rest stay live behind the controls for A/B.

### The connected world & the instance layout (`render.h`)

The whole **8×8 world is one connected map**: placements are world positions (anchor =
the world origin), so all sixty-four screens sit in their natural layout. A **free camera**
— drag to pan, shift/right/two-finger-drag to orbit and tilt, pinch or scroll to zoom —
shows any part; the host hands the renderer the
**visible box** and it emits only the cells and units inside it, so render still **scales
with visibility**: the whole world when zoomed out, a handful of cells when zoomed in,
never the 19200-cell world or the 4096-strong pool wholesale.

Chapter 3's instance was a 16-byte 2-D quad `{cx,cy, hx,hy, co,si, rgba}`. Chapter 4's
is a 24-byte 3-D box `{wx,wy(int32), wz,hz, hx,hy, co,si, rgba}` — the third dimension
(**+wz +hz**) plus **int32 `wx,wy`** (the 8×8 arena reaches 40960 subcells, past int16),
reusing facing and tint. The **kind/mesh id is not stored per instance** (nothing in
the shader reads it); it lives in the **draw grouping** — instances are emitted
contiguously by kind, the host draws one range per kind.

### Interaction made legible — overlays that never touch the sim

Clicking a tank cycles it (auto-path → manual → unselected); clicking a cell sends the
auto-path tank there. Four **render-only** overlays make that obvious, all derived
from the sim (or, for the cursor, one host-set hover cell): a **hover** highlight on
the cell under the cursor; the selected tank's **mode** (a ring + a tall spike, green
for auto-path, yellow for manual); a **destination** beacon; and the **routed path**,
a tile on each cell the tank will cross, traced from the same path tables it follows.
A tap selects/sends, a drag pans, two fingers (or the wheel) zoom.

## Source layout

```
src/
  view.h         NEW  the translucent depth key (the projection is a host-side perspective MVP)
  render.h       the instance layout (24-byte 3-D box), the kinds, the DrawList
  render.c       reads the World, emits 3-D placements grouped by kind (no sim writes)
  mesh_data.h    NEW  the baked-mesh table interface (M_CUBE, M_PYLON, ...)
  mesh_data.c    GENERATED  the baked low-poly vertex buffers (tools/gen_meshes.c)
  wasm.c         + render exports (DrawList + meshes); sim exports unchanged
  test.c         + the chapter-4 render tests; the chapter-3 sim tests are unchanged
  sim.c sim.h mites.c tanks_fire.c tanks_path.c agent_*.c collide.c grid_paths.c
  edge_paths.c escape_table.c map_data.c dirtab.c defs.h ...   FROZEN (== chapter 3)
tools/
  gen_meshes.c   NEW  host-bake the low-poly meshes -> src/mesh_data.c
  gen_escape.c gen_map.c ...   unchanged
app.js           the perspective WebGPU renderer (2 pipelines, depth, mesh bind, MVP camera)
```

## Memory budget (static, no dynamic allocation)

The **sim** memory is inherited from chapter 3 unchanged. New **render** memory:

- **instance buffer** — 24-byte 3-D `Inst` × `INST_MAX` (~24000) ≈ **576 KB** (worst
  case: the whole 8×8 world in view — every cell, the whole 4096-strong swarm, tanks,
  FX, and the overlays; the free camera culls to far fewer when zoomed in).
- **baked meshes** — `src/mesh_data.c`, ~216 vertices × 12 bytes ≈ **2.6 KB**, loaded
  once (now including the tree).
- **static props (trees)** — a second 24-byte `Inst` buffer sized to the cell count
  (`STATIC_PROP_INST_MAX`), ~**460 KB** worst case; the frozen map fills a few hundred.
- the **depth buffer** is a viewport-sized GPU texture (`depth24plus`), **not** wasm
  linear memory.

The 8×8 world (4096 mites, 19200 cells) inherits chapter 3's larger footprint, so the
linear memory is **8 MiB** (128 pages) — chapter 3's budget plus the instance buffer.
Still all static, integer, allocation-free.

## Build

`./build.sh` runs `./gen.sh` (bakes the escape table, the world map, **and the
meshes**), then compiles the freestanding wasm (`-nostdlib -ffreestanding`, no libc/
libm, no allocation) and stamps a cache-busted version. `game.wasm` is committed so
GitHub Pages serves it with no build step.

## Testing

`./test.sh` builds natively (no wasm, no GPU) and runs **174 checks**: every chapter-3
sim test unchanged, then the chapter-4 render tests — the
sim-hash-equals-chapter-3 thesis, the translucent depth key, the host-uniform camera, the
linear (uniform) camera, the emit-for-visible bound, that every emitted placement keeps
its true (un-truncated) world coordinate, that every nest gets a colour, that the
static **trees** sit only on grass corners (on open, non-nest ground), partition into
per-screen `M_TREE` runs, and re-bake deterministically, and that the **roads are one
connected network** (no stranded traffic-island rings). `render.c` / `staticmap.c` are
plain C over the `World`, so they are exercised on the host with no browser.

## Verified

- Chapter 4 builds freestanding to wasm and natively; `test.sh` passes all 174 checks.
- The sim state-hash after the scripted run equals chapter 3's golden `0x5786043F`.
- The first pass renders from primitives only (the unit cube); the second pass swaps in
  baked low-poly meshes via the kind→mesh table; neither pass touches the sim.
- The page shows the world, the tanks, and the 4096 mites in top-down perspective 3-D,
  depth-correct and flat-shaded, with the top-down minimap beside it and the chapter-3
  debug widgets live in context.

## Deferred (not built — no speculative generality)

Camera roll or a first-person fly-through (the camera stays an up-right orbit — yaw,
pitch/tilt, fov, pan, and zoom are all provided); animated or rigged meshes / skeletal
animation (static low-poly, oriented by the sim's
existing facing); ambient occlusion, normal/PBR materials (flat face-shading
only — sun shadows ARE built: a baked light-space map for the static town plus a
screen-space march for the actors); height/elevation in the **simulation** (`z` is render-only — the world stays a
2-D grid); terrain autotiling / corner-aware tilesets; a 3-D minimap (it stays
top-down); per-mite distinct models or LOD (one instanced mesh for the swarm). Each is
its own future step.
