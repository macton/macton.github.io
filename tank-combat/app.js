// tank-combat — host: WebGPU renderer + input + debug panel.
//
// The host does as little as possible. All simulation state and all
// transforms live in game.wasm. Each frame the host:
//   1. reads keyboard -> writes each tank's input bitfield (set_input)
//   2. advances the sim one step (tick)
//   3. copies the flat instance buffer wasm produced straight to the GPU
//   4. draws inst_count instanced quads
//   5. reflects wasm memory into the debug panel (and writes edits back)
//
// The wasm linear memory is the single source of truth; the panel reads and
// pokes it directly. Memory never grows, so these typed-array views stay live.

const VERT_WGSL = `
struct View { s: vec2f, o: vec2f };
@group(0) @binding(0) var<uniform> view: View;

struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) col: vec4f,
};

@vertex
fn vs(
  @location(0) corner: vec2f,   // unit quad, -1..1
  @location(1) center: vec2f,
  @location(2) half:   vec2f,
  @location(3) rot:    vec2f,    // (cos, sin)
  @location(4) col:    vec4f,
) -> VOut {
  let l = corner * half;
  let w = center + vec2f(l.x * rot.x - l.y * rot.y,
                         l.x * rot.y + l.y * rot.x);
  var o: VOut;
  o.pos = vec4f(w * view.s + view.o, 0.0, 1.0);
  o.col = col;
  return o;
}

@fragment
fn fs(i: VOut) -> @location(0) vec4f { return i.col; }
`;

// keycode -> (tank index, input bit). Tank 0: WASD/Space. Tank 1: arrows/Enter.
const IN_FWD = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_FIRE = 16;
const KEYMAP = {
  KeyW: [0, IN_FWD],  KeyS: [0, IN_BACK], KeyA: [0, IN_LEFT], KeyD: [0, IN_RIGHT], Space: [0, IN_FIRE],
  ArrowUp: [1, IN_FWD], ArrowDown: [1, IN_BACK], ArrowLeft: [1, IN_LEFT], ArrowRight: [1, IN_RIGHT], Enter: [1, IN_FIRE],
};

const held = new Set();
addEventListener("keydown", (ev) => {
  if (KEYMAP[ev.code]) { held.add(ev.code); ev.preventDefault(); }
});
addEventListener("keyup", (ev) => {
  if (KEYMAP[ev.code]) { held.delete(ev.code); ev.preventDefault(); }
});

async function main() {
  const statusEl = document.getElementById("status");
  if (!navigator.gpu) { statusEl.textContent = "WebGPU not available in this browser."; return; }

  // --- wasm ---------------------------------------------------------------
  // Streaming needs an application/wasm MIME type; fall back to bytes if the
  // host doesn't send it (some static servers, file://).
  let instance;
  try {
    ({ instance } = await WebAssembly.instantiateStreaming(fetch("game.wasm")));
  } catch {
    const bytes = await (await fetch("game.wasm")).arrayBuffer();
    ({ instance } = await WebAssembly.instantiate(bytes));
  }
  const wasm = instance.exports;
  const memU8  = () => new Uint8Array(wasm.memory.buffer);
  const memU16 = () => new Uint16Array(wasm.memory.buffer);
  const memF32 = () => new Float32Array(wasm.memory.buffer);
  wasm.init();

  const NT     = wasm.n_tanks();
  const GW     = wasm.grid_w();
  const GH     = wasm.grid_h();
  const AW     = wasm.arena_w();
  const AH     = wasm.arena_h();
  const STRIDE = wasm.inst_stride();         // floats per instance
  const MAX_INST = GW * GH + NT * 2;

  // views into wasm memory (stable: memory never grows)
  const view = {
    grid:  () => new Uint8Array(wasm.memory.buffer, wasm.grid_ptr(), GW * GH),
    x:     () => new Float32Array(wasm.memory.buffer, wasm.tank_x_ptr(), NT),
    y:     () => new Float32Array(wasm.memory.buffer, wasm.tank_y_ptr(), NT),
    ang:   () => new Uint16Array(wasm.memory.buffer, wasm.tank_angle_ptr(), NT),
    inp:   () => new Uint8Array(wasm.memory.buffer, wasm.tank_input_ptr(), NT),
    vx:    () => new Float32Array(wasm.memory.buffer, wasm.tank_vx_ptr(), NT),
    vy:    () => new Float32Array(wasm.memory.buffer, wasm.tank_vy_ptr(), NT),
    inst:  (n) => new Float32Array(wasm.memory.buffer, wasm.inst_ptr(), n * STRIDE),
  };

  // --- WebGPU -------------------------------------------------------------
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const canvas = document.getElementById("gpu");
  const ctx = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: "opaque" });

  // unit quad (two triangles), -1..1
  const quad = new Float32Array([-1,-1, 1,-1, 1,1,  -1,-1, 1,1, -1,1]);
  const quadBuf = device.createBuffer({ size: quad.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(quadBuf, 0, quad);

  const instBuf = device.createBuffer({ size: MAX_INST * STRIDE * 4, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const viewBuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });

  const module = device.createShaderModule({ code: VERT_WGSL });
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: {
      module, entryPoint: "vs",
      buffers: [
        { arrayStride: 8, stepMode: "vertex", attributes: [{ shaderLocation: 0, offset: 0, format: "float32x2" }] },
        { arrayStride: STRIDE * 4, stepMode: "instance", attributes: [
          { shaderLocation: 1, offset: 0,  format: "float32x2" },
          { shaderLocation: 2, offset: 8,  format: "float32x2" },
          { shaderLocation: 3, offset: 16, format: "float32x2" },
          { shaderLocation: 4, offset: 24, format: "float32x4" },
        ] },
      ],
    },
    fragment: { module, entryPoint: "fs", targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  const bind = device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0),
    entries: [{ binding: 0, resource: { buffer: viewBuf } }],
  });

  // world -> clip uniform, recomputed on resize. Preserves square cells and
  // centers the arena; y is flipped so grid row 0 is at the top.
  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const p = Math.min(canvas.width / AW, canvas.height / AH);  // px per world unit
    const sx = p * 2 / canvas.width;
    const sy = -p * 2 / canvas.height;
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([sx, sy, -sx * AW / 2, -sy * AH / 2]));
  }
  new ResizeObserver(resize).observe(canvas);
  resize();

  // --- debug panel --------------------------------------------------------
  const dbg = buildPanel(wasm, view, { NT, GW, GH });

  // --- frame loop ---------------------------------------------------------
  let paused = false, stepOnce = false, last = performance.now(), fps = 0;
  dbg.onPause = (v) => { paused = v; };
  dbg.onStep  = () => { stepOnce = true; };
  dbg.onReset = () => { wasm.init(); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now;
    if (dt > 1 / 30) dt = 1 / 30;            // clamp huge gaps (tab switch)
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;

    // input -> wasm
    const bits = [0, 0];
    for (const code of held) { const m = KEYMAP[code]; if (m) bits[m[0]] |= m[1]; }
    for (let t = 0; t < NT; t++) wasm.set_input(t, bits[t]);

    if (!paused || stepOnce) { wasm.tick(dt); stepOnce = false; }

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
    pass.setVertexBuffer(0, quadBuf);
    pass.setVertexBuffer(1, instBuf);
    pass.draw(6, n);
    pass.end();
    device.queue.submit([enc.finish()]);

    dbg.update({ fps, dt, instCount: n });
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// Build the debug panel DOM and return { update, onPause, onStep, onReset }.
// The panel reflects wasm memory live and pokes it on edit. Inputs that
// currently have focus are not overwritten, so editing them is possible.
function buildPanel(wasm, view, dims) {
  const { NT, GW, GH } = dims;
  const root = document.getElementById("panel");

  const stats = el("div", "stats");
  root.appendChild(stats);

  // controls
  const ctrls = el("div", "row");
  const bPause = btn("Pause"), bStep = btn("Step"), bReset = btn("Reset");
  ctrls.append(bPause, bStep, bReset);
  root.appendChild(ctrls);

  // tunables
  const tun = el("div", "tunables");
  const speed = numField("move_speed", 0, 40, 0.5, wasm.move_speed());
  const turn  = numField("turn_rate", 0, 60000, 1000, wasm.turn_rate());
  speed.input.addEventListener("change", () => wasm.set_move_speed(parseFloat(speed.input.value)));
  turn.input.addEventListener("change", () => wasm.set_turn_rate(parseInt(turn.input.value) | 0));
  tun.append(speed.wrap, turn.wrap);
  root.appendChild(tun);

  // per-tank editors
  const tankWrap = el("div", "tanks");
  const tankRows = [];
  for (let t = 0; t < NT; t++) {
    const box = el("div", "tankbox");
    const title = el("div", "tt"); title.textContent = `tank ${t}`;
    const fx = numField("x", 0, GW, 0.1, 0);
    const fy = numField("y", 0, GH, 0.1, 0);
    const fa = numField("angle", 0, 65535, 256, 0);
    const live = el("div", "live");
    const apply = () => wasm.set_tank_pose(t, parseFloat(fx.input.value), parseFloat(fy.input.value), parseInt(fa.input.value) | 0);
    fx.input.addEventListener("change", apply);
    fy.input.addEventListener("change", apply);
    fa.input.addEventListener("change", apply);
    box.append(title, fx.wrap, fy.wrap, fa.wrap, live);
    tankWrap.appendChild(box);
    tankRows.push({ fx, fy, fa, live });
  }
  root.appendChild(tankWrap);

  // grid editor (click toggles a wall)
  const gridLbl = el("div", "lbl"); gridLbl.textContent = "grid (click to toggle wall)";
  root.appendChild(gridLbl);
  const gridEl = el("div", "grid");
  gridEl.style.gridTemplateColumns = `repeat(${GW}, 1fr)`;
  const cells = [];
  for (let i = 0; i < GW * GH; i++) {
    const c = el("div", "cell");
    c.addEventListener("click", () => wasm.toggle_wall(i % GW, (i / GW) | 0));
    gridEl.appendChild(c);
    cells.push(c);
  }
  root.appendChild(gridEl);

  const focused = () => document.activeElement;

  function update({ fps, dt, instCount }) {
    const g = view.grid();
    let walls = 0; for (let i = 0; i < g.length; i++) walls += g[i];
    stats.innerHTML =
      `frame <b>${wasm.frame()}</b> &nbsp; ${(dt * 1000).toFixed(1)} ms &nbsp; ${fps.toFixed(0)} fps<br>` +
      `instances <b>${instCount}</b> &nbsp; walls <b>${walls}</b> &nbsp; mem ${(wasm.memory.buffer.byteLength / 1024) | 0} KiB`;

    if (focused() !== speed.input) speed.input.value = wasm.move_speed();
    if (focused() !== turn.input)  turn.input.value  = wasm.turn_rate();

    const x = view.x(), y = view.y(), ang = view.ang(), inp = view.inp(), vx = view.vx(), vy = view.vy();
    for (let t = 0; t < NT; t++) {
      const r = tankRows[t];
      if (focused() !== r.fx.input) r.fx.input.value = x[t].toFixed(3);
      if (focused() !== r.fy.input) r.fy.input.value = y[t].toFixed(3);
      if (focused() !== r.fa.input) r.fa.input.value = ang[t];
      const dir = ang[t] >> 11;
      r.live.textContent =
        `dir ${dir}/32  in ${bits(inp[t])}  v(${vx[t].toFixed(3)}, ${vy[t].toFixed(3)})`;
    }

    for (let i = 0; i < cells.length; i++) cells[i].classList.toggle("on", !!g[i]);
  }

  const api = { update };
  bPause.addEventListener("click", () => { const v = bPause.textContent === "Pause"; bPause.textContent = v ? "Resume" : "Pause"; api.onPause && api.onPause(v); });
  bStep.addEventListener("click", () => api.onStep && api.onStep());
  bReset.addEventListener("click", () => api.onReset && api.onReset());
  return api;

  function bits(v) {
    return ["F", "B", "L", "R", "*"].map((c, i) => (v & (1 << i)) ? c : "·").join("");
  }
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
