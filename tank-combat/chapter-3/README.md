# Tank Combat — Chapter 3: The Swarm

Chapter 3 of *Data-Oriented Design, by picture* (the book index is one level up).
It adds an **enemy faction** to [chapter 2](../chapter-2/)'s 4×4 world: **a thousand
mites** that wander, copy the last place they saw a tank on contact, and converge on
it. **The point of the project is to demonstrate a data-oriented approach** — here
the lesson is **scale**: one entity is easy, a thousand is a *data layout*. The
volume forces the structures volume needs — a fixed pool with no allocation, a
per-cell spatial index rebuilt every tick, a deterministic integer PRNG, a piece of
shared knowledge that is just **timestamped data copied on contact**, and — the
payoff — **a handful of shared route fields** standing in for a thousand per-unit
routes.

Chapter 3 is an *increment* on chapter 2: it reuses the whole sim core (fixed-point
types, the screen-major bitset grid, the movement transforms, the baked trig and
escape tables, **the two-level path tables — now read by the mites too**, the
sim/render/wasm split, version stamping, the native test harness).

Built following Mike Acton's data-oriented design rules:
[**data-oriented-design.md**](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md).

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge). The
**mites run themselves**. The four tanks are still yours: **tap a tank** to cycle it
*auto-path* (tap a cell to route it there) → *manual* (drive it, `W`/`S`/`A`/`D` or
the pad) → *unselected*. Drive a tank through the swarm and watch a sighting spread,
then 80% **hunt** it (red) while 20% carry it **home** to a nest (the nest's colour);
mites with no record **wander** (teal).

## The faction: mites

A **mite** is a **quarter-cell driven body** (`MITE_R = SUB/8 = 32` subcells, so its
diameter is about a quarter cell) on the *same* grid as the tanks. There are exactly
`N_MITES = 1000`, spawned at init and alive for the whole chapter — a **fixed pool**:
flat structure-of-arrays, statically sized, **no allocation**. A mite shares the
tanks' turn and move transforms and collides only with **walls** (its own smaller
radius); it does **not** physically collide with tanks or with other mites. Where it
has a destination it **navigates with the path tables** (via a shared route field,
below) rather than owning a route; with no destination it **wanders**.

Every mite belongs to one of **`NEST_COUNT = 4` nests** (`nest_of(i) = i % 4`), four
fixed open cells spread across the world (editable on the page). A nest is a home a
mite can path back to.

### One movement model, sized by a parameter

The tanks and the mites are both **driven bodies on the grid** — a position, a
heading, an input byte, a collision flag — so they share the turn and move
transforms. Chapter 2's `tanks_turn`/`tanks_move` are renamed **`agent_turn`/`agent_move`**,
and the collision half-extent is a **`radius` parameter** threaded into
`blocked_x`/`blocked_y`: tanks pass `TANK_R`, mites pass `MITE_R`. **Size is data you
pass, not a reason to fork the transform.** The auto-steer keys on a cell's
4-neighbour wall pattern (size-independent), so the baked escape table is unchanged
and byte-identical to chapter 2. The **input producers stay separate**: `tanks_path`
produces the tanks' routing input; `mites.c` produces the mites' — both write an
input byte that flows through the same `agent_turn`/`agent_move`.

### The per-cell index — the grid you already have is the acceleration structure

We bin once per tick into a per-cell **count** (`mite_cnt[N_WORLD_CELLS]`, one byte)
and a short **list** of the (≤`MITE_CAP`) occupants (`mite_list[...]`, `uint16`), so
"which mites are within one cell of me" is reading the ≤4 occupants of a 3×3
neighbourhood — `O(N)`. It is rebuilt **every tick** from current positions: the
**opposite end of the frequency-of-change spectrum** from the path tables, which
rebuild only on a (rare) wall edit. The page shows both ends side by side.

### The crowding cap — a rule, not a collision

**At most `MITE_CAP` (= 4) mite centres per cell, every tick**, a *decision-level*
rule (not physics). The cap gates the actual step in **every** mode: a mite takes its
preferred next cell only if it is open and not full; else the best open, non-full
neighbour toward its destination (ranked by the field's distance); else it holds. It
is enforced **deterministically** by processing mites in index order against a
running tally (current occupants + committed inbound). The invariant
`next occ[C] ≤ occ[C] + inbound[C] = tally[C] ≤ cap` holds inductively (spawn ≤4/cell,
gate only the entering edge). Pinned by tests, including a forced-convergence stress.

### The shared knowledge — last-known-tank-position (the heart of the chapter)

One **record** per mite: the **last known tank position** as a world cell (or
`REC_EMPTY`), plus the **frame it was recorded** (a timestamp). A **last-write-wins
register**; the swarm's behaviour emerges entirely from copying it on contact. Per
tick, for each mite (`mites_records`):

- **Sense.** A tank within `mite_sense` cells (Chebyshev) → record its cell, stamped
  now, and **hunt** it. A self-sensed tank is always hunted.
- **Read a peer.** Otherwise adopt the **newest** record among the mites in the 3×3
  (read from the index), if strictly newer — including an *empty* one (LWW, lowest
  index breaks ties). On adopting, roll the **role die**: `mite_phunt`% (80) → **hunt**
  the recorded cell, else → **home** (path to this mite's nest). It fires **even while
  already hunting/homing** — a newer record interrupts and re-rolls. Either way the
  mite keeps and relays the record; an adopted *empty* record means **wander**.
- **Arrive (hunt).** A hunting mite within one cell of its recorded cell: tank there →
  **refresh** (stamp now); gone → **erase** (empty, stamp now) and wander. The
  empty-stamped-now record is newest, so "it's gone" propagates back.
- **Arrive (home).** A homing mite that reaches its nest idles there (a small local
  wander) until a newer record interrupts it.

The gossip is **order-independent**: every mite reads last tick's records and writes
this tick's into a **second buffer**, then they swap (double-buffered LWW). Timestamps
are monotone.

### Navigation — a handful of shared route fields for a thousand mites

A **hunting** or **homing** mite has a **destination cell** (its recorded tank cell,
or its nest). It does not compute its own route: it reads a **shared route field** for
that destination — the same Level-1/2 composition the tanks use, with the
remaining-distance vector (`pg`) **keyed by destination cell** instead of by tank.
`edge_paths.c` is generalised to `pg_fold` / `route_next_dir_pg` / `route_remaining_pg`
over an arbitrary `(destination, pg)`; the tank entry points are thin wrappers, so the
tanks path exactly as before and the field *is* the tank route keyed by destination
(a test pins the equivalence).

Because the swarm shares destinations, the set of *distinct* active destinations is
small — the 4 nests plus the recent sighting cells the gossip converges on — even
though 1000 mites move. So a fixed table of `N_FIELDS` fields, keyed by destination:

- The **4 nest fields stay resident** (re-folded only on a wall edit / nest move).
- A **tank-goal field is folded the first tick its cell becomes an active
  destination and reused while active**; its slot is freed once no mite wants it.
- **`N_FIELDS` is sized from a measured peak, not a guess.** Instrumenting the distinct
  active-destination count across representative scenarios (idle / roaming / fleeing
  tanks, several simultaneous sightings) puts the high-water mark at **~22**, so
  `N_FIELDS = 64` gives ~3× headroom — the prove/measure/detect discipline the Level-1
  byte got, applied to a runtime peak. The page shows the live **active / 64** count
  and the running **peak**, and a test asserts the peak stays within `N_FIELDS`.
- **Overflow → greedy fallback.** If the live count ever exceeds `N_FIELDS`, those
  mites steer greedily (toroidal distance) that tick — no crash, no cap break (tested).

### Determinism

All randomness — every wander direction and every hunt/home roll — comes from a
**single integer PRNG** (xorshift32) seeded in the `World` and advanced as mites are
processed in index order. The whole swarm is a **pure function of the seed and the
inputs**: same seed + same inputs ⇒ identical state, pinned by a state-hash test
(including under overflow). The seed and the nest positions are editable on the page.

## Per-tick order (`sim.c`)

1. **Build the per-cell mite index** from current positions.
2. **Tank input** (`tanks_path`; controls for the MANUAL tank), unchanged.
3. **`mites_records`** — gossip the record (double-buffered LWW); each mite's mode +
   destination fall out; swap the record buffers.
4. **`mites_update_fields`** — collect the distinct active destinations (4 nests + the
   hunters' cells), fold a route field for each new one, reuse resident, free dropped.
5. **`mites_step`** — the crowding cap (in-order reservation) + steer each mite along
   its field (or greedy on overflow, or wander); write `mite_in`.
6. **`agent_turn`** over the tanks (`turn_rate`) and the mites (`mite_turn`).
7. **`agent_move`** over the tanks (`TANK_R`) and the mites (`MITE_R`).
8. Advance the `World` frame counter (the record timestamp clock).

## Source layout

Reused from chapter 2: `grid_paths.c`, `tanks_path.c`, `dirtab.c`, `escape_table.c`,
`map_data.c`. Renamed/generalised: `agent_turn.c` / `agent_move.c` (was `tanks_*`;
`agent_move` takes a `radius`); `collide.c` (`blocked_*` take a `radius`);
`edge_paths.c` (pathing generalised to destination-keyed `pg_fold` /
`route_next_dir_pg` / `route_remaining_pg`, with tank wrappers).

New in chapter 3:

| file | responsibility |
|------|----------------|
| `src/mites.c/.h` | the swarm: spawn, the per-cell index, the record gossip, the shared route-field table, and the cap-gated movement (hunt/home via fields, greedy on overflow, wander), plus the xorshift32 PRNG and the nests |

`defs.h` adds the mite vocabulary (`N_MITES`, `MITE_R`, `MITE_CAP`, `NEST_COUNT`,
`nest_of`, `N_FIELDS`, the modes, `REC_EMPTY`, world-cell + toroidal-distance helpers,
the shared `at_cell_centre`). `sim.h`'s `World` gains the mite pool (SoA, incl.
`mite_dest`), the double-buffered records, the per-cell index, the four nests, the
shared route-field table, the field active/peak counters, the PRNG, and the mite
tunables. `render.c` draws only the mites the camera shows (cost scales with
visibility), tinted by role (wander/hunt/home-by-nest), plus the four nest markers;
the minimap (`app.js`) shows the whole swarm and the nests.

## Memory budget (static, no dynamic allocation)

The wasm linear memory is **2 MiB** (raised from chapter 2's 1 MiB in the first
increment); measured high-water mark (`__heap_base`) ≈ **1.16 MiB**. Everything is
static, integer, allocation-free.

| data | size |
|------|------|
| Level-1 distances `l1dist[16 · 45150]` (inherited) | ~706 KB |
| Level-2 `dist2` + `nexthop` (inherited) | ~48 KB |
| mite per-cell index: `mite_cnt[4800]` (u8) + `mite_list[4800·4]` (u16) | ~43 KB |
| mite SoA: `xy`,`vxy`,`ang`,`in`,`hit`,`mode`,`dest`,`cell`,`tgt` × 1000 | ~19 KB |
| records (double-buffered): `rec_cell` (u16) + `rec_time` (u32), ×2 | ~12 KB |
| shared route fields: `field_pg[64 · 128]` (u16) + `field_dest[64]` | ~16 KB |
| `mites.c` BFS/scatter/tally/dest-mark scratch (file-static) | ~34 KB |
| instance buffer (2 screens + up to `N_MITES` mite quads + nests) | ~43 KB |
| 4 nests, PRNG state, mite tunables, 128 KB stack | — |

Each structure is sized to its real domain: the index count is one byte (cap ≤ 4),
the record is one cell + one timestamp, a mite index is a `uint16` (1000 < 0xFFFF),
the route table is sized to a *measured* peak, the cap tally is one byte.

## Build

Requires `clang` with the `wasm32` target and `wasm-ld` (LLVM ≥ 15). No emscripten.

```sh
./build.sh        # regenerates src/escape_table.c + src/map_data.c, writes game.wasm
```

`game.wasm` and the generated sources are committed so GitHub Pages serves them
without a build step.

## Testing

Because the simulation is separated from rendering and wasm, it builds and runs on
the host:

```sh
./test.sh         # compiles the sim core + src/test.c with host clang, runs it
./analyze.sh      # the inherited steer's sampled-cell / 16-pattern analysis
```

`src/test.c` covers **78 checks**: the inherited chapter-1/2 movement + pathing (a
regression after the `agent_*` rename and the `edge_paths` generalisation — names and
shape changed, not behaviour); the **pool & index**; the **crowding cap** (every tick,
naturally and under forced convergence); the **gossip** (one-hop-per-tick propagation
= order-independent, newer overwrites older, older never overwrites newer, a newer
*empty* propagates); the **behaviour** (sense → hunt, arrive erase/refresh, the 80/20
hunt/home split at the `P_HUNT` boundaries and statistically, a newer record
interrupts a homing mite and re-rolls); the **nests & shared fields** (`nest_of`
partitions the swarm into four; a homing mite reaches its nest; a hunting mite reaches
its recorded cell; **a mite's shared-field route equals the tank pathing ground truth**
for the same destination; the **distinct-destination peak is measured and stays within
`N_FIELDS`**; a forced overflow falls back to greedy without breaking the cap or
determinism); **wall safety** at `MITE_R`; and **determinism**.

## Verified

- `./test.sh` passes (78 checks): the swarm/gossip/cap/nests/fields/overflow/
  determinism tests **and** the inherited chapter-1/2 movement + pathing.
- The shared route field gives **byte-for-byte the same route as the tank pathing**
  for the same destination (the field is the tank route keyed by destination), and the
  baked escape table is byte-identical to chapter 2.
- `game.wasm` exercised in Node (init, ticks with driven tanks, the per-cell
  occupancy / belief / route-field counters, the editable tunables / seed / nests) —
  identical to native, as they share `sim.c`.
- **Not** verified here: the live WebGPU render and on-screen touch — no GPU/browser
  in the build environment. Render-test in a browser.

## Deferred (not built — no speculative generality)

Spawning waves and despawn (a free-list / generational handles over the pool); mites
damaging or being damaged by tanks; mite-vs-mite or mite-vs-tank physical collision;
more than one faction; flocking/alignment beyond the cap; **per-mite route fields**
(the experiment shares a few instead). Each is its own future step.
