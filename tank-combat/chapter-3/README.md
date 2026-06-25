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
then **hunt** it (red) — by default every adopter hunts; lower the **P(hunt)** tunable to
peel off a fraction that carry it **home** to a nest (the nest's colour). Mites with no
record **wander** (teal). Each tank's **turret** also aims at the nearest
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

Every mite belongs to one of **`NEST_COUNT = 15` nests** (`nest_of(i) = i % 15`) —
**one per screen except the tanks' start screen (0,0)**, each on the open cell nearest
its screen centre. A nest is a home a mite can path back to. Spreading the homes over
fifteen screens (rather than crowding a few) spreads the spawn/revival load across
fifteen screens' worth of exits instead of piling it out through one screen's two or
three border openings — which is what let the revival logic stay dead simple (revive,
wander, re-acquire) with no jam to dissolve.

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
assigned to one by index — `seg_of(m) = (m >> 2) & 3`, **decoupled** from `nest_of(m) =
m % 15` (15 is not a multiple of 4, so a nest's members fan out across all four
sub-segments instead of all wanting the same one — which would serialize revival to one
at a time) — and **parks at
that sub-segment's centre** (a quarter-cell off the cell centre), so the swarm spreads out
inside cells instead of stacking on the centre (and is a little harder to mow down in a
single line).
The cap is **at most one mite per (cell, sub-segment)**: a mite enters a cell only if its
own sub-segment there is free (and the cell holds fewer than `mite_cap` segments total) —
a *decision-level* rule, not physics. It is enforced **deterministically** by processing
mites in index order against a per-cell **bitmask** of reserved sub-segments (current
occupants + committed inbound, each a `1 << seg`). Pinned by tests, including a
forced-convergence stress and the one-mite-per-sub-segment invariant.

### The shared knowledge — last-known-tank-position (the heart of the chapter)

One **record** per mite: the **last known tank position** as a world cell (or
`REC_EMPTY`), plus the **frame it was recorded** (a timestamp). A **last-write-wins
register**; the swarm's behaviour emerges entirely from copying it on contact. Per
tick, for each mite (`mites_records`):

- **Sense.** A tank within `mite_sense` cells (Chebyshev) → record its cell, stamped
  now, and **hunt** it. A self-sensed tank is always hunted.
- **Read a peer.** Otherwise adopt the **newest** record among the mites in the 3×3
  (read from the index), if strictly newer — including an *empty* one (LWW, lowest
  index breaks ties). On adopting, roll the **role die**: `mite_phunt`% → **hunt** the
  recorded cell, else → **home** (path to this mite's nest). `mite_phunt` **defaults to
  100** (every adopter hunts — no couriers; lower it to send a fraction home). It fires
  **even while already hunting/homing** — a newer record interrupts and re-rolls. Either
  way the mite keeps and relays the record; an adopted *empty* record means **wander**.
- **Arrive (hunt).** A hunting mite within one cell of its recorded cell: tank there →
  **refresh** (stamp now); gone → **erase** (empty, stamp now) and wander. The
  empty-stamped-now record is newest, so "it's gone" propagates back.
- **Arrive (home).** A homing mite that reaches its nest has delivered the sighting:
  it **drops the record and reverts to wander** (empty, stamp now), rejoining the swarm.
  So a mite is nest-tinted only while *in transit*; without this, homed mites pile up at
  the nests, stay `MM_HOME` forever (perpetually nest-coloured), and relay a stale record.

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
small — the 15 nests plus the recent sighting cells the gossip converges on — even
though 1000 mites move. So a fixed table of `N_FIELDS` fields, keyed by destination:

- The **15 nest fields stay resident** (re-folded only on a wall edit).
- A **tank-goal field is folded the first tick its cell becomes an active
  destination and reused while active**; its slot is freed once no mite wants it.
- **`N_FIELDS` is sized from a measured peak, not a guess.** Instrumenting the distinct
  active-destination count puts the high-water mark at **~19** in normal play (the 15
  resident nests plus the few sighting cells the gossip has converged on), so
  `N_FIELDS = 64` holds it with headroom — the prove/measure/detect discipline the Level-1
  byte got, applied to a runtime peak. The page shows the live **active / 64** count
  and the running **peak**, and a test asserts the peak stays within `N_FIELDS`.
- **Overflow → greedy fallback.** If the live count ever exceeds `N_FIELDS`, those
  mites steer greedily (toroidal distance) that tick — no crash, no cap break (tested).

### Flocking — the fallback step (`mites.c`)

The step itself splits on one question: *is the mite cleanly pathing?* If it is hunting
or homing **and** the route field's preferred next sub-segment is free, it takes that —
the field step, as before. In **every other case** — the preferred sub-segment is
blocked, or the mite is wandering — it does **basic flocking** instead of the old
random/greedy fallback: it samples the live mites in its 3×3 (the same per-cell index),
and votes a cardinal from the three boids rules — **cohesion** (toward them),
**separation** (off the ones within a cell), and **alignment** (their heading) — then
takes the best **cap-free** cardinal; a mite with no neighbours falls back to a random
wander step. It is all integer and deterministic (no new RNG draws when neighbours
exist). Weights are `FLOCK_{COH,SEP,ALI}` (= 1/2/4): **alignment-dominant**, because
strong cohesion just re-bunches — at 1/2/4 the swarm **flows in streams**. Two effects
fall out, measured over a 4-tank camp vs the plain random/greedy fallback: blocked
hunters **flow with the swarm around a jam instead of stalling**, so **kills rise
~145→175 per 1k ticks**, and the **mean nest crowd drops** (~9.7→7.9 within 2 cells)
while alive stays ~945 and the cap holds. The worst single nest ticks up slightly
(~19.8→21.7) — cohesion's cost — which the light `COH` keeps small.

### Determinism

All randomness — every wander direction and every hunt/home roll — comes from a
**single integer PRNG** (xorshift32) seeded in the `World` and advanced as mites are
processed in index order. The whole swarm is a **pure function of the seed and the
inputs**: same seed + same inputs ⇒ identical state, pinned by a state-hash test
(including under overflow, and with combat on). The seed is editable on the page; the
fifteen nests are fixed, one per screen.

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
  The turret then swings toward that bearing at its turn rate. The turret aims, fires, and is
  drawn at a **finer 256-step resolution** (8× the 32 directions the bodies and mites move
  along, via `fine_cos`/`fine_sin`), so it tracks smoothly and points precisely; only target
  selection uses the coarse 32.
- **Fire (a bolt).** Once the turret has swung **exactly** onto the target bearing (so the
  bolt leaves straight along the barrel — a line shot can't fire in a cone) and the cooldown is
  elapsed (`fire_period`, default 30 ticks = **2/sec**; `0` = aim-only), launch a **piercing
  projectile** from the muzzle into the first free bolt slot. The bolt is a
  travelling shot (`proj_speed`, a page tunable — slow default 1 cell/tick), not a hitscan beam: each tick `proj_step`
  marches it along its fixed heading, **destroying every mite within `PROJ_HW` of the swept
  segment and piercing on through them** (a kill never stops it), until it meets a **wall** or
  has flown `PROJ_RANGE` cells, then it expires — a hot **impact spark** marks where it struck a
  wall. Because the 32-direction aim can't pin an
  off-centre mite exactly, the bolt **carries the mite it was aimed at** (`tank_proj_tgt`) and
  kills it for sure when it reaches that mite's cell — so a locked turret reliably drops what
  it shot at, while the thin half-width means other mites a sub-segment off the line **dodge**.
  Each destroyed mite is marked dead (`mite_resp` = the respawn timer), drops out of the index
  next rebuild (never drawn — minimap included — gossiped, or targeted), and spawns a
  **destruction burst**. Firing **no longer locks the turret**: the moment the bolt is away the
  barrel is free to swing onto the next target while the shot is still travelling. A tank may
  have **up to `PROJ_MAX` (4) bolts in flight at once** (a flat pool, `t*PROJ_MAX + slot`), so a
  slow bolt no longer throttles the fire rate — only when **all four slots are full** does
  firing wait for one to clear; the cadence is otherwise the cooldown. A tank also **holds its
  shot if the bolt would pass through a friendly tank** (`friendly_in_path`: another tank ahead,
  within range, inside a body-plus-beam corridor, in clear line of sight) — tanks never fire
  through each other, even at the cost of sparing a mite hiding behind one.
- **Death cry.** Each kill writes the **firing tank's cell** into the record of every live
  mite within **2× sensing range** of the destroyed one (stamped now, mode → hunt), through
  the ordinary record buffer — so a bolt that mows a line broadcasts the shooter's position
  from every body it drops, into the same gossip that spreads any sighting, and the
  survivors converge on the attacker.
- **Run-over.** A tank is one grid cell across; while it is **moving** it crushes any live mite
  whose centre lies under that one-cell footprint (an ordinary kill — burst + death cry), found
  over the 3×3 cells of the index. A parked tank is harmless — so driving through the swarm mows
  a path the way the bolt does, and the crushed mites' death cries turn the neighbours on the tank.

A dead mite **revives at its nest with its memory wiped** (record emptied, mode reset to
wander) after `mite_respawn` ticks (default 300 = **5 s**), respecting the crowding cap:
it revives only if the nest cell has a free slot once current occupancy **and inbound
reservations** are counted (the same tally `mites_step` uses), else it waits a tick.
Reviving happens right after the index is built (step 1b) so the revived mite is an
ordinary live mite for the rest of the tick. There is **one nest per screen except the
tanks' start screen (0,0)** — fifteen homes — so revived mites never pop up under a barrel,
and the spawn load is spread over fifteen screens' exits, not piled out one screen's. That
spread is what keeps this revival dead simple: a busy nest cell just makes the mite wait a
tick — with the load shared, that almost never happens, so no neighbourhood-fallback or
jam-detector machinery is needed. Firing draws **no randomness**, so the swarm stays a pure
function of the seed and inputs.

## Per-tick order (`sim.c`)

1. **Build the per-cell mite index** from current positions.
   1b. **`mites_respawn`** — revive timed-out dead mites at their nests, into the
   freshly-built index (cap-respecting), so they are ordinary live mites this tick.
2. **Tank input** (`tanks_path`; controls for the MANUAL tank), unchanged.
3. **`mites_records`** — gossip the record (double-buffered LWW); each mite's mode +
   destination fall out; swap the record buffers.
4. **`mites_update_fields`** — collect the distinct active destinations (15 nests + the
   hunters' cells), fold a route field for each new one, reuse resident, free dropped.
5. **`mites_step`** — the crowding cap (in-order reservation) + steer each mite along
   its field (or greedy on overflow, or wander); write `mite_in`.
   5b. **`tanks_fire`** — advance any bolt in flight (pierce-kill along its segment, stop at a
   wall), swing each turret onto the most-hittable mite, fire a bolt on cooldown (+ bursts +
   death cry); age the effect ring (revival already happened in 1b).
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
| `src/tanks_fire.c/.h` | combat: per-tank turret aim (turns at a rate toward the most-hittable mite — closest to the turret's bearing — via a box scan + Bresenham line of sight), the fixed-rate **piercing bolt** (a travelling projectile that mows every mite within a thin half-width of its path and pierces on until a wall) + destruction bursts, the death-cry gossip, and the cap-respecting nest respawn |

`defs.h` adds the mite vocabulary (`N_MITES`, `MITE_R`, `MITE_CAP`, `NEST_COUNT`,
`nest_of`, `N_FIELDS`, the modes, `REC_EMPTY`, world-cell + toroidal-distance helpers,
the shared `at_cell_centre`) and the combat vocabulary (`TARGET_MAX_R`, `TGT_NONE`,
`PROJ_RANGE`, `PROJ_HW`, `PROJ_SPEED_DEFAULT`, `N_FX`, `FX_DURATION`).
`sim.h`'s `World` gains the mite pool (SoA, incl. `mite_dest` and `mite_resp`), the
double-buffered records, the per-cell index, the fifteen nests, the shared route-field
table, the field active/peak counters, the per-tank turret/cooldown/target + in-flight
**bolt** state (`tank_proj_xy`/`dir`/`dist`/`tgt`/`live`), the **destruction-effect ring**
(`fx_xy`/`fx_t`), the PRNG, and the mite + combat tunables (`fire_period`, `mite_respawn`,
`turret_rate`, `proj_speed`).
`render.c` draws only the mites the camera shows (cost scales with visibility), tinted
by role (wander/hunt/home-by-nest), skips dead mites, draws each tank's turret on
`tank_turret` (separate from the body), each in-flight **bolt** (a hot streak + bright head)
and the **bursts** (white mite-kills, orange wall impacts), plus the fifteen nest markers; the minimap (`app.js`) shows the
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
| combat: per-tank turret/cooldown/target + in-flight bolts (4 slots × 4 tanks) + effect ring `fx_xy`/`fx_t`[256] | ~2 KB |
| `mites.c` BFS/scatter/tally/dest-mark scratch (file-static) | ~34 KB |
| instance buffer (2 screens + up to `N_MITES` mite quads + nests) | ~43 KB |
| 15 nests, PRNG state, mite + combat tunables, 128 KB stack | — |

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

`src/test.c` covers **117 checks**: the inherited chapter-1/2 movement + pathing (a
regression after the `agent_*` rename and the `edge_paths` generalisation — names and
shape changed, not behaviour); the **pool & index** (each mite in its own (cell,
sub-segment) slot); the **crowding cap** (every tick, naturally, under forced
convergence, **and with firing on** — kills + nest respawns; ≤1 mite per sub-segment);
the **gossip** (one-hop-per-tick propagation = order-independent, newer overwrites
older, older never overwrites newer, a newer *empty* propagates); the **behaviour**
(sense → hunt, arrive erase/refresh, the 80/20 hunt/home split at the `P_HUNT`
boundaries and statistically, a newer record interrupts a homing mite and re-rolls, a
mite that reaches its nest **reverts to wander** with its record cleared — so homed mites
don't pile up nest-coloured); the **nests & shared fields** (`nest_of` partitions the swarm into fifteen balanced groups; a homing mite
reaches its nest; a hunting mite reaches its recorded cell; **a mite's shared-field
route equals the tank pathing ground truth** for the same destination; the
**distinct-destination peak is measured and stays within `N_FIELDS`** — ~32 with combat
on; a forced overflow falls back to greedy without breaking the cap or determinism);
**combat** (the turret acquires a mite in line of sight and a **travelling bolt reaches and
destroys** it, spawning a **burst**; it leaves the index; the **bolt is a thin line that
pierces** — it kills both mites on its line and continues through the first, mites off it
(spread into other sub-segments) **dodge**; the **bolt stops at a wall** — a
collinear mite beyond it survives; a wall
blocks line of sight; the turret **swings at a turn rate** and does not snap; targeting
picks the mite **closest to the turret's direction**, not the spatially nearest; the
**turn rate gates firing** — an off-axis target isn't shot until the turret swings on; the
**turret is free to swing onto a new target while the bolt is still travelling** (firing no
longer locks it); a tank keeps **several bolts aloft at once, capped at `PROJ_MAX`**; the
death cry teaches a nearby mite the shooter's cell and flips it to hunt; a destroyed mite
revives at its nest **with its memory cleared** after the timeout; a **moving** tank runs over
a mite under it (a still one doesn't); a bolt striking a wall leaves an **impact** spark);
**wall safety** at
`MITE_R`; and **determinism** (the swarm state hash **and** the tank combat-state hash,
with firing on by default).

## Verified

- `./test.sh` passes (117 checks): the swarm/gossip/cap/nests/fields/overflow/combat/
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
future step); mite-vs-mite or mite-vs-tank physical collision; the bolt as a full physics
body (it travels, pierces, and stops at walls, but doesn't bounce, block, or collide with
tanks); more than
one faction; flocking/alignment beyond the cap; **per-mite route fields** (the
experiment shares a few instead). Each is its own future step.
