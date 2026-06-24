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
mites with no record **wander** (teal). Each tank's **turret** also aims at the nearest
mite it can see and **fires** (2 shots/sec): one shot kills the mite — it revives at its
nest after 5 s — and every nearby mite hears the **death cry** and turns to hunt the
shooter, so thinning the swarm also enrages it.

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

We bin once per tick into a per-cell **count** (`mite_cnt[N_WORLD_CELLS]`, one byte =
number of occupied sub-segments) and a **sub-segment-slotted list** (`mite_list[cell*4 +
seg]` = the mite in that sub-segment, or `REC_EMPTY`), so "which mites are within one
cell of me" is reading the ≤4 occupants of a 3×3 neighbourhood — `O(N)`. It is rebuilt
**every tick** from current positions: the **opposite end of the frequency-of-change
spectrum** from the path tables, which rebuild only on a (rare) wall edit.

### The crowding cap — a sub-segment rule, not a collision

Each cell is quartered into **`MITE_CAP` (= 4) sub-segments** (a 2×2 grid). A mite is
assigned to one by index — `seg_of(m) = (m >> 2) & 3`, the upper bits **not** `m & 3`
(that is `nest_of`, so reusing it would make every mite of a nest want the same
sub-segment of its one nest cell — serializing revival to one at a time) — and **parks at
that sub-segment's centre** (a quarter-cell off the cell centre), so the swarm spreads out
inside cells instead of stacking on the centre (and is a little harder to mow down in a
single line).
The cap is **at most one mite per (cell, sub-segment)**: a mite enters a cell only if its
own sub-segment there is free (and the cell holds fewer than `mite_cap` segments total) —
a *decision-level* rule, not physics. It is enforced **deterministically** by processing
mites in index order against a per-cell **bitmask** of reserved sub-segments (current
occupants + committed inbound, each a `1 << seg`). Pinned by tests, including a
forced-convergence stress and the one-mite-per-sub-segment invariant.

**Jam detector (give up if stuck).** The cap can deadlock a *knot* of mites all wanting
the same exits — most visibly a cluster of revived hunters funnelling out of one nest, or
homers blocked from a full nest cell. So a **hunting or homing** mite that cannot advance
toward its destination for `MITE_STUCK_MAX` (= 16) consecutive ticks **gives up and
wanders**, and the knot dissolves instead of freezing. It is self-targeting: a mite making
any progress resets its counter, and a mite stuck *at the front* (against a tank) re-senses
the tank the very next tick and resumes — so the attack holds while dead-end pile-ups clear.
Without it the swarm collapses into permanent green blobs around the nests (measured: ~170
hunters frozen at the nests and climbing; with it, that knot churns out and the wanderer
population recovers from ~14 to ~180).

### The shared knowledge — last-known-tank-position (the heart of the chapter)

One **record** per mite, a **last-write-wins register** packed into a `uint16` cell +
a `uint32` timestamp. The cell is **typed** (the top bit, since a cell index needs only
13 of 16 bits — see `defs.h`):

- a **sighting** `X` — "a tank is at X",
- a **gone** `X | GONE_FLAG` — "cell X is empty" (a dispersal broadcast),
- **no info** (`REC_EMPTY`).

The swarm's behaviour emerges from copying records on contact. Per tick, for each mite
(`mites_records`):

- **Sense.** A tank within `mite_sense` cells (Chebyshev) → record a **sighting**, stamped
  now, and **hunt** it. A self-sensed tank is always hunted.
- **Read a peer.** Otherwise adopt the **newest** qualifying record in the 3×3 (read from
  the index), if strictly newer (LWW, lowest index breaks ties). A **sighting** qualifies
  for anyone. A **gone-of-X** qualifies **only for a mite that is hunting X** — so "X is
  empty" reaches X's hunters and no one else. On adopting a sighting, roll the **role
  die**: `mite_phunt`% (80) → **hunt** the cell, else → **home** (carry it to the nest);
  it re-rolls **even while already hunting/homing**. On adopting a gone-of-X, **stand down
  to wander** (and relay it onward to the rest of the X-cluster).
- **Arrive (hunt).** A hunting mite within one cell of its recorded cell: tank there →
  **refresh** (stamp now, keep hunting); gone → **broadcast a gone-of-X** (stamped now)
  and **wander**.
- **Arrive (home).** A homing mite that reaches its nest **deposits its sighting in the
  nest if it is newer** than what the nest holds (see *the nest as a hub* below), then
  drops the record and reverts to wander.

> **Why the record is typed — the swarm used to "forget" a tank that never moved.** An
> earlier version stored an untyped cell and erased it to a fresh-stamped *empty* on
> arrival. That empty was the newest record in the system, so it propagated like any other
> and a single stale "gone" swept the whole swarm back to wander — a tank parked away from
> a nest was wiped to green within seconds (confirmed in simulation). Typing the record
> fixes it: a **gone-of-X** is adopted **only by mites already hunting X**, so it disperses
> a stale cluster off X (when a tank actually leaves, its hunters clear within a couple of
> seconds) **without ever wiping a live sighting of some other cell**. A moving tank is
> tracked the whole time, because each new cell is a newer *sighting* that supersedes the
> old via ordinary last-write-wins.

The gossip is **order-independent**: every mite reads last tick's records and writes
this tick's into a **second buffer**, then they swap (double-buffered LWW). Timestamps
are monotone.

### The nest as a knowledge hub

Each nest keeps its own record (`nest_rec_cell`/`nest_rec_time`) — the latest tank
**sighting** carried home. Two flows feed it and a timer drains it:

- **Deposit.** A homing mite (the `1 − mite_phunt` ≈ 20% that carry a contact home) that
  reaches its nest overwrites the nest's record **iff its own sighting is newer**
  (newest-wins, like the per-mite register). Only sightings are ever deposited — a courier
  never *erases* the nest, so a "gone" can't wipe the hub's knowledge.
- **Dispatch.** A mite **revived** at the nest **adopts the nest's record**: it hunts the
  last-known tank cell (reinforcements to the front), or just wanders if the nest knows
  nothing. So killing mites doesn't reset the swarm's knowledge — the nest remembers it
  and seeds the next wave.
- **Timeout.** Because the deposit flow can't erase the nest, a nest sighting that no
  courier refreshes would otherwise re-seed hunters at a ghost forever. So a nest record
  older than `nest_ttl` (default 600 ticks = 10 s; `0` = never) simply **expires**. That
  is the nest's only forgetting mechanism — a stale entry costs at most `nest_ttl` of
  wasted reinforcements before it clears (editable live on the page).

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
(including under overflow, and with combat on). The seed and the nest positions are
editable on the page.

### Combat — the tanks shoot back (`tanks_fire.c`)

Combat is its own transform that **leans on the structures already here** — the per-cell
index, the record gossip, the nests — adding only a small fixed ring of cosmetic
destruction bursts. Per tank, each tick:

- **Aim.** The turret aims **independently of the body** (`tank_turret`, separate from
  the movement heading `tank_ang`) and **turns at its own rate** (`turret_rate`, default
  ~8 ticks/90°) — it can't snap. So it targets the mite **most likely to be hit: the one
  closest to the turret's current direction** (least rotation to bring the barrel onto
  it), not the spatially nearest. It scans the per-cell index over its search box, takes
  one **line-of-sight** test per cell (an integer Bresenham walk over the wall grid, the
  whole cell in or out), and keeps the smallest bearing change (lowest index breaks ties).
  The turret then swings toward that bearing at its turn rate.
- **Fire (a laser).** Once the turret has swung **exactly** onto the target bearing (so
  the beam runs precisely along the barrel — a line shot can't fire in a cone) and the
  cooldown is elapsed, fire on a fixed rate (`fire_period`, default 30 ticks = **2/sec**;
  `0` = aim-only). The shot is a **laser**: the same Bresenham walk run *outward* from the
  tank until it meets a wall, **destroying every mite in the cells it crosses** (`LASER_MAX`
  caps the length). Each destroyed mite is marked dead (`mite_resp` = the respawn timer),
  drops out of the index next rebuild (never drawn — minimap included — gossiped, or
  targeted), and spawns a **destruction burst**. The beam is drawn for `LASER_TICKS` (~0.1 s),
  during which the **turret is locked** to its firing direction — a shot commits the aim for
  its whole duration; the turret is free to track again only once the beam fades.
- **Death cry.** Each kill writes the **firing tank's cell** into the record of every live
  mite within **2× sensing range** of the destroyed one (stamped now, mode → hunt), through
  the ordinary record buffer — so a laser that mows a line broadcasts the shooter's position
  from every body it drops, into the same gossip that spreads any sighting, and the
  survivors converge on the attacker.

A dead mite **revives near its nest** after `mite_respawn` ticks (default 300 = **5 s**),
**adopting the nest's gossip** (hunt the last-known tank cell, or wander if the nest knows
nothing — see *the nest as a hub*). Revival respects the crowding cap (a free
`(cell, sub-segment)` slot once current occupancy **and inbound reservations** are counted,
the same tally `mites_step` uses). But the nest *cell itself* sits on the swarm's hunt
routes and is usually full of passing hunters — so revival searches an **expanding ring**
around the nest (`MITE_REVIVE_R = 3`: the nest cell first, then outward) for a free slot,
rather than waiting on the one cell. Without the ring the respawn queue **jams**: once the
swarm reliably hunts (the typed-gossip fix), hunters camp the nest cells, dead mites can't
revive, and the live count bleeds away (measured: ~480 alive and falling vs ~925 stable
with the ring). Reviving happens right after the index is built (step 1b) so the revived
mite is an ordinary live mite for the rest of the tick. The **four nests sit off the tanks'
start screen** (nest 0 moved to screen (1,1)), so revived mites don't pop up under a barrel.
Firing draws **no randomness**, so the swarm stays a pure function of the seed and inputs.

## Per-tick order (`sim.c`)

1. **Build the per-cell mite index** from current positions.
   1b. **`mites_respawn`** — revive timed-out dead mites at their nests, into the
   freshly-built index (cap-respecting), so they are ordinary live mites this tick.
2. **Tank input** (`tanks_path`; controls for the MANUAL tank), unchanged.
3. **`mites_records`** — gossip the record (double-buffered LWW); each mite's mode +
   destination fall out; swap the record buffers.
4. **`mites_update_fields`** — collect the distinct active destinations (4 nests + the
   hunters' cells), fold a route field for each new one, reuse resident, free dropped.
5. **`mites_step`** — the crowding cap (in-order reservation) + steer each mite along
   its field (or greedy on overflow, or wander); write `mite_in`.
   5b. **`tanks_fire`** — swing each turret onto the most-hittable mite, fire the laser on
   cooldown (line-kill to the wall + bursts + death cry); age the effect ring (revival
   already happened in 1b).
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
| `src/tanks_fire.c/.h` | combat: per-tank turret aim (turns at a rate toward the most-hittable mite — closest to the turret's bearing — via a box scan + Bresenham line of sight), the fixed-rate **laser** (a Bresenham beam to the nearest wall destroying every mite on the line) + destruction bursts, the death-cry gossip, and the cap-respecting nest respawn |

`defs.h` adds the mite vocabulary (`N_MITES`, `MITE_R`, `MITE_CAP`, `NEST_COUNT`,
`nest_of`, `N_FIELDS`, the modes, `REC_EMPTY`, world-cell + toroidal-distance helpers,
the shared `at_cell_centre`) and the combat vocabulary (`TARGET_MAX_R`, `TGT_NONE`,
`LASER_MAX`, `LASER_TICKS`, `N_FX`, `FX_DURATION`).
`sim.h`'s `World` gains the mite pool (SoA, incl. `mite_dest` and `mite_resp`), the
double-buffered records, the per-cell index, the four nests **and their stored gossip**
(`nest_rec_cell`/`nest_rec_time`), the shared route-field
table, the field active/peak counters, the per-tank turret/cooldown/target/beam state,
the **destruction-effect ring** (`fx_xy`/`fx_t`), the PRNG, and the mite + combat
tunables (`fire_period`, `mite_respawn`, `turret_rate`).
`render.c` draws only the mites the camera shows (cost scales with visibility), tinted
by role (wander/hunt/home-by-nest), skips dead mites, draws each tank's turret on
`tank_turret` (separate from the body), the **laser beam** (glow + core) and the
**destruction bursts**, plus the four nest markers; the minimap (`app.js`) shows the
whole live swarm and the nests (it skips the dead too).

## Memory budget (static, no dynamic allocation)

The wasm linear memory is **2 MiB** (raised from chapter 2's 1 MiB in the first
increment); measured high-water mark (`__heap_base`) ≈ **1.04 MiB** (1,093,584 bytes).
Everything is static, integer, allocation-free.

| data | size |
|------|------|
| Level-1 distances `l1dist[16 · 45150]` (inherited) | ~706 KB |
| Level-2 `dist2` + `nexthop` (inherited) | ~48 KB |
| mite per-cell index: `mite_cnt[4800]` (u8) + `mite_list[4800·4]` (u16) | ~43 KB |
| mite SoA: `xy`,`vxy`,`ang`,`in`,`hit`,`mode`,`dest`,`cell`,`tgt`,`resp` × 1000 | ~21 KB |
| records (double-buffered): `rec_cell` (u16) + `rec_time` (u32), ×2 | ~12 KB |
| shared route fields: `field_pg[64 · 128]` (u16) + `field_dest[64]` | ~16 KB |
| combat: per-tank turret/cooldown/target/beam (×4) + effect ring `fx_xy`/`fx_t`[256] | ~2 KB |
| `mites.c` BFS/scatter/tally/dest-mark scratch (file-static) | ~34 KB |
| instance buffer (2 screens + up to `N_MITES` mite quads + nests) | ~43 KB |
| 4 nests, PRNG state, mite + combat tunables, 128 KB stack | — |

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

`src/test.c` covers **121 checks**: the inherited chapter-1/2 movement + pathing (a
regression after the `agent_*` rename and the `edge_paths` generalisation — names and
shape changed, not behaviour); the **pool & index** (each mite in its own (cell,
sub-segment) slot); the **crowding cap** (every tick, naturally, under forced
convergence, **and with firing on** — kills + nest respawns; ≤1 mite per sub-segment);
the **gossip** (one-hop-per-tick propagation = order-independent, newer overwrites
older, older never overwrites newer, a newer no-info record does **not** overwrite a
sighting); the **behaviour** (sense → hunt, arrive refresh / gone-of-X broadcast, the
80/20 hunt/home split at the `P_HUNT` boundaries and statistically, a newer sighting
interrupts a homing mite and re-rolls, **a gone-of-X clears an X-hunter but never a
Y-hunter**, a mite that reaches its nest reverts to wander); **nest gossip** (a homing
mite deposits its newer sighting; older gossip doesn't overwrite; an empty courier never
erases the nest; **a nest sighting expires past `nest_ttl`** and is kept within it; a
revived mite adopts the nest's record and hunts it, or wanders if the nest is empty); the
**nests & shared fields** (`nest_of` partitions the swarm into four; a homing mite
reaches its nest; a hunting mite reaches its recorded cell; **a mite's shared-field
route equals the tank pathing ground truth** for the same destination; the
**distinct-destination peak is measured and stays within `N_FIELDS`** — ~32 with combat
on; a forced overflow falls back to greedy without breaking the cap or determinism);
**combat** (the turret acquires a mite in line of sight and the **laser destroys** it,
spawning a **burst**; it leaves the index; the **laser is a thin line** — mites on it die,
mites off it (spread into other sub-segments) **dodge**; the **beam stops at a wall** — a
collinear mite beyond it survives; a wall
blocks line of sight; the turret **swings at a turn rate** and does not snap; targeting
picks the mite **closest to the turret's direction**, not the spatially nearest; the
**turn rate gates firing** — an off-axis target isn't shot until the turret swings on; the
**turret is locked while the beam is live** and free again once it fades; the
death cry teaches a nearby mite the shooter's cell and flips it to hunt; a destroyed mite
revives **at or next to its nest** after the timeout, and **into the neighbourhood when the
nest cell is full**); **wall safety** at
`MITE_R`; and **determinism** (the swarm state hash **and** the tank combat-state hash,
with firing on by default).

## Verified

- `./test.sh` passes (121 checks): the swarm/gossip/cap/nests/fields/overflow/combat/
  determinism tests **and** the inherited chapter-1/2 movement + pathing.
- The shared route field gives **byte-for-byte the same route as the tank pathing**
  for the same destination (the field is the tank route keyed by destination), and the
  baked escape table is byte-identical to chapter 2.
- The baked map is connected: the generator's self-check passes and screen (0,3) is
  **island-free** (the world drops from 93 island cells to 7 — only screens (3,0) and
  (2,2) keep their small deliberate pockets).
- `game.wasm` exercised in Node (init, ticks with driven tanks, the per-cell
  occupancy / belief / route-field counters, the editable tunables / seed / nests, and
  the **combat exports** — `fire_period`/`mite_respawn`/`turret_rate` get+set,
  `mite_resp_ptr`) and the page booted under a headless DOM/WebGPU stub (stats render the
  update-time + the alive/dead split) — identical to native, as they share `sim.c`.
- **Not** verified here: the live WebGPU render and on-screen touch — no GPU/browser
  in the build environment. Render-test in a browser.

## Deferred (not built — no speculative generality)

Spawning waves and despawn (a free-list / generational handles over the pool); mites
damaging the tanks back (tanks kill mites, but take no damage — tank health/score is a
future step); mite-vs-mite or mite-vs-tank physical collision; projectiles as bodies
(the laser is hitscan — an instant beam to the wall, not a travelling projectile); more than
one faction; flocking/alignment beyond the cap; **per-mite route fields** (the
experiment shares a few instead). Each is its own future step.
