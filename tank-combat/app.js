// tank-combat — host: WebGPU renderer + input + debug panel.
//
// The host does as little as possible. All simulation state and all
// transforms live in game.wasm, in integer fixed point. Each frame the host:
//   1. reads keyboard + on-screen pads -> writes each tank's input bitfield
//   2. advances the sim on a fixed timestep (tick takes no dt)
//   3. copies the packed integer instance buffer straight to the GPU
//   4. draws inst_count instanced quads (the shader does the only int->float)
//   5. reflects wasm memory into the debug panel (and writes edits back)
//
// The wasm linear memory is the single source of truth; the panel reads and
// pokes it directly. Memory never grows, so these typed-array views stay live.

const TICK_DT = 1 / 60;   // fixed simulation step

// Instance attributes are integers; the vertex shader is the one place fixed
// point becomes float (the GPU is float hardware — that is its data format).
// pos/half are subcells (256 == 1 cell), rot is Q14, color is unorm8.
const WGSL = `
struct View { s: vec2f, o: vec2f };
@group(0) @binding(0) var<uniform> view: View;

struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f };

const CORNERS = array<vec2f,6>(
  vec2f(-1,-1), vec2f(1,-1), vec2f(1,1),
  vec2f(-1,-1), vec2f(1,1), vec2f(-1,1));

@vertex
fn vs(@builtin(vertex_index) vi: u32,
      @location(0) center: vec2<i32>,
      @location(1) half:   vec2<i32>,
      @location(2) rot:    vec2<i32>,
      @location(3) color:  vec4f) -> VOut {
  let h = vec2f(half) * (1.0 / 256.0);
  let l = CORNERS[vi] * h;
  let r = vec2f(rot) * (1.0 / 16384.0);
  let c = vec2f(center) * (1.0 / 256.0);
  let w = c + vec2f(l.x * r.x - l.y * r.y, l.x * r.y + l.y * r.x);
  var o: VOut;
  o.pos = vec4f(w * view.s + view.o, 0.0, 1.0);
  o.col = color;
  return o;
}

@fragment
fn fs(i: VOut) -> @location(0) vec4f { return i.col; }
`;

const IN_FWD = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_FIRE = 16;
// keycode -> [tank, bit]. Tank 0: WASD/Space. Tank 1: arrows/Enter.
const KEYMAP = {
  KeyW: [0, IN_FWD], KeyS: [0, IN_BACK], KeyA: [0, IN_LEFT], KeyD: [0, IN_RIGHT], Space: [0, IN_FIRE],
  ArrowUp: [1, IN_FWD], ArrowDown: [1, IN_BACK], ArrowLeft: [1, IN_LEFT], ArrowRight: [1, IN_RIGHT], Enter: [1, IN_FIRE],
};

const held = new Set();
addEventListener("keydown", (e) => { if (KEYMAP[e.code]) { held.add(e.code); e.preventDefault(); } });
addEventListener("keyup",   (e) => { if (KEYMAP[e.code]) { held.delete(e.code); e.preventDefault(); } });

// on-screen pads write here; OR'd with keyboard each frame.
const touchBits = [0, 0];

async function main() {
  const statusEl = document.getElementById("status");
  if (!navigator.gpu) { statusEl.textContent = "WebGPU not available in this browser."; return; }

  // --- wasm ---------------------------------------------------------------
  let instance;
  try {
    ({ instance } = await WebAssembly.instantiateStreaming(fetch("game.wasm")));
  } catch {
    const bytes = await (await fetch("game.wasm")).arrayBuffer();
    ({ instance } = await WebAssembly.instantiate(bytes));
  }
  const wasm = instance.exports;
  wasm.init();

  const NT     = wasm.n_tanks();
  const GW     = wasm.grid_w();
  const GH     = wasm.grid_h();
  const SUB    = wasm.subcell();
  const STRIDE = wasm.inst_stride();          // bytes per instance (16)
  const MAX_INST = GW * GH + NT * 2;

  // views into wasm memory (stable: memory never grows)
  const view = {
    grid: () => new Uint32Array(wasm.memory.buffer, wasm.grid_ptr(), GH),  // one word per row, bit c == col c
    x:    () => new Int16Array(wasm.memory.buffer, wasm.tank_x_ptr(), NT),
    y:    () => new Int16Array(wasm.memory.buffer, wasm.tank_y_ptr(), NT),
    ang:  () => new Uint16Array(wasm.memory.buffer, wasm.tank_angle_ptr(), NT),
    inp:  () => new Uint8Array(wasm.memory.buffer, wasm.tank_input_ptr(), NT),
    vx:   () => new Int16Array(wasm.memory.buffer, wasm.tank_vx_ptr(), NT),
    vy:   () => new Int16Array(wasm.memory.buffer, wasm.tank_vy_ptr(), NT),
    hit:  () => new Uint8Array(wasm.memory.buffer, wasm.tank_hit_ptr(), NT),
    inst: (n) => new Uint8Array(wasm.memory.buffer, wasm.inst_ptr(), n * STRIDE),
  };

  // --- WebGPU -------------------------------------------------------------
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const canvas = document.getElementById("gpu");
  const ctx = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: "opaque" });

  const instBuf = device.createBuffer({ size: MAX_INST * STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const viewBuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });

  const module = device.createShaderModule({ code: WGSL });
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: {
      module, entryPoint: "vs",
      buffers: [{
        arrayStride: STRIDE, stepMode: "instance",
        attributes: [
          { shaderLocation: 0, offset: 0,  format: "sint16x2" },  // center
          { shaderLocation: 1, offset: 4,  format: "sint16x2" },  // half
          { shaderLocation: 2, offset: 8,  format: "sint16x2" },  // rot (Q14)
          { shaderLocation: 3, offset: 12, format: "unorm8x4" },  // color
        ],
      }],
    },
    fragment: { module, entryPoint: "fs", targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  const bind = device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0),
    entries: [{ binding: 0, resource: { buffer: viewBuf } }],
  });

  // world(cells) -> clip uniform, recomputed on resize. Square cells, arena
  // centered, y flipped so grid row 0 is at the top.
  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const p = Math.min(canvas.width / GW, canvas.height / GH);  // px per cell
    const sx = p * 2 / canvas.width, sy = -p * 2 / canvas.height;
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([sx, sy, -sx * GW / 2, -sy * GH / 2]));
  }
  new ResizeObserver(resize).observe(canvas);
  resize();

  buildTouchPads(document.getElementById("map"), NT);
  const dbg = buildPanel(wasm, view, { NT, GW, GH, SUB });

  statusEl.style.display = "none";   // done loading

  // --- frame loop (fixed timestep) ---------------------------------------
  let paused = false, stepOnce = false, last = performance.now(), fps = 60, acc = 0;
  dbg.onPause = (v) => { paused = v; };
  dbg.onStep  = () => { stepOnce = true; };
  dbg.onReset = () => { wasm.init(); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now;
    if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;

    const bits = [0, 0];
    for (const code of held) { const m = KEYMAP[code]; if (m) bits[m[0]] |= m[1]; }
    bits[0] |= touchBits[0]; bits[1] |= touchBits[1];
    for (let t = 0; t < NT; t++) wasm.set_input(t, bits[t]);

    if (stepOnce) { wasm.tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { wasm.tick(); acc -= TICK_DT; s++; } }
    else { acc = 0; }

    const n = wasm.inst_count();
    device.queue.writeBuffer(instBuf, 0, view.inst(n));

    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.10, g: 0.11, b: 0.13, a: 1 },
        loadOp: "clear", storeOp: "store",
      }],
    });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bind);
    pass.setVertexBuffer(0, instBuf);
    pass.draw(6, n);
    pass.end();
    device.queue.submit([enc.finish()]);

    dbg.update({ fps, dt, instCount: n });
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// Two on-screen D-pads overlaid on the map corners: tank 0 left, tank 1 right.
// Hold-to-press via pointer events, so it works for touch and mouse.
function buildTouchPads(map, NT) {
  const layout = [
    [IN_FWD, "▲", "u"], [IN_LEFT, "◄", "l"], [IN_RIGHT, "►", "r"], [IN_BACK, "▼", "d"],
  ];
  for (let t = 0; t < NT && t < 2; t++) {
    const pad = el("div", "pad " + (t === 0 ? "left" : "right"));
    for (const [bit, glyph, pos] of layout) {
      const b = el("button", "padbtn " + pos);
      b.textContent = glyph;
      const on = (e) => { touchBits[t] |= bit; e.preventDefault(); };
      const off = (e) => { touchBits[t] &= ~bit; e.preventDefault(); };
      b.addEventListener("pointerdown", on);
      b.addEventListener("pointerup", off);
      b.addEventListener("pointercancel", off);
      b.addEventListener("pointerleave", off);
      pad.appendChild(b);
    }
    map.appendChild(pad);
  }
}

// Debug panel: reflects wasm memory live and pokes it on edit. Inputs that
// currently have focus are not overwritten, so editing them works.
function buildPanel(wasm, view, dims) {
  const { NT, GW, GH, SUB } = dims;
  const root = document.getElementById("panel");

  const stats = el("div", "stats"); root.appendChild(stats);

  const ctrls = el("div", "row");
  const bPause = btn("Pause"), bStep = btn("Step"), bReset = btn("Reset");
  ctrls.append(bPause, bStep, bReset); root.appendChild(ctrls);

  const tun = el("div", "tunables");
  const speed = numField("move_speed (sub/tick)", 0, 256, 1, wasm.move_speed());
  const turn  = numField("turn_rate (ang/tick)", 0, 4000, 25, wasm.turn_rate());
  speed.input.addEventListener("change", () => wasm.set_move_speed(parseInt(speed.input.value) | 0));
  turn.input.addEventListener("change", () => wasm.set_turn_rate(parseInt(turn.input.value) | 0));
  tun.append(speed.wrap, turn.wrap); root.appendChild(tun);

  const tankWrap = el("div", "tanks");
  const rows = [];
  for (let t = 0; t < NT; t++) {
    const box = el("div", "tankbox");
    const title = el("div", "tt"); title.textContent = `tank ${t}`;
    const fx = numField("x (cells)", 0, GW, 0.1, 0);
    const fy = numField("y (cells)", 0, GH, 0.1, 0);
    const fa = numField("angle (u16)", 0, 65535, 256, 0);
    const live = el("div", "live");
    const apply = () => wasm.set_tank_pose(t,
      Math.round(parseFloat(fx.input.value) * SUB),
      Math.round(parseFloat(fy.input.value) * SUB),
      parseInt(fa.input.value) | 0);
    fx.input.addEventListener("change", apply);
    fy.input.addEventListener("change", apply);
    fa.input.addEventListener("change", apply);
    box.append(title, fx.wrap, fy.wrap, fa.wrap, live);
    tankWrap.appendChild(box);
    rows.push({ fx, fy, fa, live });
  }
  root.appendChild(tankWrap);

  const gridLbl = el("div", "lbl"); gridLbl.textContent = "grid (click to toggle wall)";
  root.appendChild(gridLbl);
  const gridEl = el("div", "grid");
  gridEl.style.gridTemplateColumns = `repeat(${GW}, 1fr)`;
  const cells = [];
  for (let i = 0; i < GW * GH; i++) {
    const c = el("div", "cell");
    c.addEventListener("click", () => wasm.toggle_wall(i % GW, (i / GW) | 0));
    gridEl.appendChild(c); cells.push(c);
  }
  root.appendChild(gridEl);

  const focused = () => document.activeElement;
  const inBits = (v) => ["F", "B", "L", "R", "*"].map((c, i) => (v & (1 << i)) ? c : "·").join("");

  function update({ fps, dt, instCount }) {
    const g = view.grid();   // row bitsets
    let walls = 0; for (let r = 0; r < GH; r++) { let v = g[r]; while (v) { v &= v - 1; walls++; } }
    stats.innerHTML =
      `frame <b>${wasm.frame()}</b> &nbsp; ${(dt * 1000).toFixed(1)} ms &nbsp; ${fps.toFixed(0)} fps<br>` +
      `instances <b>${instCount}</b> &nbsp; walls <b>${walls}</b> &nbsp; mem ${(wasm.memory.buffer.byteLength / 1024) | 0} KiB`;

    if (focused() !== speed.input) speed.input.value = wasm.move_speed();
    if (focused() !== turn.input)  turn.input.value  = wasm.turn_rate();

    const x = view.x(), y = view.y(), ang = view.ang(), inp = view.inp(), vx = view.vx(), vy = view.vy(), hit = view.hit();
    for (let t = 0; t < NT; t++) {
      const r = rows[t];
      if (focused() !== r.fx.input) r.fx.input.value = (x[t] / SUB).toFixed(3);
      if (focused() !== r.fy.input) r.fy.input.value = (y[t] / SUB).toFixed(3);
      if (focused() !== r.fa.input) r.fa.input.value = ang[t];
      r.live.textContent =
        `sub(${x[t]},${y[t]})  dir ${ang[t] >> 11}/32  in ${inBits(inp[t])}  v(${vx[t]},${vy[t]})  hit ${hit[t]}`;
    }
    for (let i = 0; i < cells.length; i++)
      cells[i].classList.toggle("on", !!((g[(i / GW) | 0] >>> (i % GW)) & 1));
  }

  const api = { update };
  bPause.addEventListener("click", () => { const v = bPause.textContent === "Pause"; bPause.textContent = v ? "Resume" : "Pause"; api.onPause && api.onPause(v); });
  bStep.addEventListener("click", () => api.onStep && api.onStep());
  bReset.addEventListener("click", () => api.onReset && api.onReset());
  return api;
}

// --- tiny DOM helpers ------------------------------------------------------
function el(tag, cls) { const e = document.createElement(tag); if (cls) e.className = cls; return e; }
function btn(label) { const b = el("button"); b.textContent = label; return b; }
function numField(name, min, max, step, val) {
  const wrap = el("label", "nf");
  const span = el("span"); span.textContent = name;
  const input = el("input"); input.type = "number"; input.min = min; input.max = max; input.step = step; input.value = val;
  wrap.append(span, input);
  return { wrap, input, span };
}

main();
