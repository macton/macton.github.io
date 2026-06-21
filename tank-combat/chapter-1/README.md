# Tank Combat — Chapter 1

Chapter 1 of *Data-Oriented Design, by picture* (the book index is one level
up). A tiny 2D tank game inspired by Atari *Combat*, built in C, compiled to
WebAssembly, rendered with WebGPU. **The point of the project is to demonstrate
a data-oriented approach to building a game** — not the game itself. Features
are added one step at a time. The page (`index.html`) presents the game above a
write-up with every data structure live and editable in context.

It is built following Mike Acton's data-oriented design rules:
[**data-oriented-design.md**](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md)
— understand the real data first, then write the simplest machine that
transforms the input you have into the output you need, at a cost you can state.

**So far:** two tanks, obstacles on a grid, movement, rotation, and wall
collision on a wrap-around (toroidal) arena, with an explicit, tested movement
contract ([`CONTRACT.md`](CONTRACT.md)): holding a throttle auto-rotates a tank
out of walls, corners, and pockets so it never gets stuck. A debug panel exposes
and edits every datum live.

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge, or
Firefox Nightly).
- **Tank 0 (amber):** `W`/`S` move, `A`/`D` turn
- **Tank 1 (cyan):** arrow keys
- **Mobile / touch:** hold the on-screen D-pads overlaid on the map corners
  (left pad = tank 0, right pad = tank 1).

## Why it's built this way

- **Simulation is fully separated from rendering and from wasm.** `sim.c` (the
  `World` data + `sim_init`/`sim_tick`) is plain portable C that knows nothing
  about WebGPU or WebAssembly. The renderer (`render.c`) reads the `World` and
  is downstream; `wasm.c` is the only wasm-specific file. So the whole
  simulation **builds and runs natively on the host** for testing — no browser,
  no GPU (see *Testing* and `CONTRACT.md`).
- **All state is data, flat and static.** The `World` is a handful of arrays.
  No objects, no hidden state.
- **Transforms are separate files by concern.** `tanks_turn.c` owns all heading
  rotation (player turn + auto-steer, since both write the heading) and
  `tanks_move.c` owns position; `collide.c` is the shared grid query; `dirtab.c`
  is the baked trig table. See *Source layout* below.
- **No dynamic allocation.** The module is freestanding wasm32 (`-nostdlib`,
  no libc, no libm). Every byte lives in static linear memory, sized at compile
  time. WASM memory is never grown, so the JS-side typed-array views over it
  stay valid for the life of the page.
- **No floating point — integer fixed point throughout.** Positions and sizes
  are *subcells* (256 subcells = 1 grid cell, Q8.8) stored as `int16_t`; the
  trig table is Q14 (`value = trig * 16384`); colors are packed `uint32_t`
  RGBA8888. The single place fixed point becomes float is the vertex shader —
  the GPU is float hardware, so that is its data format.
- **Fixed timestep, so there is no `dt` to multiply.** `tick()` advances one
  simulation step; the per-tick deltas (`move_speed`, `turn_rate`) *are* the
  data. The host runs ticks from a real-time accumulator at 60 Hz.
- **Tanks are structure-of-arrays.** `x[]`, `y[]`, `angle[]`, `input[]`,
  `vx[]`, `vy[]` — each transform streams the one field it needs. Two tanks is
  just `count == 2`; the transforms are written for the batch.
- **Heading is discrete data, not runtime trig.** A tank's heading is a
  `uint16_t` *Q5.11* angle: the top 5 bits (`angle >> 11`) select 1 of 32
  directions and index the baked cos table (sin is the same table a
  quarter-turn earlier); the low 11 bits are
  sub-direction precision so `turn_rate` can be finer than one direction per
  tick. No `sinf`, no libm.
- **One transform produces the render data.** `build_instances()` turns world
  state into a packed integer instance buffer (16 bytes/quad). JS copies that
  buffer straight to the GPU and issues a single instanced draw.
- **Explicit data protocol at the boundary.** C owns every byte of layout; JS
  never computes an offset — it asks C for the pointer or count it needs. The
  exported function list *is* the protocol (see the header of `wasm.c`).

## Source layout

Simulation core (portable C, no render, no wasm):

| file | responsibility |
|------|----------------|
| `src/defs.h` | shared sizes, input bits, exact-width types |
| `src/sim.c/.h` | the `World` data + `sim_init`/`sim_tick` |
| `src/tanks_turn.c/.h` | transform: all heading rotation (player turn + auto-steer); builds the escape table (`tanks_build_escape`) |
| `src/tanks_move.c/.h` | transform: advance + resolve grid collision (sets `hit`) |
| `src/collide.c/.h` | grid collision query (`blocked`) + per-cell open-direction masks (`cells_build_move`) |
| `src/dirtab.c/.h` | baked Q14 cos table, sin derived by quarter-turn offset |

Boundaries on top of the core:

| file | responsibility |
|------|----------------|
| `src/render.c/.h` | reads `World` → `Inst` instance buffer (`build_instances`) |
| `src/wasm.c` | the wasm boundary: static `World`, exports the data protocol |
| `src/test.c` | native tests of the sim core (the movement contract) |

`sim.c` does not include `render.h` or anything wasm — the dependency is
one-way (render and wasm depend on sim, never the reverse). Transforms take
their data as parameters and reference no globals. Only the protocol functions
are exported from the wasm (the internal transforms are not).

## The data

| data | type | notes |
|------|------|-------|
| grid | `uint32_t[15]` | bitset: one word per row, bit c = column c (1 = wall) |
| tank x, y | `int16_t[2]` each | subcells, 256 = 1 cell (Q8.8) |
| tank angle | `uint16_t[2]` | Q5.11 heading: 5-bit direction (`>>11` → 1 of 32) + 11-bit turn fraction |
| tank input | `uint8_t[2]` | bits: FWD/BACK/LEFT/RIGHT |
| tank vx, vy | `int16_t[2]` each | last tick's applied move, subcells |
| tank hit | `uint8_t[2]` | blocked-axis bitmask this tick: bit0 = x, bit1 = y (names the wall) |
| cell_escape | `uint8_t[20*15*32]` | steer lookup `[cell*32 + travel]`: precomputed nearest-open escape (bit5 = handedness, bits0-4 = direction; `0xFF` = none). Rebuilt only on grid change |
| trig table | `int16_t[32]` | cos in Q14 (`±16384`); sin = cos a quarter-turn earlier |
| instances | `Inst[W*H + 4]` | render output, 16 B: int16 cx,cy,hx,hy,co,si + uint32 rgba (RGBA8888) |

Arena is the grid: 20×15 cells, 256 subcells per cell.

## Per-tick transform

1. `set_input(tank, bits)` — host writes keyboard/touch state into `input[]`
2. `tanks_turn` — rotate the heading. The input bits decode branch-free into a
   turn direction, a throttle sign, and (when reversing) a half-turn travel
   offset — these are simple bit functions, so no table is needed. The player's
   turn is ±`turn_rate` per LEFT/RIGHT (wraps). Then, if the tank
   collided last tick (`hit`) while driving, **auto-steer** is a single lookup:
   `cell_escape[cell*32 + travel]` gives the nearest open direction (and the
   handedness to turn), and it rotates (at `turn_rate`) toward it. That slides it
   along flat walls, turns it out of convex corners, and rotates it around to the
   opening of a concave pocket — so holding a throttle never leaves it
   permanently stuck. This is the **movement contract** (`CONTRACT.md`), enforced
   by the native tests. `cell_escape` is rebuilt by `sim_grid_changed` only when
   the grid changes (`init`, `toggle_wall`): the nearest-open *scan*
   itself is precomputed for every cell × travel direction, so the per-tick path
   never searches — it reads one byte.
3. `tanks_move` — step along the heading by `move_speed` subcells, resolving
   collision one axis at a time so a tank slides along a wall it hits at an
   angle; sets `hit` when an intended axis move was rejected.

The render/host then: `build_instances` turns the `World` into the packed
integer instance buffer, and the host copies it to the GPU and draws
`inst_count` quads. (In the wasm build, `tick()` runs the sim step and refills
the instance buffer; the simulation itself contains no rendering.)

**Toroidal arena (explicit):** there is no hard edge — the world wraps. A tank
crossing an edge reappears on the opposite side; collision depends only on the
wall on the far side, so two opposite open edges let it pass through, while an
opposite wall stops it (collision tests the tank's footprint at wrapped
coordinates). The default map has a full border, so it plays as a closed arena
until you open a border cell. A blocked axis move is *rejected* (the tank keeps
its old coordinate on that axis).

**Steering handedness (explicit):** the escape turn always rotates in one
consistent direction (the handedness the search found the opening), not the
shortest signed turn — otherwise a 180° escape (e.g. backing into a dead-end
that wraps onto a wall) would oscillate forever at the antipode. The escape
search probes a *whole cell* ahead, so a direction that only admits a sub-cell
wiggle into a dead-end is correctly rejected.

Collision is destination-only, not swept — at very high `move_speed` (editable
in the panel) a tank can tunnel through a one-cell wall. Acceptable at the
default speed; see `issues/`.

## Build

Requires `clang` with the `wasm32` target and `wasm-ld` (LLVM ≥ 15). No
emscripten.

```sh
./build.sh        # writes game.wasm (~3.9 KB)
```

`game.wasm` is committed so GitHub Pages serves it without a build step.

## Testing

Because the simulation is separated from rendering and wasm, it builds and runs
on the host:

```sh
./test.sh         # compiles the sim core + src/test.c with host clang, runs it
```

`src/test.c` sets up each movement-contract case on a `World` and asserts the
outcome: open-space (no auto-steer), sliding along a flat wall, head-on into a
wall (forward and reverse), convex corners, the arena's own corners, concave
U-pockets (forward and reverse rotate out and exit), a 3000-tick roam of the
real map (never stuck more than a few ticks), and "no throttle ⇒ no movement".
No browser or GPU needed.

## Verified

- `./test.sh` passes (14 checks): the full movement contract above.
- Simulation also exercised via `game.wasm` in Node (forward motion, collision,
  pocket escape through the wasm path) — identical to native, as expected since
  they share the same `sim.c`.
- **Not** verified here: the live WebGPU render and on-screen touch pads — no
  GPU/browser in the build environment. The host uses standard WebGPU calls;
  render-test in a browser.

## Deferred (not built — no speculative generality)

Firing/projectiles, tank-vs-tank collision, scoring, sound, AI, multiple maps.
Each is its own future step.
