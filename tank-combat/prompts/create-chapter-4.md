# Prompt — Create Chapter 4: The View (top-down perspective 3D)

You are building **chapter 4** of the interactive book that teaches *data-oriented
design* by growing one small game, one feature at a time. Chapter 1 is a two-tank
Atari-*Combat* game; chapter 2 grew the world to an **8×8 toroidal grid of screens**
(`160×120 = 19,200` cells) with self-pathing tanks; chapter 3 added an enemy faction —
a thousand gossiping **mites** that swarm the tanks, with combat. Chapter 4 changes
**nothing about the simulation** and **everything about the picture**: the same world,
the same tanks, the same swarm, redrawn as a **top-down perspective 3-D town** — a
deferred-lit, shadowed, LOD'd city baked from the frozen map.

Each chapter is a self-contained web page: the running game on top, a written
walk-through below, and the game's live data shown and editable *in the context of the
text*.

> **This prompt describes the chapter as it was actually built** — a recreate-the-work
> spec, not the original sketch. The view is **top-down perspective** (an orbiting,
> tilting camera), not fixed isometric; lighting is a **deferred** pass with point
> lights and shadows, not flat face-shading; the world is a **static baked town** from
> Kenney CC0 kits, not per-cell primitive blocks. Build it incrementally (a legible
> primitive pass first), but the target is the full town below.

## The lesson — the render is a projection; the simulation does not know it is drawn

Chapters 1–3 earned the payoff this chapter spends: a strict **sim / render / wasm split
with a one-way dependency** (render reads the sim; the sim never reads the render).
Chapter 4 is the proof. We replace the top-down 2-D view with a full **top-down
perspective 3-D** view — new projection, depth, deferred shading, lights, shadows, a
camera, LOD, and art — and we do it **without editing a single line of the
simulation**. `sim.c`, `mites.c`, `tanks_path.c`, `agent_turn`/`agent_move`, the
per-cell index, the gossip, the route fields, the RNG — all **byte-for-byte identical
to chapter 3** (the only diverged "sim" file is `tanks_fire.c`, and only because the
laser became a travelling **bolt** — a model change carried over deliberately, with its
own golden hash). The things that change are the **render half** (`render.c`, the WGSL,
the view uniform, `app.js`, the static-map bake) and a folder of **baked art**.

That is the whole point, stated as a falsifiable claim and pinned by a test: *for the
same seed and the same inputs, chapter 4 produces the identical sim state-hash as
chapter 3 (modulo the bolt change, which has its own golden hash), tick for tick.* If
the hash diverges unexpectedly, presentation leaked into the model and the chapter has
failed its thesis. The DOD rules this chapter exercises: **one-way dependency**,
**presentation-only data stays out of `World`**, **match the work to the frequency of
change** (the town is baked once and re-baked only on a wall/nest edit — rare; the
projection runs every frame — constant; the art loads once), and **render scales with
what is visible, not with the population**.

A second lesson rides along: the world's geometry is **a projection of the map, too**.
The map is *static and totally known* (which cells are walls, where the nests sit), so
the **town it becomes is known too** — and it is baked **once** into a GPU instance
buffer, never re-derived per frame. The sim never learns it became a town; the town is
a *projection* of the sim's walls and nests, the same bake-it-once discipline as the
escape table.

## Starting point — copy chapter 3 verbatim, then freeze the sim

1. Copy `tank-combat/chapter-3/` to `tank-combat/chapter-4/` verbatim and get it
   building/running first (`build.sh`, `test.sh`) before changing anything. Chapter 4 is
   an *increment*, not a rewrite.
2. **Freeze the simulation.** Everything under `src/` that is not `render.{c,h}`,
   `staticmap.{c,h}`, `wasm.c`'s render exports, the mesh/map data, plus `app.js`'s draw
   path, is **closed for edits**. The chapter-3 native sim/gossip/cap/combat/determinism
   tests must keep passing **unchanged**. Add a regression that the **state-hash after K
   ticks equals chapter 3's golden value** for a fixed seed + scripted inputs. (When you
   later change the laser to a bolt, that touches `tanks_fire.c` and re-stamps a new
   golden hash — the *only* sanctioned sim divergence; everything else stays equal.)
3. **Rewrite the page's article** for the new concepts (the projection, depth, the
   deferred renderer, lights, shadows, the static town, LOD, the asset pipeline), keeping
   the page *structure*: the canvas, the camera toolbar (follow buttons + free + screen
   picker), the **top-down minimap** (keep it top-down), the live debug widgets, the
   version badge, the styling.
4. Add/keep the chapter-4 entry in the book TOC (`tank-combat/index.html`) and give
   chapter 4 its own `README.md`, `CONTRACT.md`, `ASSETS.md`, and `screen-capture.md`
   (how to render/capture the WebGPU page headlessly — see that file; do not relearn it).

## What chapter 4 adds — a top-down perspective 3-D renderer over the same data

Everything below lives in the **render half**. None of it feeds back into the sim.

### The projection — a real perspective MVP camera, in a host uniform

`render.c` emits **world placements**, never screen positions: per opaque instance a
cell `(wx, wy)` in subcells, a render-only **height `wz` + half-height `hz`** (the sim
has no z), **half-extents `(hx, hy)`**, a **facing** `(cos, sin)` (reuse the sim's
`tank_ang`/`tank_turret`), and a packed **rgba tint**. The shader applies a **model-
view-projection** matrix built host-side each frame:

- A `lookAt` eye orbiting a ground target by **yaw** (orbit) and **pitch** (tilt off
  straight-down), a **perspective** projection (live **fov**), and a **zoom** (eye
  distance). All live, all **presentation-only**, all in the **view uniform** — the only
  thing that moves the picture; panning/following/zooming change no placement.
- **Mirror clip-x** (a `FLIPX` factor in the MVP) so east is on the right, matching the
  flat top-down minimap. Because the geometry pass culls nothing (`cullMode: "none"`),
  the flipped winding is harmless; keep the **inverse MVP** so pick/pan/hover unproject
  consistently (cast a screen ray to the ground plane `z=0`).
- Clamp pitch so the **top frustum edge stays under the horizon** (`(90°−pitch) −
  fov/2 > 0`), and reach the far plane to the farthest visible ground.

This is the whole camera. Provide live **pitch / fov / zoom** controls (and a **lod
dist** slider, below), clearly labelled presentation-only.

### Depth — a real z-buffer for opaque; the painter's key only for translucent

Opaque geometry draws with a **depth buffer** (a `depth24plus` texture; the GPU resolves
occlusion from `clip.z/clip.w`, no CPU sort). Translucent/additive FX (the **bolt** glow
+ head, the destruction bursts) draw in a **later pass** with depth-test on but depth-
write off, painter-sorted back-to-front by `wx + wy + wz` so blending is ordered. Keep
the depth key a pure function and test it is monotonic along the view diagonal.

### The deferred renderer — lighting is a screen pass

Don't shade in the geometry pass; **defer it**. Render the opaque scene once into a
**G-buffer** (MRT): `gColor` (`rgba8`, `.a` = a 1/0 surface-vs-sky mask), `gNormal`
(`rgba8`, world normal `*0.5+0.5`), `gPosition` (`rgba32float`, world position in
subcells). Then a **full-screen lighting pass** reads the G-buffer and writes an **HDR**
target (`rgba16float`): a **hemisphere ambient** (sky overhead lerped to a dim ground
bounce by `normal.z`) plus one **directional sun**. A final **tonemap** pass rolls the
HDR down to the swapchain (an exposure roll-off, e.g. `1 − exp(−c·hdr)`). Background
(mask 0) is the sky.

This is the point of deferring: lighting cost scales with **lit pixels, not objects ×
lights**, which is what makes the next two features affordable.

> **Float-texture traps (you will hit these):** `rgba16float`/`rgba32float` are **not
> filterable** — declare explicit bind-group layouts with `sampleType:
> "unfilterable-float"` and read them with `textureLoad` (a `layout:"auto"` infers a
> filtering sampler and you get `[Invalid CommandBuffer]`). `texture COPY_DST = 0x02`
> (not `0x08`, which is `STORAGE_BINDING`). `textureSample` under a branch fails — use
> `textureSampleLevel`. See `screen-capture.md`.

### Point lights — one per visible mite and per FX, as additive volumes

Give the swarm and the combat real light. In C, `build_lights` walks the **same sim
state** (alive mites in view, active bolts/bursts) and packs a small light buffer (world
position, radius, colour, intensity in the alpha). On the GPU each light is a **cube
VOLUME** (the unit cube, instanced from the light buffer), **additively blended** into
the HDR, **front-faces only, no depth test** — the volume just marks the pixels it
covers, and the fragment reads the G-buffer there, falls off with distance, and adds its
colour. (Because the camera's `FLIPX` inverts winding, the volume pass uses
`cullMode: "front"`.) A light pays only for the pixels it covers, so a thousand of them
stay cheap. Pin in native tests: a light per alive mite, all positive-radius volumes at
valid positions, culled to the visible box, `build_lights` a pure function.

### Shadows — a baked sun map for the static town + a screen-space march for the actors

Split the sun's shadow by **frequency of change**:

- **The static town is baked, so its shadow is too.** Render the town's depth from the
  sun through one **orthographic** pass into a depth map at init, re-baked **only** when
  the town re-bakes (the same trigger as the instance buffer). Fit the ortho frustum to
  the world AABB. In the lighting pass, project each pixel into the sun's light clip and
  **PCF-compare** against the stored depth — long, correct building shadows. **Cull
  FRONT faces** in the bake (store the occluders' *far* side — second-depth shadow
  mapping) so a sun-lit roof never self-shadows: kills acne with no per-surface bias and
  no LOD-match needed.
- **The moving actors aren't in the bake**, so they get the complementary technique:
  **Bend Studio's screen-space shadow march** (from *Days Gone*). For each lit pixel,
  step a ray toward the sun, project each step back to screen, and if an on-screen
  surface sits in front of the ray within a thickness window, the sun is blocked. Its
  honest limit is one depth layer (it can't see an occluder's far side), so keep it
  **medium range** — it *grounds* what moves; a long march just smears a tall occluder.
  Many small steps, **no dither** (an undenoised jitter reads as grain).

The two visibilities multiply. Light it for **evening**: a low, raking, warm sun over a
dim dusk ambient, so the shadows carry the contrast.

### The world is a static town — bake the frozen map once

The map is static and totally known, so the **town it becomes is baked once**, in C
(`staticmap.c`, `build_static_map`), never re-derived per frame. Classify every world
cell:

- **wall** → a **building** (hash-picked variant + a 90° rotation, for variety)
- **open cell touching a wall** (8-neighbourhood) → an **autotiled road** (streets hug
  the building blocks). The road's rotation comes from matching the cell's neighbour mask
  against each road mesh's **canonical connection mask measured from the art at bake
  time** (`MAP_ROAD_CANON` — hand-guessing it is exactly how the streets first came out
  180° wrong).
- **wide-open interior** → **grass**
- **nest** → a **skyscraper landmark**, tinted in that nest's hue (keeps nest identity)

Pack **one instance per cell**, grouped by `(screen, mesh)` into a small run table.
Town meshes carry their own palette colour, so the instance tint is white (the shader
does `meshColour × tint`) — except the landmark. The host uploads the instance buffer
**once** and, per frame, only **frustum-culls the screen buckets** and draws the visible
runs — no re-emit, no re-classification. It re-bakes (in wasm) **only** when the map
changes (a wall toggled, a nest moved); a **version counter** tells the page to
re-upload. (The one per-frame exception is cosmetic — see *wall-shake* below.)

### The art — Kenney CC0 kits, baked offline; procedural meshes for the actors

Two mesh tables share one GPU vertex buffer, split by *frequency of change*:

- **Procedural meshes** (`tools/gen_meshes.c` → committed `src/mesh_data.c`) for the
  **dynamic** things: `M_CUBE` (overlays, barrels, beacons, bolt FX, placeholder),
  `M_PYRAMID` (a mite spike), `M_HULL` (tank body), `M_TURRET` (tank turret), `M_PYLON`
  (spare). `MESH_FOR_KIND[]` binds each opaque kind to its mesh; tanks are hull + turret
  + a barrel cube, oriented by the sim's angles. Original, CC0, no parser ships.
- **Town meshes** baked from **Kenney CC0** kits by an **offline** builder
  (`tools/bake_map_assets.mjs`, run by hand against a local download — **source art is
  not committed**) → committed `src/map_mesh_data.{c,h}` + `town_atlas.png`. Kits used:

  | group | kit (Kenney, **CC0**) | meshes |
  |---|---|---|
  | `ROAD` | [City Kit (Roads)](https://kenney.nl/assets/city-kit-roads) | `road-square`, `-end`, `-straight`, `-bend`, `-intersection`, `-crossroad` (autotiled) |
  | `BUILD` | [City Kit (Suburban)](https://kenney.nl/assets/city-kit-suburban) + [(Commercial)](https://kenney.nl/assets/city-kit-commercial) | `building-type-{a,c,e,h,k,q}` + commercial `building-{a,c}` (8 variants; roofs recoloured to contrast the grass) |
  | `GRASS` | [Tower Defense Kit](https://kenney.nl/assets/tower-defense-kit) | `tile` (the ground) |
  | `LANDMARK` | City Kit (Commercial) | `building-skyscraper-a` (each nest) |

  The baker decodes each kit's indexed-PNG palette, parses the OBJ, rotates Kenney's
  Y-up to the engine's Z-up, **normalises each mesh per-axis into the unit box `[-1,1]`**
  (footprint `x/y`, base at `z=-1`) and records a per-mesh height half-extent (`MAP_MESH_HZ`,
  subcells) so one instance restores real proportions. It also CPU-rasterises the LOD
  imposter atlas (below). Nothing runs at game time; the committed tables + atlas are the
  deliverable (CC0, derived from CC0). Record all provenance in `ASSETS.md`.

### LOD — three levels per town mesh, including a projected imposter

A full Kenney building is ~3,500 verts; a hundred cells away it is a smear. Bake **three
levels**, picked **per screen by distance**:

- **LOD0** — the full art (~3,500 verts), near screens.
- **LOD1 — a textured *imposter*** (~40-vert cage + a texture): the cheap box-and-roof
  massing, but each face samples `town_atlas.png` — the LOD0 model rendered
  orthographically (offline, by a tiny **CPU rasteriser** in the baker) from the **top +
  4 sides** and packed into one atlas. The cage's UVs index it, so a mid-distance block
  keeps its windows/doors for forty vertices. (This is the *render-to-texture lowest-LOD
  imposter* workflow, done in JS.)
- **LOD2** — the same cage in a flat, colour-matched **massing** (no texture), far
  screens.

Classify the cage's roof by the **area-weighted slope** of the upper roof faces (flat vs
gable + ridge axis) — *not* by the span of the top vertices, which rooftop clutter (AC
units, parapets) fakes into false gables. A flat roof gets full-height walls + a thin top
cap. The 12-byte table holds LOD0 in `[0,16)` and LOD2 at `MAP_LOD2_OFFSET`; the LOD1
imposters are a **separate 16-byte table** (they carry a `i16×2` UV) indexed by base mesh.
The static map emits LOD0 ids only; the host swaps to the imposter/massing by distance.
Selection: derive the LOD0→LOD1 crossover from a reference framing (max zoom, 56° pitch),
scale to LOD1→LOD2; expose a live **lod dist** slider. LOD0/LOD2 share the flat geometry
pipeline; the imposter draws in its own textured pass (sample the atlas, branch on the UV
sentinel) writing the **same G-buffer**, so the deferred lighting/shadows treat it as art.

### Combat FX — a travelling bolt (the one sanctioned sim change)

Chapter 3's instant laser becomes a **3-D projectile bolt** with a tunable speed
(`proj_speed`). This is the one place `tanks_fire.c` diverges from chapter 3; update the
combat test's fields and **re-stamp the golden hash** to the new value. The bolt streak +
head and the destruction bursts draw as `M_CUBE` in the translucent pass.

### Wall-shake — jolt the struck building (don't lose it to the static town)

Chapter 3 buzzed a wall struck by a shot (`wall_shake`: amplitude decays over the
shake's life, flipping each tick). The sim still computes it — but the walls are now the
**upload-once** town, so restore the effect host-side: export `wall_shake_cell/_t`; build
a **cell → static-instance index** when the town bakes; per frame read `wall_shake` and
patch just the struck buildings' `wx/wy` with the same buzz, snapping any that stopped
back to centre. At most `WALL_SHAKE_MAX` instances while a shot is fresh — a handful of
bytes, not a re-bake. (Note this small per-frame exception in `ASSETS.md`.)

### Keep the minimap top-down — two projections of one model

Leave the **minimap top-down** (a literal map) while the main view is perspective 3-D.
The page then shows the **same sim data drawn two ways at once** — the chapter's lesson
made visible. Don't skew the minimap.

## Build order (incremental — each step ships)

1. Copy ch3, freeze the sim, add the golden-hash regression.
2. **Primitive 3-D first:** `render.c` emits world placements; the shader projects them
   (perspective MVP in a uniform); a depth buffer; everything a flat-shaded `M_CUBE`,
   shaped per instance. A legible 3-D scene from one cube — shippable.
3. Procedural meshes (`gen_meshes.c`): tank hull/turret, mite pyramid; bind `MESH_FOR_KIND`.
4. Perspective camera controls (orbit/tilt/fov/zoom/pan) + world-anchored frustum cull
   over all screens; unproject for pick/hover.
5. The **static town** (`staticmap.c`) + the Kenney bake + upload-once/cull. Asset toggle.
6. The **deferred** renderer (G-buffer → lighting → tonemap), then **point lights**, then
   **shadows** (SSS, then the baked sun map), then evening light.
7. **LOD** (massing, then the textured imposter); the bolt; the wall-shake.

Each step keeps the sim frozen and the golden hash equal.

## Binding guidance — read first, follow strictly

1. **`data-oriented-design.md`** — Mike Acton's rules (raw:
   `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`).
   Solve the problem you have (draw the existing data; bake the known town once); the
   simplest machine first (one cube, instanced + projected, before any asset); exploit
   the constraint (the map is static → the town is baked data; lighting is a screen pass
   → cost scales with lit pixels).
2. **`AGENTS.md`** at the repo root — every rule applies. Chapter 4 is the textbook case
   for **one-way dependency**, **presentation-only data stays out of `World`** (height,
   mesh id, lights, camera, shadows, LOD, the town instances are all render-side),
   **match the work to the frequency of change** (art once; town on a wall edit;
   projection per frame), **render scales with what's visible**, and **host-bake pure
   data** (the town meshes + atlas + imposter, baked offline like the escape table).
3. **Chapter 3's sources** — the render path you replace and the sim you must leave
   alone. Cut only on the render side.

## What to deliver

- **Chapter-4 render core** over the frozen ch3 sim: the perspective MVP camera (a host
  uniform) + the depth key (a pure function), `render.c` emitting 3-D instances for
  **visible** dynamic things, `staticmap.c` baking the **static town**, the **deferred**
  WGSL (G-buffer, screen-space lighting, HDR/tonemap, additive light volumes, the SSS
  march, the baked-sun-map sample, the textured imposter pass), and the asset pipeline
  (procedural `gen_meshes.c` + the offline `bake_map_assets.mjs` → committed tables +
  `town_atlas.png`). One-way dependency preserved.
- **Native tests** (no browser/GPU): the ch3 sim suite passes **unchanged**; the
  **state-hash equals chapter 3's golden** (the bolt re-stamps its own); the projection
  is a pure transform and the **depth key is monotonic**; **visibility** holds (instance
  count scales with the visible region, not `N_MITES`/`N_WORLD_CELLS`); the **static
  bake** is a pure function (same map → byte-identical town, one instance per cell, valid
  runs, nests→landmarks); **`build_lights`** is pure (a light per alive mite, culled to
  the box).
- **The WebGPU page:** the world + tanks + swarm drawn **top-down perspective 3-D** as a
  deferred-lit, shadowed, LOD'd town, the **top-down minimap** beside it, the follow
  camera + picker working, live pitch/fov/zoom + **lod dist** + low-poly-art toggle
  (presentation-only), and the ch3 debug widgets in context.
- `build.sh`/`gen.sh`/`test.sh`, a visible cache-busted version, `ASSETS.md` (provenance
  + licences + the kits + the imposter atlas), `README.md`, `CONTRACT.md` (central
  promise: *the view is a projection — the sim hash equals chapter 3*, tested), and
  `screen-capture.md` (headless WebGPU render/capture).

## Behaviour to pin (so it's reproducible)

- **Sim byte-identical to chapter 3** (state hash equal; the bolt is the lone, golden-
  re-stamped exception). Presentation never feeds back.
- **The camera is a pure-function MVP in a uniform**; depth is a z-buffer (opaque) +
  painter's key `x+y+z` (translucent FX).
- **Lighting is a deferred screen pass** — cost scales with lit pixels; point lights are
  additive volumes; sun shadows are a **baked ortho map (static town) + a screen-space
  march (actors)**.
- **The town is a once-baked projection of the frozen map** — uploaded once, frustum-
  culled per frame, re-baked only on a wall/nest edit.
- **Three-level LOD** picked per screen by distance; the imposter is LOD0 projected onto
  a cheap cage via `town_atlas.png`.
- **Presentation-only data stays out of `World`** — height, mesh id, lights, camera,
  shadows, LOD, the town instances, the wall-shake patch are all render-side.

## Memory budget (state it; static, no dynamic allocation in the sim)

The **sim** memory is unchanged from chapter 3 (inherited). New **render** memory: the
dynamic instance buffer (24-byte `Inst` × the in-view swarm/tanks/FX/overlays), the
**static town** buffer (24-byte `Inst` × `N_WORLD_CELLS = 19,200`, one per cell, built
once), the run table, the procedural + **town mesh** data segments (the latter ~400 KB
of packed `int8`), the 16-byte **imposter** table, the **G-buffer + HDR + depth**
textures (viewport-sized), the **baked sun shadow map** (one ortho depth texture), and
`town_atlas.png`. All static, integer-positioned, allocation-free; the per-frame map
cost is a cull (plus the tiny wall-shake patch).

## Deferred (not built — no speculative generality)

No camera roll or first-person fly-through (the camera stays an up-right orbit — yaw,
pitch/tilt, fov, pan, zoom are all provided); animated/rigged meshes or skeletal
animation (static low-poly, oriented by the sim's existing facing); ambient occlusion,
normal/PBR materials (flat baked albedo + the deferred sun/ambient/shadow — sun shadows
*are* built); height/elevation in the **simulation** (`z` is render-only — the world
stays a 2-D grid); a 3-D minimap (it stays top-down); per-mite distinct models (one
instanced mesh for the swarm); grass props/trees, normal-mapped imposters. Each is its
own future step; note it here, not in the code.
