# Tank Combat — Chapter 4: The View (isometric 3D)

Chapter 4 of *Data-Oriented Design, by picture* (the book index is one level up). It
takes [chapter 3](../chapter-3/)'s world — the 4×4 toroidal grid, the four self-routing
tanks, the thousand gossiping mites, the combat — and **redraws it isometric 3-D**,
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
view is **isometric 3-D**; the minimap stays **top-down** — the same data drawn two
ways at once. The four tanks are still yours: **tap a tank** to cycle it *auto-path*
(tap a cell to route it there) → *manual* (`W`/`S`/`A`/`D` or the pad) → *unselected*.
Two **render-only** controls sit under the canvas (clearly labelled presentation): a
**low-poly art** toggle (placeholder cubes ↔ baked meshes) and a **zoom**.

## The thesis, stated as a test

For a fixed seed + scripted inputs run for *K* ticks, the **full sim state-hash equals
chapter 3's** — golden value `0xFB9DAC47`, captured from chapter-3's own sources and
pinned in `test.c`. Every chapter-3 sim test still passes unchanged. That is the
contract; see [`CONTRACT.md`](CONTRACT.md).

## What changed (the render half only)

### The projection — dimetric 2:1, in the shader (`iso.h`)

The world is the same `BIG_W × BIG_H` grid of cells in subcells. Each instance now
carries a **3-D world placement** — a cell `(wx, wy)`, a render-only **height `wz`**,
a **facing** (the body/turret angle the sim already stores), and a **tint** — and the
**vertex shader** projects it:

```
screenX = (wx - wy) * ISO_X
screenY = (wx + wy) * ISO_Y - wz * ISO_Z      // ISO_X : ISO_Y = 2 : 1
```

The classic **dimetric 2:1** ratio, **orthographic** (no perspective). `render.c`
emits placements, never baked screen positions — the iso basis, zoom, and follow
offset live in the **view uniform**, so the camera is a uniform, not a rebuild. The
constants are exported from wasm so the shader reads one source of truth; the native
test exercises the C `iso_project`/`iso_depth` directly.

### Depth — a z-buffer for the meshes, a painter's key for the glow

Opaque geometry draws with a **depth buffer** (a new texture in the pass): depth is
`wx + wy + wz`, the GPU resolves occlusion, no CPU sort. The key is **monotone along
the view diagonal** (tested). Translucent FX (laser glow, bursts) draw in a **second
pass** — depth-test on, write off, **painter-sorted back-to-front** by `wx + wy + wz`.

### Shading — flat per-face, no asset required

One directional light; each face is `tint * (ambient + (1-ambient)·dot(n, light))`,
computed in the shader from the face normal. An axis-aligned block reads top-bright,
sides-darker — the iso cue, for free.

### Assets are late-bound data

The **first pass** is built entirely from a **unit cube**, instanced and shaped by each
instance's half-extents (a thin slab floor, a full cube wall, a body/turret/barrel, a
tiny mite cube, a tall nest pylon). The **second pass** binds a baked low-poly **mesh
per kind** through the `MESH_FOR_KIND` table — *the instance data is unchanged; only
the bound mesh differs*. Meshes are **host-baked once** (`tools/gen_meshes.c` →
`src/mesh_data.c`, committed, compiled in, uploaded once at startup). The page toggle
flips between the two passes live. See [`ASSETS.md`](ASSETS.md).

### The connected world & the instance layout (`render.h`)

The whole **4×4 world is one connected map**: placements are world positions (anchor =
the world origin), so all sixteen screens sit in their natural layout. A **free camera**
— drag to pan, pinch or scroll to zoom — shows any part; the host hands the renderer the
**visible box** and it emits only the cells and units inside it, so render still **scales
with visibility**: the whole world when zoomed out, a handful of cells when zoomed in,
never the 4800-cell world or the 1000-strong pool wholesale.

Chapter 3's instance was a 16-byte 2-D quad `{cx,cy, hx,hy, co,si, rgba}`. Chapter 4's
is a 20-byte 3-D box `{wx,wy, wz,hz, hx,hy, co,si, rgba}` — exactly **+wz +hz**,
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
  iso.h          NEW  the dimetric projection + depth key (pure, shared with the test)
  render.h       the instance layout (20-byte 3-D box), the kinds, the DrawList
  render.c       reads the World, emits 3-D placements grouped by kind (no sim writes)
  mesh_data.h    NEW  the baked-mesh table interface (M_CUBE, M_PYLON, ...)
  mesh_data.c    GENERATED  the baked low-poly vertex buffers (tools/gen_meshes.c)
  wasm.c         + render exports (DrawList, iso basis, meshes); sim exports unchanged
  test.c         + the chapter-4 render tests; the chapter-3 sim tests are unchanged
  sim.c sim.h mites.c tanks_fire.c tanks_path.c agent_*.c collide.c grid_paths.c
  edge_paths.c escape_table.c map_data.c dirtab.c defs.h ...   FROZEN (== chapter 3)
tools/
  gen_meshes.c   NEW  host-bake the low-poly meshes -> src/mesh_data.c
  gen_escape.c gen_map.c ...   unchanged
app.js           the isometric WebGPU renderer (2 pipelines, depth, mesh bind, camera)
```

## Memory budget (static, no dynamic allocation)

The **sim** memory is inherited from chapter 3 unchanged. New **render** memory:

- **instance buffer** — 20-byte 3-D `Inst` × `INST_MAX` (~6500) ≈ **130 KB** (worst
  case: the whole world in view — every cell, the whole swarm, tanks, FX, and the
  overlays; the free camera culls to far fewer when zoomed in).
- **baked meshes** — `src/mesh_data.c`, 162 vertices × 8 bytes ≈ **1.3 KB**, loaded
  once.
- the **depth buffer** is a viewport-sized GPU texture (`depth24plus`), **not** wasm
  linear memory.

The wasm high-water mark is unchanged from chapter 3 (~1.04 MiB), so the linear memory
stays **2 MiB** (32 pages). Still all static, integer, allocation-free.

## Build

`./build.sh` runs `./gen.sh` (bakes the escape table, the world map, **and the
meshes**), then compiles the freestanding wasm (`-nostdlib -ffreestanding`, no libc/
libm, no allocation) and stamps a cache-busted version. `game.wasm` is committed so
GitHub Pages serves it with no build step.

## Testing

`./test.sh` builds natively (no wasm, no GPU) and runs **134 checks**: every chapter-3
sim test unchanged (115), then the chapter-4 render tests (19) — the
sim-hash-equals-chapter-3 thesis, the dimetric projection, the monotone depth key, the
linear (uniform) camera, and the emit-for-visible bound. `render.c` is plain C over the
`World`, so it is exercised on the host with no browser.

## Verified

- Chapter 4 builds freestanding to wasm and natively; `test.sh` passes all 134 checks.
- The sim state-hash after the scripted run equals chapter 3's golden `0xFB9DAC47`.
- The first pass renders from primitives only (the unit cube); the second pass swaps in
  baked low-poly meshes via the kind→mesh table; neither pass touches the sim.
- The page shows the world, the tanks, and the thousand mites in isometric 3-D,
  depth-correct and flat-shaded, with the top-down minimap beside it and the chapter-3
  debug widgets live in context.

## Deferred (not built — no speculative generality)

Perspective camera / free 3-D orbit (the iso angle is fixed; a zoom is provided);
animated or rigged meshes / skeletal animation (static low-poly, oriented by the sim's
existing facing); shadows, ambient occlusion, normal/PBR materials (flat face-shading
only); height/elevation in the **simulation** (`z` is render-only — the world stays a
2-D grid); terrain autotiling / corner-aware tilesets; a 3-D minimap (it stays
top-down); per-mite distinct models or LOD (one instanced mesh for the swarm). Each is
its own future step.
