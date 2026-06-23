# Tank Combat — Chapter 3: The Swarm

Chapter 3 of *Data-Oriented Design, by picture* (the book index is one level up).
It adds an **enemy faction** to [chapter 2](../chapter-2/)'s 4×4 world: **a thousand
mites** that wander, copy the last place they saw a tank on contact, and swarm
toward it. **The point of the project is to demonstrate a data-oriented approach**
— here the lesson is **scale**: one entity is easy, a thousand is a *data layout*.
The volume forces the structures volume needs — a fixed pool with no allocation, a
per-cell spatial index rebuilt every tick, a deterministic integer PRNG, and a
piece of shared knowledge that is just **timestamped data copied on contact**, with
no central manager and no per-unit pathfinding.

Chapter 3 is an *increment* on chapter 2: it reuses the whole sim core (fixed-point
types, the screen-major bitset grid, the movement transforms, the baked trig and
escape tables, the two-level path tables — the tanks keep routing exactly as
before, the sim/render/wasm split, version stamping, the native test harness).

Built following Mike Acton's data-oriented design rules:
[**data-oriented-design.md**](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md).

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge). The
**mites run themselves**. The four tanks are still yours: **tap a tank** to cycle it
*auto-path* (tap a cell to route it there) → *manual* (drive it, `W`/`S`/`A`/`D` or
the pad) → *unselected*. Drive a tank through the swarm and watch a sighting spread
(seeking = red, relaying = amber, wandering = teal) and dissolve when you leave.

## The faction: mites

A **mite** is a **quarter-cell driven body** (`MITE_R = SUB/8 = 32` subcells, so its
diameter is about a quarter cell) on the *same* grid as the tanks. There are exactly
`N_MITES = 1000`, spawned at init and alive for the whole chapter — a **fixed pool**:
flat structure-of-arrays, statically sized, **no allocation**. A mite shares the
tanks' turn and move transforms and collides only with **walls** (its own smaller
radius); it does **not** physically collide with tanks or with other mites. It does
**not** use the path tables — it steers **greedily** and relies on the shared
auto-steer to slide around walls. *4 routed tanks beside 1000 cheap mites — the
algorithm matched to the need.*

### One movement model, sized by a parameter

The tanks and the mites are both **driven bodies on the grid** — a position, a
heading, an input byte, a collision flag — so they share the turn and move
transforms. Chapter 2's `tanks_turn`/`tanks_move` are renamed **`agent_turn`/`agent_move`**
(they operate on "a set of moving bodies," not on tanks), and the collision
half-extent is now a **`radius` parameter** threaded into `blocked_x`/`blocked_y`:
the tanks pass `TANK_R`, the mites pass `MITE_R`. **Size is data you pass, not a
reason to fork the transform.** The auto-steer keys on a cell's 4-neighbour wall
pattern, which is size-independent, so `cell_pattern` and the baked escape table are
unchanged and serve both sizes — the generated table is byte-identical to chapter 2.
The **input producers stay separate** (split by independence): `tanks_path` produces
the tanks' routing input; a new `mites_step` produces the mites'. Both write an input
byte that flows through the same `agent_turn`/`agent_move`.

### The per-cell index — the grid you already have is the acceleration structure

A swarm needs to know, cheaply, **how many mites are in each cell** and **which mites
are near a given mite**. We bin once per tick into a per-cell **count**
(`mite_cnt[N_WORLD_CELLS]`, one byte) and a short **list** of the (≤`MITE_CAP`)
occupants (`mite_list[N_WORLD_CELLS * MITE_CAP]`, `uint16`). That turns "which mites
are within one cell of me" from a 1000×1000 scan into reading the ≤4 occupants of a
3×3 neighbourhood — `O(N)`. It is rebuilt **every tick** from current positions: this
is the **opposite end of the frequency-of-change spectrum** from the path tables,
which rebuild only on a (rare) wall edit. The page shows both ends side by side.

### The crowding cap — a rule, not a collision

**At most `MITE_CAP` (= 4) mite centres per cell, every tick.** This is a
*decision-level* rule, not physics: a mite that would step into a full cell picks a
different open neighbour with room, or holds (emits no drive) and re-evaluates next
tick. It is enforced **deterministically** by processing mites in index order against
a running per-cell tally — current occupants plus the moves already committed this
tick (in-transit reservations re-added first) — so the *n*-th mite sees the earlier
ones' commitments. The invariant `next occ[C] ≤ occ[C] + inbound[C] = tally[C] ≤ cap`
holds inductively: spawn places ≤4 per cell, and each tick gates only the cell a mite
*enters* (chapter 1's "test only the leading edge"). Pinned by a test (the cap holds
under forced convergence of the whole swarm on one cell).

### The shared knowledge — last-known-tank-position (the heart of the chapter)

*What is the minimal data the swarm's behaviour depends on?* One **record** per mite:
the **last known tank position** as a world cell (or `REC_EMPTY`), plus the **frame it
was recorded** (a timestamp). It is a **last-write-wins register**, and the swarm's
behaviour emerges entirely from copying it on contact. Per tick, for each mite:

- **Sense.** A tank within `mite_sense` cells (Chebyshev; a handful of tanks, scanned
  directly) → record its cell, stamped now, and **seek**.
- **Read a peer.** Otherwise adopt the **newest** record among the mites in the 3×3
  (read from the index), if it is strictly newer than yours — including an *empty* one
  (last-write-wins; lowest mite index breaks ties). On adopting, roll the seek die:
  **`mite_pseek`% (80) → seek**, else **wander** (but keep relaying the record).
- **Arrive & check.** A seeking mite within one cell of its recorded cell: if a tank is
  there, **refresh** (stamp now); if not, **erase** (empty, stamped now) and wander —
  "it's gone" is the newest record, so it propagates back by the same read rule.
- **Otherwise** keep the current record, mode, and target.

The gossip is **order-independent**: every mite reads last tick's records and writes
this tick's into a **second buffer**, then the buffers swap (double-buffered LWW). So
propagation never depends on iteration order, and timestamps are monotone.

### Movement: turning a mode into an input byte

`mites_step` unifies wander and seek as **"choose the next cell, then steer to it":**
wander picks a random open, non-full neighbour; seek picks the open, non-full
neighbour that most reduces the toroidal cell-distance to the recorded cell (greedy,
canonical N,E,S,W tie-break); if none qualifies, hold. "Current cell → chosen next
cell" becomes turn/drive bits exactly the way `tanks_path` derives a routing tank's
input (`at_cell_centre`, shared in `defs.h`): turn toward the cardinal, drive only
when the discrete heading is axis-aligned — so a mite tracks cells and clears the
one-cell inter-screen gaps.

### Determinism

All randomness — every wander direction and every seek die — comes from a **single
integer PRNG** (xorshift32) seeded in the `World` and advanced as mites are processed
in index order. The whole swarm is a **pure function of the seed and the inputs**:
same seed + same inputs ⇒ identical state, pinned by a state-hash test. The seed is
editable on the page (re-seed → re-scatter the swarm).

## Per-tick order (`sim.c`)

1. **Build the per-cell mite index** from current positions.
2. **Tank input** (`tanks_path` — routing tanks; controls for the MANUAL tank), unchanged.
3. **`mites_step`** — update records (double-buffered LWW), pick mode + next cell
   (cap-gated, in-order reservations), write `mite_in`; swap the record buffers.
4. **`agent_turn`** over the tanks (`turn_rate`) and over the mites (`mite_turn`).
5. **`agent_move`** over the tanks (`TANK_R`) and over the mites (`MITE_R`).
6. Advance the `World` frame counter (it is the record timestamp clock).

## Source layout

Reused from chapter 2 unchanged: `grid_paths.c`, `edge_paths.c`, `tanks_path.c`
(the tanks keep routing), `dirtab.c`, `escape_table.c`, `map_data.c`. Renamed and
generalised: `agent_turn.c` / `agent_move.c` (was `tanks_*`; `agent_move` takes a
`radius`), `collide.c` (`blocked_x`/`blocked_y` take a `radius`).

New in chapter 3:

| file | responsibility |
|------|----------------|
| `src/mites.c/.h` | the swarm: deterministic spawn, the per-cell index, and `mites_step` (record gossip + crowding cap + wander/seek → input), plus the xorshift32 PRNG |

`defs.h` adds the mite vocabulary (`N_MITES`, `MITE_R`, `MITE_CAP`, the modes, the
`REC_EMPTY` sentinel, world-cell + toroidal-distance helpers, and the shared
`at_cell_centre`). `sim.h`'s `World` gains the mite pool (SoA), the double-buffered
records, the per-cell index, the PRNG state, and the mite tunables. `render.c` draws
only the mites the camera shows (cost scales with visibility) tinted by belief, and
the minimap (`app.js`) shows the whole swarm as dots.

## Memory budget (static, no dynamic allocation)

The wasm linear memory is raised from chapter 2's **1 MiB to 2 MiB** — measured
high-water mark (`__heap_base`) is **~1.15 MiB (1,203,536 bytes)**, so 1 MiB no
longer holds it (the mite pool, its per-cell index, and the bigger instance buffer
push it over). Everything is still static, integer, allocation-free.

| data | size |
|------|------|
| Level-1 distances `l1dist[16 · 45150]` (inherited) | ~706 KB |
| Level-2 `dist2` + `nexthop` (inherited) | ~48 KB |
| mite per-cell index: `mite_cnt[4800]` (u8) + `mite_list[4800·4]` (u16) | ~43 KB |
| mite SoA: `xy`,`vxy`,`ang`,`in`,`hit`,`mode`,`cell`,`tgt` × 1000 | ~17 KB |
| records (double-buffered): `rec_cell` (u16) + `rec_time` (u32), ×2 | ~12 KB |
| `mites.c` BFS/scatter/tally scratch (file-static) | ~29 KB |
| instance buffer (2 screens + up to `N_MITES` mite quads) | ~43 KB |
| PRNG state, mite tunables, 128 KB stack | — |

`game.wasm` itself is ~21 KB of code. Each structure is sized to its real domain:
the index count is one byte (cap ≤ 4), the record is one cell + one timestamp, a mite
index in the list is a `uint16` (1000 < 0xFFFF), the cap tally is one byte.

## Build

Requires `clang` with the `wasm32` target and `wasm-ld` (LLVM ≥ 15). No emscripten.

```sh
./build.sh        # regenerates src/escape_table.c + src/map_data.c, writes game.wasm
```

`build.sh` runs `gen.sh` first (the host-baked escape table + world map, unchanged
from chapter 2), then compiles. `game.wasm` and the generated sources are committed
so GitHub Pages serves them without a build step.

## Testing

Because the simulation is separated from rendering and wasm, it builds and runs on
the host:

```sh
./test.sh         # compiles the sim core + src/test.c with host clang, runs it
./analyze.sh      # the inherited steer's sampled-cell / 16-pattern analysis
```

`src/test.c` covers **69 checks**: the inherited chapter-1/2 movement + pathing (a
regression after the rename — names changed, not behaviour); the **pool & index**
(counts sum to `N_MITES`, each mite in exactly its cell's list, spawn ≤4 on open
reachable cells); the **crowding cap** (≤4 every tick, naturally and under forced
convergence); the **gossip** (one-hop-per-tick propagation = order-independent, newer
overwrites older, older never overwrites newer, a newer *empty* propagates); the
**behaviour** (sense → record + seek, arrive-with-no-tank → erase + wander,
arrive-with-tank → refresh, the 80/20 seek split at the `P_SEEK` boundaries and
statistically); **wall safety** (a mite's `MITE_R` footprint never overlaps a wall
across a run); and **determinism** (same seed ⇒ identical state hash; a different seed
scatters differently; re-seeding is reproducible).

## Verified

- `./test.sh` passes (69 checks): the swarm/gossip/cap/determinism tests **and** the
  inherited chapter-1/2 movement + pathing after the rename.
- The baked escape table is **byte-identical** to chapter 2 — proving the
  radius-parameterised collision changed names, not geometry.
- `game.wasm` exercised in Node (init, 400+ ticks driving a tank through the swarm,
  the per-cell occupancy and belief-field reads, the editable tunables and re-seed,
  wall toggle + tank routing) — identical to native, as they share `sim.c`.
- **Not** verified here: the live WebGPU render and on-screen touch — no GPU/browser
  in the build environment. The host uses standard WebGPU calls; render-test in a
  browser.

## Deferred (not built — no speculative generality)

Spawning waves and despawn (a free-list / generational handles over the pool); mites
damaging or being damaged by tanks; mite-vs-mite or mite-vs-tank physical collision;
mites using the path tables for smarter routing; more than one faction;
flocking/alignment beyond the cap. Each is its own future step.
