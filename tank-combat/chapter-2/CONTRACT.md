# Chapter 2 contract — pathing & viewport

The explicit promises chapter 2 makes, so they can be relied on and tested. The
tests in `src/test.c` enforce them (37 checks). Chapter 2 **inherits chapter 1's
movement contract** ([../chapter-1/CONTRACT.md](../chapter-1/CONTRACT.md)) for the
manual tank — sized footprint, rotate-then-move tick order, auto-steer reacting to
the previous tick's `hit`, slide-on-walls, toroidal wrap, the collide-speed
modifier, manual-turn suppresses auto-steer — now on the 80×60 toroidal world.

## The world

- **One connected toroidal grid.** The world is a single `(4·GRID_W)×(4·GRID_H)` =
  80×60 grid, organised as 4×4 screen-grids. A tank crosses from one screen into
  its neighbour, and around the outer wrap, wherever the edge cell and the
  neighbouring cell are **both open** — the same far-side-wall rule as chapter 1.
- **The viewport is presentation only.** Scrolling moves what you see by whole
  screen-grids (toroidally); it never touches the simulation. A tank keeps pathing
  whether or not its screen is in view. *(tested: scroll then assert tank
  state/frame unchanged; an off-screen tank still reaches its destination.)*

## The two tank classes

- **One manual tank, one control set.** The keyboard and the single on-screen pad
  drive the manual tank (index 0) only. *(tested: the manual tank is not
  selectable as a pathed target.)*
- **Pathed tanks follow the shortest route** produced by the tables, take a
  wall-free path, cross matched inner edges and the outer wrap, and **stop on
  arrival** at the destination cell.

## Pathing promises (each has a test)

- **Level-1 distance is symmetric and its derived arrow is correct.** For a screen,
  `dist(a,b) == dist(b,a)`, and walking the derived next-direction from any cell to
  a target reaches it in exactly the stored distance, on open cells only.
- **Edge points are mutually matched.** Every connecting edge point has a partner
  that is the open cell directly across its matched border, and the partnership is
  mutual.
- **Level 2 composes Level 1.** Following the next-hop matrix from any edge point A
  to any reachable B, summing each step's weight (1 to cross a matched border, else
  the Level-1 distance between two same-screen edge cells), equals `dist2[A][B]`.
- **Routes are globally shortest.** A pathed route's length equals a brute-force
  breadth-first shortest path over the entire 80×60 toroidal grid — for a
  same-screen target, a cross-screen target (across matched inner edges), and a
  target across each outer wrap.
- **A pathed tank reaches a reachable target without ever entering a wall.** Across
  the run its footprint never overlaps a wall cell, and it stops on the exact
  destination cell. (Holds for same-screen, cross-screen, and wrap targets.)
- **An unreachable destination produces no path, not a spin.** If the goal is in a
  sealed region, the tank's status is `PS_NOPATH`, it never drives into a wall, and
  it stays put — no spin, no wall-grind.
- **Selection & destination are explicit, single-target.** Clicking a pathed tank
  selects it; clicking a cell sets that tank's destination and **replaces** any
  prior one; clearing removes it. The highlighted path always reflects the current
  tables (it is `path_trace`, the same per-tick lookup dry-run).

## Boundaries / non-promises

- **Shortest is in the hierarchical/grid sense** above (BFS step count on the open
  cells), and is what the tests pin. Ties are broken canonically.
- **Pathed tanks turn in place at cell centres**, so a route with many turns is
  slower than a straight one; tune `turn_rate`/`move_speed` live.
- **Tanks do not collide with each other** (inherited from chapter 1), so pathing
  is independent of other tanks' positions and fully deterministic.
- **Edge points are statically capped** (`N_EDGE_MAX`); the curated map and normal
  edits stay well under the cap. Opening enough border cells to exceed it would
  leave some crossings unconnected — a deliberate static-sizing boundary.
- **Wall edits rebuild fully, not incrementally.** Toggling a wall rebuilds that
  screen's Level-1 table and the whole of Level 2; cheap because it is rare.

## How it's tested

`./test.sh` builds the simulation core (no wasm, no renderer, no GPU) with the
host C compiler and runs `src/test.c`, which builds the world, exercises the path
tables and a live pathed tank, and asserts each promise above — including a
brute-force BFS over the whole 80×60 grid as the ground truth for "shortest".
Because the simulation is separated from rendering, these tests need no browser or
GPU.
