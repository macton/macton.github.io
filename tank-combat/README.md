# Tank Combat

A tiny 2D tank game inspired by Atari *Combat*, built in C, compiled to
WebAssembly, rendered with WebGPU. **The point of the project is to demonstrate
a data-oriented approach to building a game** — not the game itself. Features
are added one step at a time.

It is built following Mike Acton's data-oriented design rules:
[**data-oriented-design.md**](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md)
— understand the real data first, then write the simplest machine that
transforms the input you have into the output you need, at a cost you can state.

**Step 1 (this commit):** two tanks, obstacles on a grid, movement, rotation,
and collision against walls and the arena edge. A debug panel exposes and edits
every datum live.

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge, or
Firefox Nightly).
- **Tank 0 (amber):** `W`/`S` move, `A`/`D` turn
- **Tank 1 (cyan):** arrow keys
- **Mobile / touch:** hold the on-screen D-pads overlaid on the map corners
  (left pad = tank 0, right pad = tank 1).

## Why it's built this way

- **All state is data, flat and static.** `game.c` is the entire simulation: a
  handful of arrays and the transforms over them. No objects, no hidden state.
- **No dynamic allocation.** The module is freestanding wasm32 (`-nostdlib`,
  no libc, no libm). Every byte lives in static linear memory, sized at compile
  time. WASM memory is never grown, so the JS-side typed-array views over it
  stay valid for the life of the page.
- **No floating point — integer fixed point throughout.** Positions and sizes
  are *subcells* (256 subcells = 1 grid cell, Q8.8) stored as `i16`; the trig
  table is Q14 (`value = trig * 16384`); colors are `u8`. The single place
  fixed point becomes float is the vertex shader — the GPU is float hardware,
  so that is its data format.
- **Fixed timestep, so there is no `dt` to multiply.** `tick()` advances one
  simulation step; the per-tick deltas (`move_speed`, `turn_rate`) *are* the
  data. The host runs ticks from a real-time accumulator at 60 Hz.
- **Tanks are structure-of-arrays.** `x[]`, `y[]`, `angle[]`, `input[]`,
  `vx[]`, `vy[]` — each transform streams the one field it needs. Two tanks is
  just `count == 2`; the transforms are written for the batch.
- **Heading is discrete data, not runtime trig.** A tank's heading is a `u16`
  angle; `angle >> 11` indexes the 32-entry cos/sin table. No `sinf`, no libm.
- **One transform produces the render data.** `build_instances()` turns world
  state into a packed integer instance buffer (16 bytes/quad). JS copies that
  buffer straight to the GPU and issues a single instanced draw.
- **Explicit data protocol at the boundary.** C owns every byte of layout; JS
  never computes an offset — it asks C for the pointer or count it needs. The
  exported function list *is* the protocol (see the header of `game.c`).

## The data

| data | type | notes |
|------|------|-------|
| grid | `u8[20*15]` | `0` empty / `1` wall |
| tank x, y | `i16[2]` each | subcells, 256 = 1 cell (Q8.8) |
| tank angle | `u16[2]` | `0..65535`, `>>11` → 1 of 32 directions |
| tank input | `u8[2]` | bits: FWD/BACK/LEFT/RIGHT/FIRE |
| tank vx, vy | `i16[2]` each | last tick's applied move, subcells |
| trig table | `i16[32]` ×2 | cos/sin in Q14 (`±16384`) |
| instances | `Inst[W*H + 4]` | render output, 16 B: i16 cx,cy,hx,hy,co,si + u8 rgba |

Arena is the grid: 20×15 cells, 256 subcells per cell.

## Per-tick transform

1. `set_input(tank, bits)` — host writes keyboard/touch state into `input[]`
2. `tanks_turn` — add/subtract `turn_rate` from each `angle` per LEFT/RIGHT (wraps)
3. `tanks_move` — step along the heading by `move_speed` subcells, resolving
   collision one axis at a time so a tank slides along a wall it hits at an angle
4. `build_instances` — world state → packed integer instance buffer
5. host copies the instance buffer to the GPU and draws `inst_count` quads

**Out-of-range / collision policy (explicit):** the arena edge and any wall
cell count as blocked; a blocked axis move is *rejected* (the tank keeps its
old coordinate on that axis). This keeps every grid index in range. Collision
is destination-only, not swept — at very high `move_speed` (editable in the
panel) a tank can tunnel through a one-cell wall. Acceptable at the default
speed; see `issues/`.

## Build

Requires `clang` with the `wasm32` target and `wasm-ld` (LLVM ≥ 15). No
emscripten.

```sh
./build.sh        # writes game.wasm (~3.4 KB)
```

`game.wasm` is committed so GitHub Pages serves it without a build step.

## Verified

- Simulation tested in Node against `game.wasm`: tank placement, forward motion
  (moves along heading, perpendicular axis unchanged), turning (angle/direction
  index advance), wall collision (stops before interior walls), edge clamp
  (stays ≥ tank radius), and instance count (`walls + tanks*2`). All instances
  land inside the arena.
- **Not** verified here: the live WebGPU render and on-screen touch pads — no
  GPU/browser in the build environment. The host uses standard WebGPU calls;
  render-test in a browser.

## Deferred (not built — no speculative generality)

Firing/projectiles, tank-vs-tank collision, scoring, sound, AI, multiple maps.
Each is its own future step.
