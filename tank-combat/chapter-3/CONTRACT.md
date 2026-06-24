# Chapter 3 contract — the swarm

The explicit promises chapter 3 makes, so they can be relied on and tested. The
tests in `src/test.c` enforce them (121 checks). Chapter 3 **inherits chapter 1's
movement contract** and **chapter 2's pathing/viewport contract**
([../chapter-2/CONTRACT.md](../chapter-2/CONTRACT.md)) unchanged — the four tanks
still route themselves exactly as before. The rename of the shared transforms to
`agent_turn`/`agent_move` (with a `radius` parameter) changed names, not behaviour:
the inherited tests still pass and the baked escape table is byte-identical.

## The pool

- **A fixed pool of `N_MITES` (= 1000) mites**, spawned at init and alive for the
  whole chapter — flat structure-of-arrays, statically sized, **no dynamic
  allocation**. *(tested: the index counts sum to `N_MITES`.)*
- **Spawn is deterministic and legal.** From the seed, mites scatter across **open,
  reachable** cells only, **≤ 1 per (cell, sub-segment)** (so ≤ 4 per cell), each parked
  at its sub-segment's centre. *(tested: every mite is on an open cell reachable from the
  open ring; each occupies its own slot.)*

## The per-cell index

- **Rebuilt every tick from current positions.** A cell is quartered into `MITE_CAP` (= 4)
  **sub-segments** (2×2); `mite_list[cell*4 + seg]` is the mite in that sub-segment (or
  `REC_EMPTY`), and `mite_cnt[cell]` is the number of occupied sub-segments. Each mite
  occupies its own slot, `seg_of(m) = (m >> 2) & 3`. *(tested.)* It is the swarm's `O(N)`
  acceleration structure — "which mites are within one cell of me" is reading a 3×3
  neighbourhood, not a 1000×1000 scan.

## The crowding cap

- **At most one mite per (cell, sub-segment)** — so ≤ `mite_cap` (≤ 4) mites per cell —
  after every tick. A mite is assigned a sub-segment by index (`(m >> 2) & 3` — distinct
  from `nest_of = m & 3`, so a nest's mites spread over all four segments and revive in
  parallel rather than one at a time), moves into it
  (parking a quarter-cell off the cell centre, which spreads the swarm out), and enters a
  cell only if its own sub-segment there is free. Enforced by a deterministic in-order
  reservation — a per-cell **bitmask** of reserved sub-segments (current occupants +
  committed inbound) — not by physics. A blocked mite re-routes to a neighbour with a free
  sub-segment, or holds, and re-evaluates next tick. *(tested: the cap and the
  one-per-sub-segment invariant hold across thousands of natural ticks **and** under
  forced convergence of the whole swarm on one cell; and it is binding — cells reach 4.)*

## The shared record (last-write-wins)

- **One cell + one timestamp per mite** — the last known tank position (a world cell,
  or empty), and the frame it was recorded. **Newer timestamp wins**; ties break on
  lowest mite index. *(tested.)*
- **Empty is a readable value that propagates.** A newer *empty* record overwrites an
  older cell record by the same rule. *(tested.)*
- **Timestamps are monotone.** An older record never overwrites a newer one. *(tested.)*
- **The gossip is order-independent.** Every mite reads the previous tick's records and
  writes the next tick's, then the buffers swap (double-buffered), so a planted record
  spreads exactly **one hop per tick** along a connected in-range chain, regardless of
  iteration order. *(tested.)*

## Behaviour

- **The record is typed.** A record cell is a **sighting** "tank at X" (a plain cell), a
  **gone** "X is empty" (`X | GONE_FLAG`, the top bit), or **no info** (`REC_EMPTY`). Only
  sightings and matching gones propagate; no-info never does. *(tested.)*
- **Sense → sighting + hunt.** A tank within `mite_sense` cells sets the record to a
  sighting of the tank's cell, stamped this frame, and the mite hunts it. A self-sensed
  tank is always hunted. *(tested.)*
- **Adopt → 80/20 hunt/home.** On adopting a newer peer **sighting**, the mite rolls the
  role die: with probability `mite_phunt` (= 80%) it **hunts** the cell, else it **paths
  home** to its nest; either way it keeps and relays the sighting. The roll fires **even
  while already hunting or homing**. *(tested: boundaries `P_HUNT = 0`/`100` exact, default
  split matches the RNG stream, and a newer sighting interrupts a homing mite.)*
- **Targeted "gone".** A **gone-of-X** record is adopted **only by a mite that is hunting
  X** — it then stands down to wander and relays the gone onward. A gone-of-X never touches
  a mite hunting a *different* cell, so it disperses a stale cluster off X without ever
  wiping a live sighting elsewhere (the swarm cannot "forget" a tank that has not moved).
  *(tested: a gone-of-X clears an X-hunter and is ignored by a Y-hunter.)*
- **Arrive (hunt).** A hunting mite that reaches within one cell of its recorded cell
  refreshes the sighting (stamp now) if a tank is there; if the tank is **gone** it
  broadcasts a **gone-of-X** (stamp now) and wanders. *(tested: refresh; the gone-of-X
  broadcast.)*
- **Arrive (home).** A homing mite that reaches its nest **deposits its sighting in the
  nest if it is newer** than the nest's, then reverts to wander. *(tested: it reverts to
  wander; the deposit promise below.)*
- **A no-info record means wander.** A mite with no recorded cell wanders.

## Nests as knowledge hubs

- **Each nest stores the latest sighting** (`nest_rec_cell`/`nest_rec_time`). A homing mite
  that reaches its nest overwrites it **iff the mite's sighting is newer** (newest-wins).
  Only sightings are deposited — a courier never erases the nest. *(tested: a newer sighting
  is deposited; an older one is not; an empty courier never erases it.)*
- **A revived mite adopts its nest's gossip**: it hunts the nest's last-known tank cell,
  or wanders if the nest knows nothing — so killing the swarm doesn't erase its knowledge;
  the nest seeds the next wave. *(tested: revive-and-hunt with intel; revive-and-wander
  without.)*
- **Nest memory times out.** A nest sighting older than `nest_ttl` (default 600 ticks =
  10 s; `0` = never) expires — the nest's only forgetting mechanism, so it stops re-seeding
  hunters at a position no courier has refreshed. *(tested: expires past the TTL; kept
  within it.)*

## Nests & shared route fields

- **Every mite belongs to one of four nests**, `nest_of(i) = i % 4`, partitioning the
  swarm into four equal groups. *(tested.)*
- **A hunting/homing mite navigates the path tables, not a greedy heuristic.** A homing
  mite reaches its nest cell; a hunting mite reaches within one cell of its recorded
  cell. *(tested.)*
- **The route is a shared field keyed by destination, equal to the tank ground truth.**
  A mite's lookup into the field for a destination returns the same step the tank
  pathing returns for that destination — the field *is* the tank route, keyed by
  destination instead of by tank. *(tested.)*
- **The field table is sized from a measured peak.** Across representative scenarios the
  distinct active-destination count peaks at ~22 (≈21 with combat on — the death cry's
  goals overlap existing sightings), and `N_FIELDS = 64` holds it with headroom; the live
  count and peak are shown on the page. If the count ever exceeds `N_FIELDS`, the overflow
  mites steer greedily that tick — **no crash, no cap break**. *(tested: the peak stays
  within `N_FIELDS`; a forced overflow holds the cap and stays deterministic.)*

## Movement & collision

- **Mites collide only with walls**, through the shared `agent_move` at radius
  `MITE_R`. A mite's footprint **never overlaps a wall** across a run. *(tested.)*
- **Mites do not collide with tanks or with each other** — crowding is the cap (a
  decision-level rule), not physics. The cap gates the actual step in every mode.

## Combat (the tanks shoot the swarm)

- **Each turret aims independently of the body and turns at a finite rate.**
  `tank_turret` is separate from `tank_ang` (movement heading) and swings toward its aim
  at `turret_rate` per tick — it does not snap. With no target it relaxes toward the body
  heading. *(tested: the turret aims even with firing off; it does not snap; the body is
  unaffected.)*
- **The target is the mite MOST LIKELY TO BE HIT, not the spatially nearest.** Among the
  mites in line of sight (within the search radius), the turret targets the one closest to
  its current direction — the least rotation to bring the barrel on — over the per-cell
  index; lowest index breaks angle ties. *(tested: a farther but aligned mite is chosen
  over a nearer off-axis one.)*
- **Line of sight is required.** A mite is targetable only if an integer Bresenham walk
  from the tank cell to the mite cell crosses no wall. *(tested: a mite behind a wall is
  not targeted.)*
- **Fire is a fixed-rate, exactly-aimed laser.** The tank fires only once the turret has
  swung **exactly** onto the target bearing (so the beam runs precisely along the barrel —
  a line shot is never a cone, so the turn rate gates firing) and the cooldown is elapsed,
  every `fire_period` ticks (default 30 = 2/sec; `0` disables firing, aim only). The shot
  is a **laser**: a thin ray marched from the tank along the barrel until it meets a wall
  (length capped at `LASER_MAX`), destroying mites whose position lies within `BEAM_HW` of
  the beam line. Because mites sit in sub-segments a quarter-cell off centre, mites **on**
  the line die and mites **off** it dodge — the aimed target is always destroyed. The beam
  is drawn for `LASER_TICKS` (~0.1 s), during which the **turret is locked** to its firing
  direction (it cannot turn toward another target until the beam fades) — a shot commits
  the aim. Each destroyed mite is marked dead and leaves the per-cell index next rebuild —
  a corpse is not drawn (on the map either), gossiped, or targeted — and spawns a cosmetic
  destruction burst. *(tested: the laser destroys the target + a burst; it kills mites on
  its line and **misses ones off it**; it stops at a wall (a collinear mite beyond
  survives); the cooldown; the corpse leaving the index; an off-axis target not shot until
  the turret swings on; the turret holds while the beam is live and turns again once it
  fades.)*
- **Every kill is a death cry.** Every live mite within **2× `mite_sense`** cells of a
  destroyed one has its record set to the firing tank's cell (stamped now) and its mode set to hunt,
  through the ordinary record buffer — the swarm turns on its attacker by the same gossip
  that spreads any sighting. *(tested.)*
- **A dead mite revives at its nest with its memory cleared, after `mite_respawn` ticks**
  (default 300 = 5 s). The record is emptied and the mode reset to wander, so it does not
  resume an old hunt. Revival **respects the crowding cap** (current occupancy + inbound
  reservations); if the nest is full it waits a tick. The four nests sit **off the tanks'
  start screen** so revived mites don't appear under a barrel. *(tested: revival at the
  nest after the timeout with the record cleared; the cap holds across thousands of ticks
  with firing on.)*

## Determinism

- **The swarm is a pure function of the seed and the inputs.** Same seed + same inputs
  ⇒ identical state hash after K ticks; a different seed scatters differently;
  re-seeding to the same seed is reproducible. *(tested.)* All randomness is one
  xorshift32 stream advanced in index order. **Combat draws no randomness** — aiming,
  firing, kills, and respawns are deterministic, so they replay identically too.
  *(tested: the tank combat-state hash matches across runs, with firing on by default.)*

## Boundaries / non-promises

- **The cap is the structural maximum 4** (it sizes the occupant list). The `mite_cap`
  tunable may tighten it within `[1, MITE_CAP]`; it cannot exceed 4.
- **The route table is bounded by `N_FIELDS`.** Beyond the measured peak it is a
  deliberate static size; an overflow degrades to greedy steering for that tick, it does
  not grow the table.
- **Mites route, tanks route — but the cost differs.** Each tank holds its own route;
  the mites share a few fields keyed by destination. A mite stuck where no field route
  exists (overflow, or a sealed goal) falls back to greedy and relies on the
  wander/erase rules and the swarm's collective coverage.
- **The laser is hitscan, and only mites take damage.** The beam is an instant line to
  the nearest wall, not a travelling projectile; tanks fire but are not damaged by the
  swarm (tank health/score is not promised here). A turret parked facing a nest
  legitimately suppresses it — a revived mite in the beam's path is destroyed again.
  Destruction bursts are cosmetic only (no gameplay effect).
- **The viewport is presentation only** (inherited): following, sliding, the picker,
  and which mites are drawn never change the simulation.

## How it's tested

`./test.sh` builds the simulation core (no wasm, no renderer, no GPU) with the host C
compiler and runs `src/test.c`, which builds the world, drives the swarm, and asserts
each promise above. Because the simulation is separated from rendering, these tests
need no browser or GPU.
