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
- `app.js` uploads that instance buffer **once** and, per frame, only **frustum-culls the
  screen buckets** and draws the visible runs. The sole per-frame map cost is the cull —
  no re-emit, no re-classification. It re-bakes (in wasm) **only** when the map itself
  changes (a wall toggled, a nest moved); a version counter tells the page to re-upload
  exactly then.

This is the chapter's thesis applied to the map: the sim never knows it became a town —
the town is a *projection* of the sim's walls and nests, the same bake-it-once discipline
as the escape table.

## What ships in the repo

**Procedural meshes** — original, CC0, no attribution. A packed `int8` vertex buffer
(12 bytes/vertex: position + face normal + white colour), uploaded once. No glTF parser
ships in the binary.

| mesh (`mesh_data.h`) | geometry | bound to kind | used for |
|---|---|---|---|
| `M_CUBE`    | unit box | every kind (placeholder pass) + `K_RING`, `K_BARREL`, `K_DEST` (asset pass) | overlays, barrels, beacons, and the placeholder for all kinds |
| `M_PYLON`   | tapered tower | *(unbound)* | spare — the nest is a town landmark now |
| `M_PYRAMID` | square pyramid | `K_MITE` | the swarm (a little spike) |
| `M_HULL`    | tapered box | `K_HULL` | tank bodies (oriented by `tank_ang`) |
| `M_TURRET`  | tapered dome | `K_TURRET` | tank turrets (oriented by `tank_turret`) |

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
| [Kenney 3-D kits](https://kenney.nl/assets) (Tanks, Top-down) | Kenney | **CC0** | glTF/OBJ | tank + prop meshes |
| [FREE Stylized Tank](https://mreliptik.itch.io/free-lowpoly-tank-3d-model) (+ [4-tank pack](https://mreliptik.itch.io/4-low-poly-stylized-tanks)) | MrEliptik | free, **credit requested** | glTF/FBX | the four tank meshes |
| [Quaternius low-poly packs](https://quaternius.com/) | Quaternius | **CC0** | glTF/OBJ/FBX | mite "creature" meshes, props |

Keep the imported set **small and sized to the need**: one tank mesh re-tinted four ways
beats four near-identical downloads. The four tank identity colours are applied as the
per-instance **tint**, so a single neutral tank mesh serves all four.
