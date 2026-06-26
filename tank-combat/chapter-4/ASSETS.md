# Chapter 4 assets — provenance & licences

Assets in this chapter are **late-bound data**. The renderer's first pass needs none
(it draws a unit cube, shaped per instance); the second pass binds a baked low-poly
**mesh per kind** through one table. Swapping art is a render-only change — it touches
neither the projection, the depth, the shading, nor the simulation.

## What ships in the repo

The meshes are **host-authored, original, procedural low-poly geometry** — built in
`tools/gen_meshes.c` and baked to committed `src/mesh_data.c` (the same bake-on-the-host
discipline as the escape table). They are released **CC0 / public domain**; no
attribution required. Nothing is downloaded at build or run time, and no glTF parser
ships in the binary — the runtime loads a packed `int8` vertex buffer (162 vertices, ~1.3 KB),
uploaded once at startup.

| mesh (`mesh_data.h`) | geometry | bound to kind | used for |
|---|---|---|---|
| `M_CUBE`    | unit box | every kind (placeholder pass) + `K_FLOOR`, `K_WALL`, `K_BARREL` (asset pass) | floors (thin slab), walls (full cube), barrels, and the placeholder for all kinds |
| `M_PYLON`   | tapered tower (frustum) | `K_NEST` | the 15 nest markers |
| `M_PYRAMID` | square pyramid | `K_MITE` | the thousand mites (a little spike) |
| `M_HULL`    | tapered box | `K_HULL` | tank bodies (oriented by `tank_ang`) |
| `M_TURRET`  | tapered dome (frustum) | `K_TURRET` | tank turrets (oriented by `tank_turret`) |

Translucent FX (the bolt streak/head and the destruction bursts) draw as `M_CUBE` in the
blended pass — no distinct mesh needed.

## The binding point — where external art drops in

The asset pass is **one table**, `MESH_FOR_KIND[]` in `src/mesh_data.c` (generated),
read by `app.js` per draw. To use a different model for a kind:

1. Convert/strip the source model at **build time** into the engine's compact vertex
   format (non-indexed triangle list, `int8` position + face normal in a unit box) and
   emit it from `tools/gen_meshes.c` as a new `M_*` mesh — exactly as the cube/pyramid
   are emitted today. **Do not** ship multi-megabyte source art or a runtime parser;
   commit the baked buffer.
2. Point the kind at it in `MESH_FOR_KIND` (e.g. `[K_HULL] = M_TANK`).

Nothing else moves — the instance data (placement, facing, tint) is unchanged, and a
glTF tank simply replaces the body box.

## Curated external art (itch.io / Kenney / Quaternius)

If you'd rather drop in real downloaded art than the host-baked originals above, this
is a short, verified-at-time-of-writing list. **Prefer CC0** (public domain, no
attribution) so the swap is zero-friction; where a pack is "free + credit," keep the
credit here. **Confirm the licence on each page before using it — terms change.**

| asset | creator | licence | format | candidate use |
|------|---------|---------|--------|-------------|
| [Isometric Blocks](https://kenney-assets.itch.io/isometric-blocks) (130+ tiles) | Kenney | **CC0** | PNG tiles | terrain tiles (sprite path) |
| [Isometric Prototypes Tiles](https://kenney-assets.itch.io/isometric-prototypes-tiles) | Kenney | **CC0** | PNG | floors/walls + a stand-in unit |
| [Kenney 3-D kits](https://kenney.nl/assets) (Tower-Defense, Tanks, Top-down) | Kenney | **CC0** | glTF/OBJ | tank + nest + prop meshes |
| [FREE Stylized Tank](https://mreliptik.itch.io/free-lowpoly-tank-3d-model) (+ [4-tank pack](https://mreliptik.itch.io/4-low-poly-stylized-tanks)) | MrEliptik | free, **credit requested** | glTF/FBX | the four tank meshes |
| [Quaternius low-poly packs](https://quaternius.com/) (animals, nature, ultimate kits) | Quaternius | **CC0** | glTF/OBJ/FBX | mite "creature" meshes, nests, props |

Browse for more (filtered):
[isometric + tanks](https://itch.io/game-assets/tag-isometric/tag-tanks) ·
[CC0 + isometric](https://itch.io/game-assets/assets-cc0/tag-isometric) ·
[CC0 + free + low-poly](https://itch.io/game-assets/assets-cc0/free/tag-low-poly) ·
[free + low-poly + tanks](https://itch.io/game-assets/free/tag-low-poly/tag-tanks).

Keep the imported set **small and sized to the need**: one tank mesh re-tinted four
ways beats four near-identical downloads; one mite mesh; a handful of tile kinds. The
four tank identity colours are applied as the per-instance **tint**, so a single
neutral tank mesh serves all four.
