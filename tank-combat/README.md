# Tank Combat

A tiny 2D tank game inspired by Atari *Combat*, built in C, compiled to
WebAssembly, rendered with WebGPU. **The point of the project is to demonstrate
a data-oriented approach to building a game** — not the game itself. Features
are added one step at a time.

**Step 1 (this commit):** two tanks, obstacles on a grid, movement, rotation,
and collision against walls and the arena edge. A debug panel exposes and edits
every datum live.

Play: open `index.html` from any web server with WebGPU (recent Chrome/Edge, or
Firefox Nightly).
- **Tank 0 (amber):** `W`/`S` move, `A`/`D` turn
- **Tank 1 (cyan):** arrow keys

## Why it's built this way

Following Mike Acton's data-oriented design rules: understand the real data
first, then write the simplest machine that transforms the input you have into
the output you need, at a cost you can state.

- **All state is data, flat and static.** `game.c` is the entire simulation: a
  handful of arrays and the transforms over them. No objects, no hidden state.
- **No dynamic allocation.** The module is freestanding wasm32 (`-nostdlib`,
  no libc, no libm). Every byte lives in static linear memory, sized at compile
  time. WASM memory is never grown, so the JS-side typed-array views over it
  stay valid for the life of the page.
- **Tanks are structure-of-arrays.** `x[]`, `y[]`, `angle[]`, `input[]`,
  `vx[]`, `vy[]` — each transform streams the one field it needs. Two tanks is
  just `count == 2`; the transforms are written for the batch.
- **Heading is discrete data, not runtime trig.** A tank's heading is a
  `u16` angle; `angle >> 11` indexes a baked 32-entry cos/sin table. No `sinf`,
  no libm, no per-frame trig — a lookup, as the simplification pass suggests.
- **One transform produces the render data.** `build_instances()` turns world
  state into a flat instance buffer (10 floats per quad). JS copies that buffer
  straight to the GPU and issues a single instanced draw. The host does almost
  nothing.
- **Explicit data protocol at the boundary.** C owns every byte of layout; JS
  never computes an offset — it asks C for the pointer or count it needs. The
  exported function list *is* the protocol (see the header of `game.c`).

## The data

| data | type | size |
|------|------|------|
| grid | `u8[20*15]` | 300 B, `0` empty / `1` wall |
| tank x, y | `f32[2]` each | world units |
| tank angle | `u16[2]` | `0..65535`, `>>11` → 1 of 32 directions |
| tank input | `u8[2]` | bits: FWD/BACK/LEFT/RIGHT/FIRE |
| tank vx, vy | `f32[2]` each | last frame's applied move (debug) |
| instances | `f32[(W*H + 4)*10]` | render output: cx,cy,hx,hy,cos,sin,rgba |

Arena is the grid: 20×15 cells, 1 world unit per cell.

## Per-frame transform

1. `set_input(tank, bits)` — host writes keyboard state into `input[]`
2. `tanks_turn` — add/subtract from each `angle` per LEFT/RIGHT bits (wraps)
3. `tanks_move` — step along the heading by `speed*dt`, resolving collision one
   axis at a time so a tank slides along a wall it hits at an angle
4. `build_instances` — world state → flat instance buffer
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
./build.sh        # writes game.wasm (~4 KB)
```

`game.wasm` is committed so GitHub Pages serves it without a build step.

## Verified

- Simulation tested in Node against `game.wasm`: tank placement, exact forward
  motion (2.5 → 6.5 world units over 1 s at speed 4), turning (angle and
  direction index advance), wall collision (stops before interior walls), edge
  clamp, and instance count (`walls + tanks*2`).
- **Not** verified here: the live WebGPU render — no GPU/browser in the build
  environment. The host uses standard WebGPU calls; render-test in a browser.

## Deferred (not built — no speculative generality)

Firing/projectiles, tank-vs-tank collision, scoring, sound, AI, multiple maps.
Each is its own future step.
