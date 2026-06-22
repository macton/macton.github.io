// tank-combat chapter 2 — host: WebGPU renderer + input + pathing debug panel.
//
// As in chapter 1, all simulation state and all transforms live in game.wasm, in
// integer fixed point. The host reads keyboard/touch -> the manual tank's input,
// ticks the sim, copies the packed instance buffer to the GPU, and reflects wasm
// memory into the live panel. New in chapter 2: a one-screen viewport with
// scroll buttons, click-to-select / click-to-set-destination for pathed tanks,
// and widgets over the path tables (Level-1 flow field, edge points, next-hop).
//
// The wasm builds the instance buffer in SCREEN-LOCAL coordinates for the camera
// screen, so the renderer/view transform here is identical to chapter 1 — the
// canvas is one GRID_W x GRID_H arena; only the camera changes what is drawn.

const TICK_DT = 1 / 60;

const WGSL = `
struct View { s: vec2f, o: vec2f };
@group(0) @binding(0) var<uniform> view: View;
struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f };
const CORNERS = array<vec2f,6>(
  vec2f(-1,-1), vec2f(1,-1), vec2f(1,1),
  vec2f(-1,-1), vec2f(1,1), vec2f(-1,1));
@vertex
fn vs(@builtin(vertex_index) vi: u32,
      @location(0) center: vec2<i32>, @location(1) half: vec2<i32>,
      @location(2) rot: vec2<i32>, @location(3) color: vec4f) -> VOut {
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

const IN_FWD = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8;
// only the manual tank (index 0) takes keyboard/pad input.
const KEYMAP = { KeyW: IN_FWD, KeyS: IN_BACK, KeyA: IN_LEFT, KeyD: IN_RIGHT };
const held = new Set();
addEventListener("keydown", (e) => { if (KEYMAP[e.code]) { held.add(e.code); e.preventDefault(); } });
addEventListener("keyup",   (e) => { if (KEYMAP[e.code]) { held.delete(e.code); e.preventDefault(); } });
let touchBits = 0;

const DIR_GLYPH = ["↑", "→", "↓", "←"];           // N E S W
const STATUS = ["idle", "routing", "arrived", "no path"];

async function main() {
  const VERSION = (typeof window !== "undefined" && window.TANK_VERSION) || "dev";
  const vEl = document.getElementById("version");
  if (vEl) vEl.textContent = "v" + VERSION;
  const statusEl = document.getElementById("status");
  if (!navigator.gpu) { if (statusEl) statusEl.textContent = "WebGPU not available in this browser."; return; }

  // --- wasm ---------------------------------------------------------------
  const url = "game.wasm?v=" + encodeURIComponent(VERSION);
  let instance;
  try { ({ instance } = await WebAssembly.instantiateStreaming(fetch(url))); }
  catch { const bytes = await (await fetch(url)).arrayBuffer(); ({ instance } = await WebAssembly.instantiate(bytes)); }
  const wasm = instance.exports;
  wasm.init();

  const C = {
    NT: wasm.n_tanks(), NP: wasm.n_pathed(), GW: wasm.grid_w(), GH: wasm.grid_h(),
    NC: wasm.n_cells(), SX: wasm.screens_x(), SY: wasm.screens_y(), NS: wasm.n_screens(),
    BW: wasm.big_w(), BH: wasm.big_h(), SUB: wasm.subcell(), TRI: wasm.tri(),
    NE: wasm.n_edge_max(), EPS: wasm.ep_per_screen_max(),
    STRIDE: wasm.inst_stride(), MAX_INST: wasm.inst_max(), DYN: wasm.dyn_count(),
  };

  // live typed-array views into wasm memory (stable: memory never grows)
  const mem = () => wasm.memory.buffer;
  const view = {
    grid: () => new Uint32Array(mem(), wasm.grid_ptr(), C.NS * C.GH),
    xy:   () => new Int16Array(mem(), wasm.tank_xy_ptr(), C.NT * 2),
    ang:  () => new Uint16Array(mem(), wasm.tank_angle_ptr(), C.NT),
    inp:  () => new Uint8Array(mem(), wasm.tank_input_ptr(), C.NT),
    hit:  () => new Uint8Array(mem(), wasm.tank_hit_ptr(), C.NT),
    l1:   () => new Uint16Array(mem(), wasm.l1dist_ptr(), C.NS * C.TRI),
    epScreen: () => new Uint8Array(mem(), wasm.ep_screen_ptr(), C.NE),
    epCell:   () => new Uint16Array(mem(), wasm.ep_cell_ptr(), C.NE),
    epCross:  () => new Uint8Array(mem(), wasm.ep_cross_ptr(), C.NE),
    epPart:   () => new Uint8Array(mem(), wasm.ep_partner_ptr(), C.NE),
    sepCount: () => new Uint8Array(mem(), wasm.screen_ep_count_ptr(), C.NS),
    sep:      () => new Uint8Array(mem(), wasm.screen_ep_ptr(), C.NS * C.EPS),
    dist2:    () => new Uint16Array(mem(), wasm.dist2_ptr(), C.NE * C.NE),
    nexthop:  () => new Uint8Array(mem(), wasm.nexthop_ptr(), C.NE * C.NE),
    pdScreen: () => new Uint8Array(mem(), wasm.pdest_screen_ptr(), C.NP),
    pdCell:   () => new Uint16Array(mem(), wasm.pdest_cell_ptr(), C.NP),
    phas:     () => new Uint8Array(mem(), wasm.phas_ptr(), C.NP),
    pgoal:    () => new Uint8Array(mem(), wasm.pgoal_ptr(), C.NP),
    pstatus:  () => new Uint8Array(mem(), wasm.pstatus_ptr(), C.NP),
    instBytes: (start, n) => new Uint8Array(mem(), wasm.inst_ptr() + start * C.STRIDE, n * C.STRIDE),
  };

  // --- pathing read helpers (mirror grid_paths.c, for the flow-field widget) --
  const triIndex = (a, b) => { if (a > b) { const t = a; a = b; b = t; } return a * (2 * C.NC - a + 1) / 2 + (b - a); };
  const l1get = (l1, s, a, b) => l1[s * C.TRI + triIndex(a, b)];
  const DCX = [0, 1, 0, -1], DCY = [-1, 0, 1, 0];
  function l1nextdir(l1, s, src, dst) {
    if (src === dst) return -1;
    const d0 = l1get(l1, s, src, dst); if (d0 === 0xFFFF) return -1;
    const scx = src % C.GW, scy = (src / C.GW) | 0;
    for (let d = 0; d < 4; d++) {
      const nx = scx + DCX[d], ny = scy + DCY[d];
      if (nx < 0 || nx >= C.GW || ny < 0 || ny >= C.GH) continue;
      if (l1get(l1, s, ny * C.GW + nx, dst) === d0 - 1) return d;
    }
    return -1;
  }

  // --- WebGPU (one-screen arena view, as chapter 1) -----------------------
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const canvas = document.getElementById("gpu");
  const ctx = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: "opaque" });
  const instBuf = device.createBuffer({ size: C.MAX_INST * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const viewBuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const module = device.createShaderModule({ code: WGSL });
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: { module, entryPoint: "vs", buffers: [{
      arrayStride: C.STRIDE, stepMode: "instance",
      attributes: [
        { shaderLocation: 0, offset: 0, format: "sint16x2" },
        { shaderLocation: 1, offset: 4, format: "sint16x2" },
        { shaderLocation: 2, offset: 8, format: "sint16x2" },
        { shaderLocation: 3, offset: 12, format: "unorm8x4" },
      ] }] },
    fragment: { module, entryPoint: "fs", targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  const bind = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: viewBuf } }] });

  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const p = Math.min(canvas.width / C.GW, canvas.height / C.GH);
    const sx = p * 2 / canvas.width, sy = -p * 2 / canvas.height;
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([sx, sy, -sx * C.GW / 2, -sy * C.GH / 2]));
  }
  new ResizeObserver(resize).observe(canvas);
  resize();

  // --- viewport: scroll buttons + click-to-select / click-to-set-dest ------
  const scrollOne = (dx, dy) => wasm.scroll(dx, dy);
  document.querySelector(".scrollbtn.su").onclick = () => scrollOne(0, -1);
  document.querySelector(".scrollbtn.sd").onclick = () => scrollOne(0, 1);
  document.querySelector(".scrollbtn.sl").onclick = () => scrollOne(-1, 0);
  document.querySelector(".scrollbtn.sr").onclick = () => scrollOne(1, 0);

  function clickCell(e) {
    const r = canvas.getBoundingClientRect();
    const p = Math.min(r.width / C.GW, r.height / C.GH);
    const x0 = (r.width - p * C.GW) / 2, y0 = (r.height - p * C.GH) / 2;
    const lcx = Math.floor((e.clientX - r.left - x0) / p);
    const lcy = Math.floor((e.clientY - r.top - y0) / p);
    if (lcx < 0 || lcx >= C.GW || lcy < 0 || lcy >= C.GH) return;
    const wcx = wasm.cam_sx() * C.GW + lcx, wcy = wasm.cam_sy() * C.GH + lcy;
    // clicking a pathed tank selects it; otherwise set the selected tank's dest
    const xy = view.xy();
    for (let t = 1; t < C.NT; t++) {
      if ((xy[2 * t] >> 8) === wcx && (xy[2 * t + 1] >> 8) === wcy) { wasm.set_selected(t); return; }
    }
    wasm.set_dest(wasm.selected(), wcx, wcy);
  }
  canvas.addEventListener("click", clickCell);

  buildTouchPad(document.getElementById("map"));
  const dbg = mountWidgets(wasm, view, C, { l1get, l1nextdir });
  if (statusEl) statusEl.style.display = "none";

  // --- frame loop ---------------------------------------------------------
  let paused = false, stepOnce = false, last = performance.now(), fps = 60, acc = 0;
  let lastWallsVer = -1;
  dbg.onPause = (v) => { paused = v; };
  dbg.onStep = () => { stepOnce = true; };
  dbg.onReset = () => { wasm.init(); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now;
    if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;

    let bits = 0;
    for (const code of held) bits |= KEYMAP[code];
    bits |= touchBits;
    wasm.set_input(0, bits);                 // the single control set drives the manual tank

    if (stepOnce) { wasm.tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { wasm.tick(); acc -= TICK_DT; s++; } }
    else { acc = 0; }

    device.queue.writeBuffer(instBuf, 0, view.instBytes(0, C.DYN));
    const wv = wasm.walls_version();
    if (wv !== lastWallsVer) {
      lastWallsVer = wv;
      device.queue.writeBuffer(instBuf, wasm.wall_base() * C.STRIDE, view.instBytes(wasm.wall_base(), wasm.wall_count()));
    }
    const n = wasm.inst_count();
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(),
      clearValue: { r: 0.10, g: 0.11, b: 0.13, a: 1 }, loadOp: "clear", storeOp: "store" }] });
    pass.setPipeline(pipeline); pass.setBindGroup(0, bind); pass.setVertexBuffer(0, instBuf);
    pass.draw(6, n); pass.end();
    device.queue.submit([enc.finish()]);

    document.getElementById("camlabel").textContent = `screen ${wasm.cam_sx()},${wasm.cam_sy()}`;
    dbg.update({ fps, dt, instCount: n, wallsVer: wv });
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// one D-pad for the manual tank (hold-to-press)
function buildTouchPad(map) {
  const IN = { u: 1, d: 2, l: 4, r: 8 };
  const pad = el("div", "pad");
  for (const [glyph, pos, bit] of [["▲", "u", IN.u], ["◄", "l", IN.l], ["►", "r", IN.r], ["▼", "d", IN.d]]) {
    const b = el("button", "padbtn " + pos); b.textContent = glyph;
    const on = (e) => { touchBits |= bit; e.preventDefault(); e.stopPropagation(); };
    const off = (e) => { touchBits &= ~bit; e.preventDefault(); e.stopPropagation(); };
    b.addEventListener("pointerdown", on); b.addEventListener("pointerup", off);
    b.addEventListener("pointercancel", off); b.addEventListener("pointerleave", off);
    pad.appendChild(b);
  }
  map.appendChild(pad);
}

function mountWidgets(wasm, view, C, P) {
  const updaters = [];
  const api = { update(info) { for (const u of updaters) u(info); } };
  const focused = () => document.activeElement;
  const inBits = (v) => ["F", "B", "L", "R"].map((c, i) => (v & (1 << i)) ? c : "·").join("");
  const at = (id) => document.getElementById(id);
  const TANK_COLORS = ["#f29e29", "#4db3e6", "#78cd78", "#c48ceb"];

  // controls: pause / step / reset + show-path toggle
  const cc = at("w-controls");
  if (cc) {
    cc.classList.add("row");
    const bPause = btn("Pause"), bStep = btn("Step"), bReset = btn("Reset");
    cc.append(bPause, bStep, bReset);
    bPause.onclick = () => { const v = bPause.textContent === "Pause"; bPause.textContent = v ? "Resume" : "Pause"; api.onPause && api.onPause(v); };
    bStep.onclick = () => api.onStep && api.onStep();
    bReset.onclick = () => api.onReset && api.onReset();
    const lab = el("label", "chk");
    const cb = el("input"); cb.type = "checkbox"; cb.checked = true;
    cb.onchange = () => wasm.set_show_path(cb.checked ? 1 : 0);
    lab.append(cb, document.createTextNode("show path")); cc.append(lab);
  }

  // 4x4 minimap of the world
  const mm = at("w-minimap");
  if (mm) {
    const cells = [];
    for (let i = 0; i < C.NS; i++) {
      const s = el("div", "scr");
      const sx = i % C.SX, sy = (i / C.SX) | 0;
      s.onclick = () => wasm.scroll(sx - wasm.cam_sx(), sy - wasm.cam_sy());
      mm.appendChild(s); cells.push(s);
    }
    updaters.push(() => {
      const xy = view.xy();
      const cam = wasm.cam_sy() * C.SX + wasm.cam_sx();
      for (let i = 0; i < C.NS; i++) { cells[i].classList.toggle("cam", i === cam); cells[i].textContent = ""; }
      for (let t = 0; t < C.NT; t++) {
        const wcx = (xy[2 * t] >> 8), wcy = (xy[2 * t + 1] >> 8);
        const s = ((wcy / C.GH) | 0) * C.SX + ((wcx / C.GW) | 0);
        const d = el("div", "dot"); d.style.background = TANK_COLORS[t] || "#fff";
        d.style.left = (((wcx % C.GW) + 0.5) / C.GW * 100) + "%";
        d.style.top = (((wcy % C.GH) + 0.5) / C.GH * 100) + "%";
        cells[s].appendChild(d);
      }
    });
  }

  // stats
  const sc = at("w-stats");
  if (sc) {
    sc.classList.add("stats");
    updaters.push(({ fps, dt, instCount }) => {
      sc.innerHTML =
        `frame <b>${wasm.frame()}</b> · ${(dt * 1000).toFixed(1)} ms · ${fps.toFixed(0)} fps · ` +
        `viewport <b>${wasm.cam_sx()},${wasm.cam_sy()}</b><br>` +
        `world <b>${C.BW}×${C.BH}</b> cells · edge points <b>${wasm.ep_count()}</b> · ` +
        `instances <b>${instCount}</b> · mem <b>${(wasm.memory.buffer.byteLength / 1048576).toFixed(1)}</b> MiB`;
    });
  }

  // per-tank readout (class + goal + current dir lookup) + pose editors
  const kc = at("w-tanks");
  if (kc) {
    const rows = [];
    for (let t = 0; t < C.NT; t++) {
      const box = el("div", "tankbox t" + t);
      const title = el("div", "tt"); title.textContent = `tank ${t} — ${t === 0 ? "manual" : "pathed"}`;
      const fx = numField("x (cells)", 0, C.BW, 0.1, 0);
      const fy = numField("y (cells)", 0, C.BH, 0.1, 0);
      const fa = numField("angle (u16)", 0, 65535, 256, 0);
      const apply = () => wasm.set_tank_pose(t, Math.round(parseFloat(fx.input.value) * C.SUB), Math.round(parseFloat(fy.input.value) * C.SUB), parseInt(fa.input.value) | 0);
      fx.input.onchange = apply; fy.input.onchange = apply; fa.input.onchange = apply;
      const live = el("div", "live");
      box.append(title, fx.wrap, fy.wrap, fa.wrap, live);
      if (t >= 1) {
        const r = el("div", "row"); r.style.marginTop = "6px";
        const bSel = btn("select"), bClr = btn("clear dest");
        bSel.onclick = () => wasm.set_selected(t);
        bClr.onclick = () => wasm.clear_dest(t);
        r.append(bSel, bClr); box.append(r);
      }
      kc.appendChild(box); rows.push({ fx, fy, fa, live, title });
    }
    updaters.push(() => {
      const xy = view.xy(), ang = view.ang(), inp = view.inp(), hit = view.hit();
      const phas = view.phas(), pds = view.pdScreen(), pdc = view.pdCell(), pg = view.pgoal(), pst = view.pstatus();
      const sel = wasm.selected();
      for (let t = 0; t < C.NT; t++) {
        const r = rows[t], x = xy[2 * t], y = xy[2 * t + 1];
        const wcx = x >> 8, wcy = y >> 8;
        const sx = (wcx / C.GW) | 0, sy = (wcy / C.GH) | 0, cx = wcx % C.GW, cy = wcy % C.GH;
        if (focused() !== r.fx.input) r.fx.input.value = (x / C.SUB).toFixed(2);
        if (focused() !== r.fy.input) r.fy.input.value = (y / C.SUB).toFixed(2);
        if (focused() !== r.fa.input) r.fa.input.value = ang[t];
        r.title.textContent = `tank ${t} — ${t === 0 ? "manual" : "pathed"}${t === sel ? "  ◀ selected" : ""}`;
        let s = `world cell (${wcx},${wcy}) = screen ${sx},${sy} cell ${cx},${cy}\ndir ${ang[t] >> 11}/32  in ${inBits(inp[t])}  hit ${hit[t]}`;
        if (t >= 1) {
          const p = t - 1;
          if (phas[p]) {
            const dsx = pds[p] % C.SX, dsy = (pds[p] / C.SX) | 0, dcx = pdc[p] % C.GW, dcy = (pdc[p] / C.GW) | 0;
            const g = pg[p] === 0xFF ? "—" : DIR_GLYPH[pg[p]];
            s += `\ndest screen ${dsx},${dsy} cell ${dcx},${dcy}  status ${STATUS[pst[p]]}  next ${g}`;
          } else s += `\nno destination`;
        }
        r.live.textContent = s;
      }
    });
  }

  // tunables
  const tc = at("w-tunables");
  if (tc) {
    const speed = numField("move_speed (sub/tick)", 0, 256, 1, wasm.move_speed());
    const turn = numField("turn_rate (ang/tick)", 0, 8000, 64, wasm.turn_rate());
    const coll = numField("collide_scale (/256)", 0, 256, 16, wasm.collide_scale());
    speed.input.onchange = () => wasm.set_move_speed(parseInt(speed.input.value) | 0);
    turn.input.onchange = () => wasm.set_turn_rate(parseInt(turn.input.value) | 0);
    coll.input.onchange = () => wasm.set_collide_scale(parseInt(coll.input.value) | 0);
    tc.append(speed.wrap, turn.wrap, coll.wrap);
    updaters.push(() => {
      if (focused() !== speed.input) speed.input.value = wasm.move_speed();
      if (focused() !== turn.input) turn.input.value = wasm.turn_rate();
      if (focused() !== coll.input) coll.input.value = wasm.collide_scale();
    });
  }

  // camera-screen wall editor (toggles walls in the current screen)
  const gc = at("w-grid");
  if (gc) {
    gc.classList.add("cellgrid"); gc.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = [];
    for (let i = 0; i < C.NC; i++) {
      const c = el("div", "cell");
      const lcx = i % C.GW, lcy = (i / C.GW) | 0;
      c.onclick = () => wasm.toggle_wall(wasm.cam_sx() * C.GW + lcx, wasm.cam_sy() * C.GH + lcy);
      gc.appendChild(c); cells.push(c);
    }
    let lastKey = "";
    updaters.push(() => {
      const key = wasm.cam_sx() + "," + wasm.cam_sy() + "," + wasm.walls_version();
      if (key === lastKey) return; lastKey = key;
      const g = view.grid(), s = wasm.cam_sy() * C.SX + wasm.cam_sx();
      for (let i = 0; i < C.NC; i++) cells[i].classList.toggle("on", !!((g[s * C.GH + ((i / C.GW) | 0)] >>> (i % C.GW)) & 1));
    });
  }

  // Level-1 flow field viewer
  const fc = at("w-flow");
  if (fc) {
    let fScreen = 0, fTarget = 7 * C.GW + 10, lastKey = "";   // default: centre cell of screen 0
    const head = el("div", "row"); head.style.marginBottom = "6px";
    const selScr = el("select");
    for (let s = 0; s < C.NS; s++) { const o = el("option"); o.value = s; o.textContent = `screen ${s % C.SX},${(s / C.SX) | 0}`; selScr.appendChild(o); }
    selScr.onchange = () => { fScreen = parseInt(selScr.value) | 0; lastKey = ""; };
    const hint = el("span"); hint.style.cssText = "color:var(--dim);font-size:13px;align-self:center";
    hint.textContent = "click a cell = target";
    head.append(selScr, hint);
    const grid = el("div", "cellgrid"); grid.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = [];
    for (let i = 0; i < C.NC; i++) {
      const c = el("div", "cell");
      c.onclick = () => { fTarget = i; lastKey = ""; };
      grid.appendChild(c); cells.push(c);
    }
    fc.append(head, grid);
    updaters.push(() => {
      const key = fScreen + "," + fTarget + "," + wasm.walls_version();
      if (key === lastKey) return; lastKey = key;
      const l1 = view.l1();
      for (let i = 0; i < C.NC; i++) {
        const c = cells[i]; c.className = "cell";
        if (i === fTarget) { c.classList.add("tgt"); c.textContent = "◎"; c.title = "target"; continue; }
        if (P.l1get(l1, fScreen, i, i) === 0xFFFF) { c.classList.add("on"); c.textContent = ""; c.title = "wall"; continue; }
        const d = P.l1get(l1, fScreen, i, fTarget);
        if (d === 0xFFFF) { c.textContent = "·"; c.title = "unreachable"; continue; }
        const dir = P.l1nextdir(l1, fScreen, i, fTarget);
        c.classList.add("arr"); c.textContent = dir < 0 ? "·" : DIR_GLYPH[dir]; c.title = "dist " + d;
      }
    });
  }

  // edge-point list
  const ec = at("w-edges");
  if (ec) {
    ec.classList.add("elist");
    let lastVer = -1;
    updaters.push(({ wallsVer }) => {
      if (wallsVer === lastVer) return; lastVer = wallsVer;
      const n = wasm.ep_count(), es = view.epScreen(), elc = view.epCell(), cr = view.epCross(), pa = view.epPart();
      let h = "";
      for (let i = 0; i < n; i++) {
        const sx = es[i] % C.SX, sy = (es[i] / C.SX) | 0, cx = elc[i] % C.GW, cy = (elc[i] / C.GW) | 0;
        h += `<div><b>#${i}</b> screen ${sx},${sy} cell ${cx},${cy} · cross ${DIR_GLYPH[cr[i]]} · partner <b>#${pa[i] === 0xFF ? "—" : pa[i]}</b></div>`;
      }
      ec.innerHTML = h || "<div>no edge points</div>";
    });
  }

  // next-hop matrix (row for a chosen source edge point)
  const nc = at("w-nexthop");
  if (nc) {
    const head = el("div", "row"); head.style.marginBottom = "6px";
    const sel = el("select"); const lab = el("span");
    lab.style.cssText = "color:var(--dim);font-size:13px;align-self:center"; lab.textContent = "next-hop / dist from edge point";
    head.append(lab, sel);
    const box = el("div", "matrix"); nc.append(head, box);
    let lastVer = -1, src = 0, built = -1;
    const fill = () => {
      const n = wasm.ep_count();
      if (built !== n) { sel.innerHTML = ""; for (let i = 0; i < n; i++) { const o = el("option"); o.value = i; o.textContent = "#" + i; sel.appendChild(o); } built = n; if (src >= n) src = 0; sel.value = src; }
      const d2 = view.dist2(), nh = view.nexthop();
      let h = "<table class='mx'><tr><th>to B</th><th>dist2</th><th>next C</th></tr>";
      for (let b = 0; b < n; b++) {
        const dd = d2[src * C.NE + b], nn = nh[src * C.NE + b];
        h += `<tr><td>#${b}</td><td>${dd === 0xFFFF ? "∞" : dd}</td><td>${nn === 0xFF ? "—" : "#" + nn}</td></tr>`;
      }
      box.innerHTML = h + "</table>";
    };
    sel.onchange = () => { src = parseInt(sel.value) | 0; fill(); };
    updaters.push(({ wallsVer }) => { if (wallsVer !== lastVer) { lastVer = wallsVer; fill(); } });
  }

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
