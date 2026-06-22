# Prompt — Create Chapter 2: Bigger Maps & Pathing

You are building **chapter 2** of the interactive book that teaches
*data-oriented design* by growing one small game, one feature at a time. Chapter 1
is a two-tank Atari-*Combat* game (in `tank-combat/chapter-1/`). Chapter 2 adds a
much bigger world and **pathing**: tanks that route themselves across it.

Each chapter is a self-contained web page: the running game on top, a written
walk-through below, and the game's live data shown and editable *in the context
of the text*.

## Starting point — copy chapter 1, then strip the prose

1. Copy `tank-combat/chapter-1/` to `tank-combat/chapter-2/` verbatim and get it
   building/running first (`build.sh`, `test.sh`) — chapter 2 is an *increment* on
   chapter 1, not a rewrite. Reuse its sim core: fixed-point types, the bitset
   grid, `tanks_move`/`tanks_turn`/`collide`, the baked trig and escape tables,
   the sim/render/wasm split, the instance-buffer frequency-of-change split, the
   debug-widget page, version stamping, and the native test harness.
2. **Strip the page's article text.** Chapter 2 discusses net-new ideas, so remove
   chapter 1's explanatory prose from `index.html` (the `<article>` write-up) but
   keep the page *structure*: the game canvas, the on-screen controls, the live
   debug-widget anchors, the version badge, the styling. Write fresh chapter-2
   prose for the new concepts as you build them.
3. Add a chapter-2 entry to the book's table of contents (`tank-combat/index.html`)
   and give chapter 2 its own `README.md` and `CONTRACT.md`.

## What chapter 2 adds

### A 4×4 world of screen-grids, with scrolling
- The world is a **4×4 arrangement of screen-grids**, each one a chapter-1-sized
  grid (keep `GRID_W`×`GRID_H` per screen). The viewport shows **one screen-grid
  at a time**.
- Add on-screen **scroll buttons** (left/right and up/down) that move the viewport
  by one screen-grid.
- The world is **toroidal at its outer edge** (scrolling/driving off the right of
  the 4×4 wraps to the left, top to bottom, etc.), exactly like chapter 1's wrap
  but at the 4×4 boundary.
- **Inner edges between screen-grids are connected like the wrap, by matching open
  elements**: a tank crosses from one screen-grid into its neighbour wherever the
  edge cell and the neighbouring edge cell are both open (the same far-side-wall
  rule chapter 1 already uses — so mechanically the world is one big
  `(4·GRID_W)×(4·GRID_H)` toroidal grid, *organised* as 4×4 screens for display,
  scrolling, and the two-level pathing below).

### Two tank classes
Replace chapter 1's two identical tanks with two **classes**, stored as data (a
class tag or separate SoA arrays — your call, justify it):
- **Manual** — exactly **one** instance, controlled as in chapter 1. There is only
  **one** set of on-screen controls (keyboard + one touch pad), and they drive
  this tank.
- **Pathed** — a small fixed number of instances (you choose `N_PATHED`, keep it
  minimal and explicit). A pathed tank is given a destination by the player and
  drives itself there along the shortest route:
  - **Click a pathed tank to select it**, then **click a grid cell to set its
    destination**. The tank then follows the route.
  - **Highlight the path**: while choosing (and while the tank follows it), draw
    the planned route — the cells it will traverse, across screen-grids — updating
    as you move the destination around.

A pathed tank's per-tick "input" is **derived from the path tables** (the next
direction toward its goal) and fed into the *same* movement model as the manual
tank — don't fork the movement transform; produce its input from the lookup.

## Binding guidance — read first, follow strictly

These are requirements, not suggestions.

1. **`data-oriented-design.md`** — Mike Acton's rules
   (raw: `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`).
   Understand the real data first; write the simplest machine that turns the input
   you have into the output you need; exploit every constraint; solve only the
   problem you have.
2. **`AGENTS.md`** at the repo root — every rule applies. The pathing here is a
   textbook case for several of them: **match the work to the frequency of change**
   (the grid changes rarely; tanks path constantly → precompute tables, read a
   lookup per tick); **the table is the data, runtime is a lookup**; **size every
   structure to its real domain**; **find the minimal data the answer depends on**;
   integer/fixed-point only; no dynamic allocation; sim/render/wasm separation with
   native tests.
3. **`grid-paths`** (in this repo: `grid-paths/paths.js`, live at
   `macton.github.io/grid-paths`). The intra-screen pathing method comes from here
   — read it before designing.

## The pathing data design (the heart of the chapter)

Path **hierarchically**, two levels, both built as precomputed tables read by a
per-tick lookup (never a per-tick search):

### Level 1 — within a screen-grid: grid-paths all-pairs routing
Within a single screen you must be able to path **any cell to any cell** (including
the connecting edge points), so the intra-screen data is effectively the
**all-pairs** table grid-paths builds: for a **target** cell, the next step toward
it from each source (grid-paths' `{ dir, dist }`, with its `dist = -1` / island
handling for unreachable). Per-tick pathing is then a **lookup of the current
cell's next step**. Build it by grid-paths' shortest-path fill, updating
incrementally if you allow live wall edits.

All-pairs sounds huge but is small once stored to its real domain — and keeping it
small *is* the data-oriented exercise of this chapter. Per screen there are
`GRID_W·GRID_H` (= 300) cells → ~90k ordered (source, target) pairs. The levers,
to be worked out carefully (the exact scheme is yours to design and justify):
- **A direction is 2 bits** (N/S/E/W): ~90k × 2 bits ≈ 22 KB/screen (~360 KB for
  16) if you store direction directly. Decide how to encode what doesn't fit 2
  bits: **unreachable ("blocked") is rare → keep it out of the 2-bit field** (a
  separate sparse set / per-screen list) and pick one canonical step on ties. For a
  first cut, 3–4 bits/element (room for a "none" code) is fine; tighten later.
- **The pair is symmetric**, so there is ~2× redundancy to fold — store each
  unordered pair once. But fold along the symmetry that *actually* holds: the
  **distance** is symmetric (`dist(a,b) == dist(b,a)`) and the shortest-path cell
  set is symmetric, whereas the **next direction** from a→b is *not* the next
  direction from b→a (it is the far end of the same path). A clean resolution is to
  store the **symmetric all-pairs distance** (upper triangle) and **derive the
  2-bit direction at runtime** by stepping to the lower-distance neighbour — store
  the metric, derive the arrow (the "find the minimal data the answer depends on"
  move). This also hands Level-2 its edge-point distances for free.
- **The per-target tables are not independent** — grid-paths shows that one
  obstacle edits many targets' tables and that neighbouring targets' fields differ
  by little. So do **not** store 300 independent per-cell tables; share their
  common structure. Read grid-paths closely for these couplings.

State the final layout and per-screen / total byte budget. The destination the
player clicks is just a target slice of this table — no separate on-demand build.

### Level 2 — between screen-grids: a next-hop matrix over edge points
The **connecting edge points** are the open cells on screen-grid borders that
connect to a neighbour (or wrap) — the matching open elements. Build a **matrix of
all connecting edge points to all other connecting edge points**: given a start
point **A** and an end point **B**, the entry stores the **next edge point C** on
the shortest route from A to B (filled in the same style as grid-paths' next-step
tables, but the "step" is the next edge point, chosen by **shortest total
length**). Compose the two levels: the distance between two edge points *on the
same screen* is exactly that screen's Level-1 `dist`; crossing a matched inner edge
costs one step. So the matrix is built **on top of** the Level-1 distances — call
that composition out; it is the data-oriented win.

### Putting them together (a pathed tank's route)
Destination clicked → find its screen and cell. If it's the tank's current screen,
follow that screen's Level-1 flow field to the cell. Otherwise: look up the
Level-2 next-hop matrix from the tank's current exit edge point toward the
destination screen to get the next edge point C; follow Level-1 toward C; cross the
inner edge; repeat until in the destination screen, then follow Level-1 to the
cell. The highlighted path is read straight out of these tables.

Keep all of this **integer, packed, statically sized, derived from the
rarely-changing grid/connectivity, and read by a per-tick lookup**. The wasm
linear memory budget may need raising from chapter 1's fixed size — that is a
deliberate static sizing decision (still no dynamic allocation), so size it to the
tables and say what it is.

## What to deliver

- The chapter-2 sim core (extending chapter 1): the 4×4 world, the two tank
  classes, the Level-1 flow-field builder, the Level-2 edge-point next-hop matrix,
  and the per-tick path-follow that turns a tank's goal into movement input — all
  freestanding `wasm32`, integer fixed point, no allocation, one-way dependency
  (render/wasm depend on the sim, never the reverse).
- **Native tests** of the pathing (no browser/GPU): a path reaches a reachable
  target and never enters a wall; it takes a shortest-length route; an unreachable
  target is reported, not looped on; inter-screen routes reach the destination
  screen and cell across matched inner edges and across the outer wrap; the
  next-hop matrix's hop distances equal the composed Level-1 + crossing distances.
- The WebGPU page: one-screen viewport with scroll buttons, the single manual
  control set, click-to-select / click-to-set-destination for pathed tanks, the
  live path highlight, and live debug widgets for the new data (the world/screen
  layout, a tank's class + goal + current `dir` lookup, a screen's flow field, the
  edge-point list and the next-hop matrix) embedded next to the prose.
- `build.sh`/`test.sh` (extended), a visible cache-busted version, `README.md`,
  and a `CONTRACT.md` covering the new behaviours, with the contract tested.

## Movement & path behaviour to pin (so it's reproducible)

Inherit chapter 1's pinned behaviour (sized tank footprint, rotate-then-move tick
order with auto-steer reacting to the previous tick's `hit`, slide-on-walls,
toroidal wrap, collide-speed modifier, manual-turn suppresses auto-steer). On top:

- **One manual tank, one control set.** The keyboard and the single on-screen pad
  drive the manual tank only.
- **Pathed tanks follow the shortest route** produced by the tables, take a wall-
  free path, cross matched inner edges and the outer wrap, and **stop on arrival**
  at the destination cell. An unreachable destination produces no path (and is
  shown as such), never a spin or a wall-grind.
- **Selection & destination are explicit, single-target:** clicking a pathed tank
  selects it; clicking a cell sets that tank's destination and replaces any prior
  one; the highlighted path always reflects the current tables.
- **The viewport is presentation only** — scrolling changes what you see, never
  the simulation; a tank keeps pathing whether or not its screen is in view.

## Definition of done

- Chapter 2 builds freestanding to wasm and natively; `test.sh` passes, including
  the new pathing/contract tests.
- No floating point in the sim; no dynamic allocation; every path table is
  precomputed against the rarely-changing grid/connectivity and read by a per-tick
  lookup; data is packed and sized to its real domain (state the memory budget).
- The page shows the bigger world, the two tank classes, scrolling, and self-
  pathing tanks with a live highlighted route, and explains each data structure
  (Level-1 flow field, Level-2 next-hop matrix, their composition) with the live
  data in context.
- You can state, for each table and transform, what it costs, how often it is
  built vs read, and why it is shaped the way it is.
