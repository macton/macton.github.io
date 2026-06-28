# Chapter 4 assets — provenance & licences

Assets in this chapter are **late-bound data**, split by *frequency of change* into two
mesh tables that share one GPU vertex buffer:

- **Procedural meshes** — the **dynamic** things (tanks, the swarm, FX, overlays). Host-
  authored original low-poly geometry, baked by `tools/gen_meshes.c` → committed
  `src/mesh_data.c`. The renderer's first pass needs none of them (it draws a unit cube,
  shaped per instance); the second pass binds a mesh per kind through `MESH_FOR_KIND[]`.
- **Town meshes** — the **static** map (roads, buildings, grass, a landmark). Baked from
  Kenney CC0 kits by `tools/bake_map_assets.mjs` → committed `src/map_mesh_data.c`.

Swapping art is a render-only change — it touches neither the projection, the depth, the
shading, nor the simulation.

## The static town (the map is data)

The map is **static and totally known**: which cells are walls, where the nests sit. So
the *town* it becomes is known too, and it is baked **once** — never re-derived per frame:

- `src/staticmap.c` (`build_static_map`) classifies every world cell — **wall → a
  building** (hash-picked variant + rotation), **an open cell touching a wall → an
  autotiled road**, **a wide-open interior → grass**, **a nest → a landmark** (tinted in
  that nest's hue) — and packs **one instance per cell**, grouped by `(screen, mesh)` into
  a small run table.
- **Road connectivity** (`compute_road_map`): a building cluster stranded in the grass
  would otherwise bake a *closed ring* of road that touches no street — a **"traffic
  island"** you can't drive off. So before classifying, the bake lays the base ring down,
  labels the road's connected components, and **bridges every isolated ring to the main
  network** by carving the shortest **grass corridor** to it (never through a building — the
  sim has a wall there). The few road pockets sealed *inside* a block (no grass to bridge
  through) are dropped back to grass, so **every road cell ends up in one connected network**.
  Still render-only and baked once: the sim never sees a road, a ring, or a bridge.
- `app.js` uploads that instance buffer **once** and, per frame, only **frustum-culls the
  screen buckets** and draws the visible runs — no re-emit, no re-classification. (The one
  exception is cosmetic: a building **struck by a bolt** buzzes for a few ticks, so the host
  nudges just that instance's position from the sim's `wall_shake` state — a handful of
  bytes, not a re-bake.) It re-bakes (in wasm) **only** when the map itself changes (a wall
  toggled, a nest moved); a version counter tells the page to re-upload exactly then.

This is the chapter's thesis applied to the map: the sim never knows it became a town —
the town is a *projection* of the sim's walls and nests, the same bake-it-once discipline
as the escape table.

### Static props (trees)

A second baked-once layer, alongside the town: low-poly **trees** (`M_TREE`) scattered for
visual interest on the grass. `src/staticmap.c` (`build_static_props`) places one at a **grid-
cell corner** wherever the four cells meeting there are all grass — deep in the open, hash-
thinned to ~22% of eligible corners, each grounded with a hash-driven size — and groups them
by screen into runs, exactly like the town. The host uploads the buffer once and frustum-culls
per screen; the trees also cast into the baked sun shadow map. Because they sit on **corners,
not one-per-cell**, they live in their **own buffer** — the town's one-instance-per-cell
invariant (which the wall-shake cell→instance map depends on) is left intact. They are pure
scenery: the **sim never sees them** (no collision), and a **thin trunk** parked on a cell
boundary reads as decoration, not an obstacle — there is no reason to expect a tank to hit one.

## What ships in the repo

**The dynamic mesh table** — a packed `int8` vertex buffer (12 bytes/vertex: position + face
normal + colour), uploaded once. No glTF/OBJ parser ships in the binary: `tools/gen_meshes.c`
bakes both the **original host-authored shapes** (CC0, no attribution) **and** a few **CC0
external models** (read from a local download at bake time, OBJ → the same packed format —
exactly the town's discipline) into one committed `src/mesh_data.c`.

| mesh (`mesh_data.h`) | geometry | bound to kind | used for |
|---|---|---|---|
| `M_CUBE`    | unit box | every kind (placeholder pass) + `K_RING`, `K_DEST` (asset pass) | overlays, beacons, and the placeholder for all kinds |
| `M_PYLON`   | tapered tower | *(unbound)* | spare — the nest is a town landmark now |
| `M_PYRAMID` | square pyramid | `K_MITE` (far LOD) | the swarm drawn cheap when the whole pool is in view |
| `M_HULL`    | tapered box | *(fallback)* | the procedural tank body, used only if the OBJ art is absent at bake |
| `M_TURRET`  | tapered dome | *(fallback)* | the procedural tank turret, same |
| `M_TREE`    | thin trunk + faceted canopy | *(unbound — a STATIC prop)* | scenery scattered on grass corners (its own static-props buffer, not a per-frame kind) |
| `M_TANK_HULL`   | **baked OBJ** — body + tracks | `K_HULL` | tank chassis (oriented by `tank_ang`); per-material grey keeps tracks dark under the identity tint |
| `M_TANK_TURRET` | **baked OBJ** — turret + gun | `K_TURRET` | tank turret + barrel (oriented by `tank_turret`); the gun is part of this mesh, so `K_BARREL` draws nothing |
| `M_MITE`        | **baked OBJ** — low-poly crab | `K_MITE` (near LOD) | the swarm drawn detailed when few are in view; baked white so the mode tint (idle/hunt/home) colours it |

The two tank parts are baked in **one shared unit-box frame** (normalised over the whole tank,
the bottom anchored to the floor), so the renderer draws both at the same centre + scale and only
the **rotation** differs — the hull spins with the chassis heading, the turret with the aim — and
they stay registered as one tank. The **mite LOD** is a per-frame mesh swap in `app.js`: the
detailed skull binds only while ≤ `MITE_LOD_MAX` mites fall in the visible box (zoomed in); past
that the whole swarm draws as the cheap `M_PYRAMID` spike, bounding the per-frame triangle budget
for a pool up to 4096 strong (the mites never cast into the baked sun shadow map, so this is their
only cost). `M_TREE` and the tank's tracks carry their **own baked colours** the same way the town
meshes do; everything else is white so the instance tint shows through.

### The dynamic OBJ art (Quaternius, CC0)

The tank and the mite are **Quaternius CC0** models. Unlike the town's **large** Kenney kits (kept
out of the repo), these two small **CC0** OBJs are **committed** at `tools/quaternius/` — because
`build.sh` re-runs `gen_meshes.c` on every build, so the source must be present for the bake to
reproduce the art (absent it, the build silently falls back to the procedural box/dome + pyramid).
Committing them makes the build hermetic: `./build.sh` always reproduces `src/mesh_data.c` byte-for-
byte. `tools/quaternius/LICENSE.txt` carries the CC0 dedication.

`gen_meshes.c` splits the tank OBJ's object groups (`Tank_body` + the two `TrackMesh` → hull,
`Tank_Turret` + `Tank_Gun` → turret), maps Blender Y-up to the engine's Z-up with the gun pointing
+x, and bakes the crab white with a −90° yaw so its claws lead. To swap in a different Quaternius
model, drop its OBJ into `tools/quaternius/`, point the loader at it in `tools/gen_meshes.c`, and
`./build.sh`. The packs:

| model | pack (Quaternius, **CC0 1.0**) | file | becomes |
|---|---|---|---|
| tank | [Animated Tanks](https://quaternius.com/packs/animatedtanks.html) | `Tank.obj` | `M_TANK_HULL` + `M_TANK_TURRET` |
| crab | [Cute Monsters](https://quaternius.com/packs/cutemonsters.html) | `Crab.obj` | `M_MITE` |

Re-fetch (Google-Drive-hosted) with `pip install gdown` then
`gdown --folder https://drive.google.com/drive/folders/11onydnSTchHz2MZDJ3tK7O3cazIEPQAJ` (tanks,
OBJ) and `…/1LWGbuzCZ2qJwYoPRlMnqTOsFLAfMbBng` (Cute Monsters, OBJ).

Translucent FX (the bolt streak/head and the destruction bursts) draw as `M_CUBE` in the
blended pass — no distinct mesh needed.

**Town meshes** — baked from **Kenney CC0** kits into `src/map_mesh_data.c` at **three LODs**:
16 full-detail **LOD0** + 16 colour-matched **LOD2** flat massing (one 12-byte table, LOD2 at
`MAP_LOD2_OFFSET`, ~406 KB), plus 16 **LOD1 imposters** (a separate 16-byte table that carries
UVs). Each LOD1/LOD2 is a ~40-vert box+roof cage; the imposter additionally samples
`town_atlas.png` — the LOD0 art rendered (offline, by a CPU rasteriser in the baker)
orthographically from the top + 4 sides and packed into one atlas, so the cheap cage shows the
real building's windows/doors from a distance. The committed buffers + atlas are **CC0** (derived
from CC0 source). The *source art is not committed*; the bake reads a local download. Kits used:

| group (`map_mesh_data.h`) | source kit (Kenney, **CC0**) | meshes |
|---|---|---|
| `MAP_ROAD_*`     | [City Kit (Roads)](https://kenney.nl/assets/city-kit-roads) | square, end, straight, bend, intersection, crossroad (autotiled) |
| `MAP_BUILD_*`    | [City Kit (Suburban)](https://kenney.nl/assets/city-kit-suburban) + [(Commercial)](https://kenney.nl/assets/city-kit-commercial) | 8 building variants, roofs recoloured to contrast the grass |
| `MAP_GRASS_BASE` | [Tower Defense Kit](https://kenney.nl/assets/tower-defense-kit) | the ground tile |
| `MAP_LANDMARK_BASE` | City Kit (Commercial) | the skyscraper marking each nest |

### Re-baking the town

Download the kits, then run the offline builder (source art stays out of the repo):

```
node tools/bake_map_assets.mjs <assets-dir> src
# <assets-dir>/city-kit-suburban, /city-kit-commercial, /city-kit-roads, /kenney_tower-defense-kit
```

It decodes each kit's indexed-PNG palette, parses the OBJ, rotates Kenney's Y-up to the
engine's Z-up, normalises each mesh per-axis into a unit box, recolours building roofs,
CPU-rasterises the LOD0 projections into the imposter atlas, and emits
`src/map_mesh_data.{c,h}` (the LOD0 + LOD2 table, the LOD1 imposter table, group bases,
per-mesh height) plus `town_atlas.png`. Nothing runs at game time; the committed tables +
atlas are the deliverable.

## The binding points — where external art drops in

- **Dynamic kind → mesh:** `MESH_FOR_KIND[]` in `src/mesh_data.c` (generated), read by
  `app.js` per draw. Convert a source model at **build time** into the compact vertex
  format, emit it from `tools/gen_meshes.c` as a new `M_*`, and point the kind at it (e.g.
  `[K_HULL] = M_TANK`). The instance data (placement, facing, tint) is unchanged — a glTF
  tank simply replaces the body box. **Do not** ship source art or a runtime parser.
- **Town treatment → mesh:** the group bases in `map_mesh_data.h` and the classify in
  `src/staticmap.c`. Add a building variant to the `BUILD` group in
  `tools/bake_map_assets.mjs` and the hash in `classify` picks it up; the road autotile is
  driven by canonical connection masks read from the kit at rotation 0.

## Curated external art (itch.io / Kenney / Quaternius)

A short, verified-at-time-of-writing list for swapping in real downloaded art. **Prefer
CC0** (public domain, no attribution); where a pack is "free + credit," keep the credit
here. **Confirm the licence on each page before using it — terms change.**

| asset | creator | licence | format | candidate use |
|------|---------|---------|--------|-------------|
| [City Kit (Suburban / Commercial / Roads)](https://kenney.nl/assets/city-kit-suburban) | Kenney | **CC0** | OBJ/glTF | the static town (used here) |
| [Tower Defense Kit](https://kenney.nl/assets/tower-defense-kit) | Kenney | **CC0** | OBJ/glTF | ground tile, props, turrets |
| [Kenney 3-D kits](https://kenney.nl/assets) (Tanks, Top-down) | Kenney | **CC0** | glTF/OBJ | prop meshes |
| [Quaternius — Animated Tanks](https://quaternius.com/packs/animatedtanks.html) | Quaternius | **CC0** | OBJ/FBX/glTF | **the tank (used here)** — `M_TANK_HULL` / `M_TANK_TURRET` |
| [Quaternius — Cute Monsters](https://quaternius.com/packs/cutemonsters.html) | Quaternius | **CC0** | OBJ/FBX/glTF | **the mite (used here)** — the crab, `M_MITE` |
| [FREE Stylized Tank](https://mreliptik.itch.io/free-lowpoly-tank-3d-model) | MrEliptik | free, **credit requested** | glTF/FBX | alt tank meshes |

Keep the imported set **small and sized to the need**: one tank mesh re-tinted four ways
beats four near-identical downloads. The four tank identity colours are applied as the
per-instance **tint**, so a single neutral tank mesh serves all four.
