# Tank Combat — Chapter 2: Bigger Maps & Pathing

Chapter 2 of *Data-Oriented Design, by picture* (the book index is one level
up). It grows [chapter 1](../chapter-1/)'s single 20×15 screen into a **4×4 world
of screen-grids** and adds **pathing**: tanks that route themselves to a clicked
destination along the shortest wall-free way, across screens and around the
world's wrap. **The point of the project is to demonstrate a data-oriented
approach** — here, that a shortest-path system is just *precomputed tables read
by a per-tick lookup*, sized to their real domain. Chapter 2 is an *increment* on
chapter 1: it reuses its sim core (fixed-point types, the bitset grid,
`tanks_turn`/`tanks_move`/`collide`, the baked trig and escape tables, the
sim/render/wasm split, the instance-buffer split, version stamping, the native
test harness) unchanged where it can.

Built following Mike Acton's data-oriented design rules:
[**data-oriented-design.md**](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md).

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge).
- **Manual tank (amber, index 0):** `W`/`S` move, `A`/`D` turn — or the on-screen
  D-pad. There is exactly one control set and it drives this tank only.
- **Pathed tanks (indices 1–3):** click one to select it, then click a grid cell
  to send it there. Its planned route is highlighted; it follows it and stops on
  arrival. An unreachable target shows as *no path* rather than spinning.
- **Scroll** the viewport one screen with the on-screen arrows (or the minimap).

## The world: one big toroidal grid, organised as screens

Mechanically the world is a single **`(4·GRID_W)×(4·GRID_H)` = 80×60 toroidal
grid**. The 4×4 split into 20×15 *screens* is an organisation for display (the
viewport shows one screen), scrolling, and the two-level pathing. The grid is the
chapter-1 bitset, now screen-major: `grid[screen*GRID_H + cy]`, one 20-bit word
per screen-row. `cell_is_wall(grid, wcx, wcy)` resolves a world cell to its screen
and wraps at the big-grid edge, so:

- **Inner screen edges connect by the same rule as the wrap.** A tank crosses
  into a neighbour wherever the border cell and the neighbouring cell are both
  open — chapter 1's "depends only on the wall on the far side", now at every
  screen boundary. The collision footprint and the steer's neighbour probes cross
  borders with no special cases.
- **The default map** (generated on the host, `tools/gen_map.c` →
  `src/map_data.c`) gives every screen solid borders with a one-cell gap at the
  centre of each side; the gaps line up across inner borders and the outer wrap,
  so the whole world is one connected toroidal grid (64 connecting edge points).

## Two tank classes, stored as a partition

There is one **manual** tank and `N_PATHED` (= 3) **pathed** tanks. Both classes
share every per-tank field (`tank_xy`, `tank_ang`, `tank_in`, …) and run through
the *same* `tanks_turn`/`tanks_move`. The only difference is the **source** of
`tank_in`: the manual tank from `set_input`, a pathed tank from `tanks_path` (the
path lookup). So there is **no class tag** — the index range *is* the class
(`0` manual, `1..N_PATHED` pathed). Pathed-only state (destination, remaining-
distance vector) lives in arrays sized `N_PATHED`, not `N_TANKS`.

A pathed tank's per-tick input is the next cardinal toward its goal, turned into
the *same* button bits the manual tank uses (turn toward the cardinal; drive only
when the discrete heading is exactly aligned). Because `tanks_move` steps along
`di = angle>>11` (the discrete heading), "exactly aligned" is exactly axis-
aligned, so the tank tracks grid cells and never clips a corner — **the movement
transform is not forked**. It snaps to the cell centre at each turn (a Pac-Man-
style grid tracker), which keeps it centred through the one-cell inter-screen
gaps regardless of speed.

## The pathing: two precomputed levels, read by a lookup

The expensive part — finding shortest routes — is done against the rarely-changing
grid and **stored**; per tick a tank reads the next step. All-pairs *sounds* huge;
sizing it to its real domain is the exercise.

### Level 1 — within a screen: all-pairs distance (`grid_paths.c`)

Within one screen, route any cell to any cell. We **store the metric and derive
the arrow**:

- **Store** the symmetric all-pairs **distance**, upper triangle only — one
  `uint16` per unordered pair (`dist(a,b) == dist(b,a)` because the within-screen
  graph is undirected). Built by a BFS from every open cell, confined to the
  screen; wall/disconnected cells are `L1_INF`.
- **Derive** the next direction at runtime: the next step from `s` toward target
  `t` is the in-screen neighbour whose stored distance to `t` is one less
  (canonical N,E,S,W tie-break). Direction is *not* symmetric, so it can't be
  folded — but distance can, and the distance is also exactly what Level 2 needs,
  so one table serves both ("find the minimal data the answer depends on").

**Budget:** `TRI = 300·301/2 = 45,150` pairs × 2 bytes = **90 KB/screen**,
**1.41 MB** for 16 screens — the dominant cost. The raw domain is 0..299 (9 bits);
we spend a byte-aligned `uint16` for a branch-free hot-path index, a deliberate
trade in a budget we are already raising (a 9-bit pack is the listed tightening).
Built on init and rebuilt for *one* screen on a wall edit there; read every tick.

### Level 2 — between screens: an edge-point next-hop matrix (`edge_paths.c`)

The **connecting edge points** are the open border cells whose neighbour across
the matched border is also open. Over those points we build an all-pairs next-hop
matrix (`nexthop[A][B]` = next edge point toward B; `dist2[A][B]` = its length) by
Floyd–Warshall — **on top of Level 1**: two edge points on the same screen are
joined by an edge weighted with that screen's Level-1 distance; crossing a matched
border costs 1. So Level 2 never re-walks the grid; it reuses the Level-1
distances. With the default map there are 64 edge points, so the matrix is tiny
(`dist2`/`nexthop` ≈ 48 KB at the static cap).

### Composition (a route, with no per-destination grid search)

When a destination is set we fold the matrix into a small per-tank vector
`pg[Y]` = shortest **remaining** distance from edge point `Y` to the goal
(`dist2(Y, E)` to a goal-screen entry point `E`, plus that screen's Level-1
distance from `E` in to the goal cell). That fold happens once per destination
change. Per tick a pathed tank in screen `S`:

- if the goal is in `S` and Level-1-reachable → follow `S`'s Level-1 flow to it;
- else → pick the exit edge point `X` of `S` minimising
  `L1(cell→X) + 1 + pg[partner(X)]`, then follow Level-1 toward `X` (and cross
  when on it).

A handful of table reads, **never a per-tick search**. Every step strictly
decreases the true remaining distance, so the route is **globally shortest**
(verified in tests against a brute-force BFS over the whole 80×60 grid), never
enters a wall, and an unreachable goal yields *no path* (status `PS_NOPATH`)
rather than a spin. The on-screen highlight is `path_trace` — the same per-tick
lookup dry-run cell by cell — so it is read straight out of the tables the tank
follows.

## Source layout

Reused from chapter 1, generalised to the big grid: `tanks_turn.c`,
`tanks_move.c`, `dirtab.c`, `escape_table.c`. `collide.c` now resolves a world
cell to its screen (`cell_is_wall`) and wraps at the big-grid edge.

New in chapter 2:

| file | responsibility |
|------|----------------|
| `src/grid_paths.c/.h` | LEVEL 1: per-screen all-pairs distance (BFS fill), the `tri_index` packing, `l1_next_dir` (derive the arrow from the metric) |
| `src/edge_paths.c/.h` | LEVEL 2: enumerate edge points + partners, Floyd–Warshall next-hop, `l2_compute_pg` (compose), `route_next_dir` (per-tick step), `path_trace` (highlight) |
| `src/tanks_path.c/.h` | per-tick transform: a pathed tank's input bits from the route lookup (grid-tracking) |
| `src/map.h`, `src/map_data.c` | the baked 4×4 world map (generated by `tools/gen_map.c`) |

`sim.c` holds the `World` (now with the path tables), the per-tick order
(`tanks_path` → `tanks_turn` → `tanks_move`), and the rare mutators
(`sim_toggle_wall`, `sim_set_dest`, `sim_scroll`) that rebuild only the tables a
change can affect. `render.c` builds the one-screen viewport (camera-local quads:
walls, tanks in view, the highlight, edge markers). `wasm.c` exports the protocol
and pointers to every table for the live widgets. The dependency is one-way:
render/wasm depend on sim, never the reverse.

## Memory budget (static, no dynamic allocation)

The wasm linear memory is raised from chapter 1's 1 MiB to **4 MiB**, sized
statically to the tables:

| data | size |
|------|------|
| Level-1 distances `l1dist[16 · 45150]` `uint16` | ~1.41 MB |
| Level-2 `dist2` (`uint16`) + `nexthop` (`uint8`), 128² | ~48 KB |
| per-tank `pg`, edge-point arrays, world scalars | ~5 KB |
| instance buffer + highlight trace scratch | ~20 KB |

`game.wasm` itself is ~14 KB of code. Every table is precomputed against the
rarely-changing grid/connectivity and read by a per-tick lookup; data is packed
and sized to its real domain.

## Build

Requires `clang` with the `wasm32` target and `wasm-ld` (LLVM ≥ 15). No
emscripten.

```sh
./build.sh        # regenerates src/escape_table.c + src/map_data.c, writes game.wasm
```

`build.sh` runs `gen.sh` first to regenerate the host-baked data (escape table +
world map) from current geometry, then compiles. `game.wasm` and the generated
sources are committed so GitHub Pages serves them without a build step.

## Testing

Because the simulation is separated from rendering and wasm, it builds and runs
on the host:

```sh
./test.sh         # compiles the sim core + src/test.c with host clang, runs it
./analyze.sh      # the inherited steer's sampled-cell / 16-pattern analysis
```

`src/test.c` covers (37 checks, see [CONTRACT.md](CONTRACT.md)): Level-1 symmetry
and that the derived arrow walk reaches a target in exactly the stored distance;
edge-point partners and that **next-hop hop distances equal the composed Level-1 +
crossing distances**; that hierarchical route lengths **equal a brute-force BFS
over the whole 80×60 grid** (globally shortest) for same-screen, cross-screen, and
both wraps; that a live pathed tank reaches reachable targets **without its
footprint ever overlapping a wall** and stops on the destination cell; that an
unreachable (sealed) target is reported, not looped on; that selection/destination
are explicit and single-target; that **scrolling never changes the simulation**;
and that the inherited movement model still holds on the big grid.

## Verified

- `./test.sh` passes (37 checks): the pathing tables, the globally-shortest-route
  proof vs brute-force BFS, wall-free arrival, unreachable handling, the
  selection/destination/viewport contract, and inherited movement.
- `game.wasm` exercised in Node (init, all three pathed tanks routing to cross-
  screen and wrap destinations and arriving on the exact cell, wall-toggle table
  rebuild, scroll, and the flow-field JS logic matching the C) — identical to
  native, as expected since they share `sim.c`.
- **Not** verified here: the live WebGPU render and on-screen touch/scroll — no
  GPU/browser in the build environment. The host uses standard WebGPU calls;
  render-test in a browser.

## Deferred (not built — no speculative generality)

Firing/projectiles, tank-vs-tank collision, dynamic edge-point counts beyond the
static cap, incremental (rather than full-rebuild) Level-1 updates on wall edits,
moving pathed tanks avoiding each other, multiple maps. Each is its own future
step.
