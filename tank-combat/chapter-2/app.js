// tank-combat chapter 2 — host: WebGPU renderer + input + pathing debug panel.
//
// All simulation + transforms live in game.wasm (integer fixed point). The host
// reads input, ticks the sim, uploads the packed instance buffer, draws it, and
// reflects wasm memory into the live panel. The wasm builds the view (one screen,
// or two during a slide) in screen-local subcells; the host drives the camera:
// it FOLLOWS the selected tank and SLIDES the new screen in when it crosses a
// border, and a screen PICKER lets you look around the world.

const TICK_DT = 1 / 60;
const SLIDE_DUR = 0.28;

const WGSL = `
struct View { s: vec2f, o: vec2f };
@group(0) @binding(0) var<uniform> view: View;
struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f };
const CORNERS = array<vec2f,6>(vec2f(-1,-1),vec2f(1,-1),vec2f(1,1),vec2f(-1,-1),vec2f(1,1),vec2f(-1,1));
@vertex fn vs(@builtin(vertex_index) vi: u32, @location(0) center: vec2<i32>,
      @location(1) half: vec2<i32>, @location(2) rot: vec2<i32>, @location(3) color: vec4f) -> VOut {
  let h = vec2f(half) * (1.0/256.0); let l = CORNERS[vi]*h; let r = vec2f(rot)*(1.0/16384.0);
  let c = vec2f(center)*(1.0/256.0);
  let w = c + vec2f(l.x*r.x - l.y*r.y, l.x*r.y + l.y*r.x);
  var o: VOut; o.pos = vec4f(w*view.s + view.o, 0.0, 1.0); o.col = color; return o;
}
@fragment fn fs(i: VOut) -> @location(0) vec4f { return i.col; }`;

const IN_FWD = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8;
const KEYMAP = { KeyW: IN_FWD, KeyS: IN_BACK, KeyA: IN_LEFT, KeyD: IN_RIGHT,
                 ArrowUp: IN_FWD, ArrowDown: IN_BACK, ArrowLeft: IN_LEFT, ArrowRight: IN_RIGHT };
const held = new Set();
addEventListener("keydown", (e) => { if (KEYMAP[e.code]) { held.add(e.code); e.preventDefault(); } });
addEventListener("keyup",   (e) => { if (KEYMAP[e.code]) { held.delete(e.code); e.preventDefault(); } });
let touchBits = 0;

const DIR_GLYPH = ["↑", "→", "↓", "←"];
const STATE = ["unselected", "auto-path", "manual"];
const STATUS = ["idle", "routing", "arrived", "no path"];
const sdir = (a, b, n) => (a === b ? 0 : ((b - a + n) % n === 1 ? 1 : -1));  // toroidal one-step

async function main() {
  const VERSION = (typeof window !== "undefined" && window.TANK_VERSION) || "dev";
  const vEl = document.getElementById("version"); if (vEl) vEl.textContent = "v" + VERSION;
  const statusEl = document.getElementById("status");
  if (!navigator.gpu) { if (statusEl) statusEl.textContent = "WebGPU not available in this browser."; return; }

  const url = "game.wasm?v=" + encodeURIComponent(VERSION);
  let instance;
  try { ({ instance } = await WebAssembly.instantiateStreaming(fetch(url))); }
  catch { ({ instance } = await WebAssembly.instantiate(await (await fetch(url)).arrayBuffer())); }
  const wasm = instance.exports; wasm.init();

  const C = {
    NT: wasm.n_tanks(), GW: wasm.grid_w(), GH: wasm.grid_h(), NC: wasm.n_cells(),
    SX: wasm.screens_x(), SY: wasm.screens_y(), NS: wasm.n_screens(), BW: wasm.big_w(), BH: wasm.big_h(),
    SUB: wasm.subcell(), TRI: wasm.tri(), NE: wasm.n_edge_max(), EPS: wasm.ep_per_screen_max(),
    STRIDE: wasm.inst_stride(), MAX_INST: wasm.inst_max(),
  };
  const mem = () => wasm.memory.buffer;
  const view = {
    grid: () => new Uint32Array(mem(), wasm.grid_ptr(), C.NS * C.GH),
    xy:   () => new Int16Array(mem(), wasm.tank_xy_ptr(), C.NT * 2),
    ang:  () => new Uint16Array(mem(), wasm.tank_angle_ptr(), C.NT),
    inp:  () => new Uint8Array(mem(), wasm.tank_input_ptr(), C.NT),
    tstate: () => new Uint8Array(mem(), wasm.tstate_ptr(), C.NT),
    l1:   () => new Uint8Array(mem(), wasm.l1dist_ptr(), C.NS * C.TRI),
    epScreen: () => new Uint8Array(mem(), wasm.ep_screen_ptr(), C.NE),
    epCell:   () => new Uint16Array(mem(), wasm.ep_cell_ptr(), C.NE),
    epCross:  () => new Uint8Array(mem(), wasm.ep_cross_ptr(), C.NE),
    epPart:   () => new Uint8Array(mem(), wasm.ep_partner_ptr(), C.NE),
    dist2:    () => new Uint16Array(mem(), wasm.dist2_ptr(), C.NE * C.NE),
    nexthop:  () => new Uint8Array(mem(), wasm.nexthop_ptr(), C.NE * C.NE),
    pdScreen: () => new Uint8Array(mem(), wasm.pdest_screen_ptr(), C.NT),
    pdCell:   () => new Uint16Array(mem(), wasm.pdest_cell_ptr(), C.NT),
    phas:     () => new Uint8Array(mem(), wasm.phas_ptr(), C.NT),
    pgoal:    () => new Uint8Array(mem(), wasm.pgoal_ptr(), C.NT),
    pstatus:  () => new Uint8Array(mem(), wasm.pstatus_ptr(), C.NT),
    instBytes: (n) => new Uint8Array(mem(), wasm.inst_ptr(), n * C.STRIDE),
  };
  const triIndex = (a, b) => { if (a > b) { const t = a; a = b; b = t; } return a * (2 * C.NC - a + 1) / 2 + (b - a); };
  const l1get = (l1, s, a, b) => l1[s * C.TRI + triIndex(a, b)];
  const DCX = [0, 1, 0, -1], DCY = [-1, 0, 1, 0];
  function l1nextdir(l1, s, src, dst) {
    if (src === dst) return -1; const d0 = l1get(l1, s, src, dst); if (d0 === 0xFF) return -1;
    const scx = src % C.GW, scy = (src / C.GW) | 0;
    for (let d = 0; d < 4; d++) { const nx = scx + DCX[d], ny = scy + DCY[d];
      if (nx < 0 || nx >= C.GW || ny < 0 || ny >= C.GH) continue;
      if (l1get(l1, s, ny * C.GW + nx, dst) === d0 - 1) return d; }
    return -1;
  }

  // --- WebGPU -------------------------------------------------------------
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
    vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: C.STRIDE, stepMode: "instance", attributes: [
      { shaderLocation: 0, offset: 0, format: "sint16x2" }, { shaderLocation: 1, offset: 4, format: "sint16x2" },
      { shaderLocation: 2, offset: 8, format: "sint16x2" }, { shaderLocation: 3, offset: 12, format: "unorm8x4" } ] }] },
    fragment: { module, entryPoint: "fs", targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  const bind = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: viewBuf } }] });

  let scaleX = 1, scaleY = 1;
  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr)), h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const p = Math.min(canvas.width / C.GW, canvas.height / C.GH);
    scaleX = p * 2 / canvas.width; scaleY = -p * 2 / canvas.height;
  }
  new ResizeObserver(resize).observe(canvas); resize();
  // view origin pans by (voxCells, voyCells) for the slide
  function setView(vox, voy) {
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([scaleX, scaleY,
      -scaleX * (C.GW / 2 + vox), -scaleY * (C.GH / 2 + voy)]));
  }

  // --- camera (follow + slide) + picker -----------------------------------
  // wasm owns the camera (cam_sx/cam_sy); JS only animates the slide.
  const CAM = () => ({ sx: wasm.cam_sx(), sy: wasm.cam_sy() });
  const R = { wasm, view, C };
  let slide = null;             // { dx, dy, toSx, toSy, t0 }
  let pickerOpen = false, manualView = false, lastSel = 255;
  function startSlide(toSx, toSy, dx, dy) {
    if (dx === 0 && dy === 0) { wasm.set_camera(toSx, toSy); return; }
    wasm.set_slide(toSx, toSy, dx, dy); slide = { dx, dy, toSx, toSy, t0: performance.now() / 1000 };
  }
  // Opening the picker does NOT deselect — so a selected tank can be sent to a
  // cell in another screen. Picking a screen enters "manual view" (follow paused);
  // follow re-engages when you set a destination or (re)select a tank.
  function setPicker(open) {
    pickerOpen = open;
    document.getElementById("picker").style.display = open ? "grid" : "none";
    for (const a of document.querySelectorAll(".scrollbtn")) a.style.display = open ? "block" : "none";
  }
  function gotoScreen(sx, sy) { const c = CAM(); startSlide(sx, sy, sdir(c.sx, sx, C.SX), sdir(c.sy, sy, C.SY)); manualView = true; setPicker(false); }

  // canvas click: a tank cell -> cycle its state; an empty cell -> set the
  // selected (auto-path) tank's destination there.
  function clickCell(e) {
    const r = canvas.getBoundingClientRect();
    const p = Math.min(r.width / C.GW, r.height / C.GH);
    const lcx = Math.floor((e.clientX - r.left - (r.width - p * C.GW) / 2) / p);
    const lcy = Math.floor((e.clientY - r.top - (r.height - p * C.GH) / 2) / p);
    if (lcx < 0 || lcx >= C.GW || lcy < 0 || lcy >= C.GH) return;
    const cam = CAM(); const wcx = cam.sx * C.GW + lcx, wcy = cam.sy * C.GH + lcy;
    const xy = view.xy();
    for (let t = 0; t < C.NT; t++)
      if ((xy[2 * t] >> 8) === wcx && (xy[2 * t + 1] >> 8) === wcy) { wasm.cycle_tank(t); manualView = false; return; }
    const sel = wasm.selected();
    if (sel !== 255 && view.tstate()[sel] === 1 /*AUTOPATH*/) { wasm.set_dest(sel, wcx, wcy); manualView = false; }
  }
  canvas.addEventListener("click", clickCell);

  // screen-(x,y) button toggles the picker; cells / arrows pick a screen
  document.getElementById("camlabel").addEventListener("click", () => setPicker(!pickerOpen));
  const arrowStep = (dx, dy) => { const c = CAM(); gotoScreen((c.sx + dx + C.SX) % C.SX, (c.sy + dy + C.SY) % C.SY); };
  document.querySelector(".scrollbtn.su").onclick = () => arrowStep(0, -1);
  document.querySelector(".scrollbtn.sd").onclick = () => arrowStep(0, 1);
  document.querySelector(".scrollbtn.sl").onclick = () => arrowStep(-1, 0);
  document.querySelector(".scrollbtn.sr").onclick = () => arrowStep(1, 0);

  buildTouchPad(document.getElementById("map"));
  const dbg = mountWidgets(wasm, view, C, { l1get, l1nextdir });
  // minimaps showing the actual map + tanks + active routes (picker + in-article)
  const pickerMap = makeMiniMap(document.getElementById("picker"), gotoScreen, R);
  const articleMap = makeMiniMap(document.getElementById("w-minimap"), gotoScreen, R);
  if (statusEl) statusEl.style.display = "none";

  // --- frame loop ---------------------------------------------------------
  let paused = false, stepOnce = false, last = performance.now(), fps = 60, acc = 0;
  dbg.onPause = (v) => { paused = v; }; dbg.onStep = () => { stepOnce = true; }; dbg.onReset = () => { wasm.init(); slide = null; setPicker(false); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now; if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;
    const sel = wasm.selected(), ts = view.tstate();
    if (sel !== lastSel) { manualView = false; lastSel = sel; }   // (re)selecting a tank re-engages follow
    const manual = sel !== 255 && ts[sel] === 2;

    // input drives the manual tank only
    let bits = 0; for (const c of held) bits |= KEYMAP[c]; bits |= touchBits;
    if (manual) wasm.set_input(sel, bits);
    document.querySelector(".pad").style.display = manual ? "grid" : "none";

    if (stepOnce) { wasm.tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { wasm.tick(); acc -= TICK_DT; s++; } }
    else acc = 0;

    // follow the selected tank; slide when it changes screen (unless looking around)
    if (!pickerOpen && !manualView && sel !== 255 && !slide) {
      const xy = view.xy(), c = CAM();
      const tsx = (xy[2 * sel] >> 8) / C.GW | 0, tsy = (xy[2 * sel + 1] >> 8) / C.GH | 0;
      if (tsx !== c.sx || tsy !== c.sy) startSlide(tsx, tsy, sdir(c.sx, tsx, C.SX), sdir(c.sy, tsy, C.SY));
    }
    // advance the slide / set the view
    if (slide) {
      let p = (now / 1000 - slide.t0) / SLIDE_DUR;
      if (p >= 1) { wasm.set_camera(slide.toSx, slide.toSy); slide = null; setView(0, 0); }
      else setView(slide.dx * C.GW * p, slide.dy * C.GH * p);
    } else setView(0, 0);

    const n = wasm.inst_count();
    device.queue.writeBuffer(instBuf, 0, view.instBytes(n));
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(),
      clearValue: { r: 0.10, g: 0.11, b: 0.13, a: 1 }, loadOp: "clear", storeOp: "store" }] });
    pass.setPipeline(pipeline); pass.setBindGroup(0, bind); pass.setVertexBuffer(0, instBuf);
    pass.draw(6, n); pass.end(); device.queue.submit([enc.finish()]);

    const cam = CAM(), camIdx = cam.sy * C.SX + cam.sx;
    articleMap.update(camIdx);
    if (pickerOpen) pickerMap.update(camIdx);
    document.getElementById("camlabel").textContent = pickerOpen ? "pick a screen" : `screen ${cam.sx},${cam.sy} ▾`;
    dbg.update({ fps, dt, instCount: n, cam });
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// one D-pad (shown only while a tank is in MANUAL)
function buildTouchPad(map) {
  const layout = [["▲", "u", IN_FWD], ["◄", "l", IN_LEFT], ["►", "r", IN_RIGHT], ["▼", "d", IN_BACK]];
  const pad = el("div", "pad"); pad.style.display = "none";
  for (const [g, pos, bit] of layout) {
    const b = el("button", "padbtn " + pos); b.textContent = g;
    const on = (e) => { touchBits |= bit; e.preventDefault(); e.stopPropagation(); };
    const off = (e) => { touchBits &= ~bit; e.preventDefault(); e.stopPropagation(); };
    b.addEventListener("pointerdown", on); b.addEventListener("pointerup", off);
    b.addEventListener("pointercancel", off); b.addEventListener("pointerleave", off);
    pad.appendChild(b);
  }
  map.appendChild(pad);
}

// a minimap of the 4x4 world: each screen is a tiny canvas showing the walls,
// the tanks, and any active routes; click a screen to view it.
function makeMiniMap(container, onPick, R) {
  const { wasm, view, C } = R;
  const BODY = [[242,158,41],[77,179,230],[120,205,120],[196,140,235]];
  const PATH = [[165,120,45],[55,118,158],[82,140,82],[132,98,165]];   // dim body shade for routes
  const BG = [22,24,30], WALL = [70,75,90];
  const canvases = [], ctxs = [], imgs = [];
  for (let i = 0; i < C.NS; i++) {
    const cv = document.createElement("canvas"); cv.width = C.GW; cv.height = C.GH; cv.className = "mmcell";
    const sx = i % C.SX, sy = (i / C.SX) | 0; cv.onclick = () => onPick(sx, sy);
    container.appendChild(cv); canvases.push(cv); ctxs.push(cv.getContext("2d")); imgs.push(ctxs[i].createImageData(C.GW, C.GH));
  }
  const put = (img, lcx, lcy, c) => { const o = (lcy * C.GW + lcx) * 4, d = img.data; d[o] = c[0]; d[o+1] = c[1]; d[o+2] = c[2]; d[o+3] = 255; };
  const scr = (wcx, wcy) => ((wcy / C.GH) | 0) * C.SX + ((wcx / C.GW) | 0);
  return { update(camIdx) {
    const grid = view.grid(), xy = view.xy(), phas = view.phas(), pst = view.pstatus();
    for (let s = 0; s < C.NS; s++) { const img = imgs[s];
      for (let cy = 0; cy < C.GH; cy++) { const w = grid[s * C.GH + cy];
        for (let cx = 0; cx < C.GW; cx++) put(img, cx, cy, ((w >> cx) & 1) ? WALL : BG); } }
    for (let t = 0; t < C.NT; t++) { if (!phas[t] || pst[t] !== 1) continue;
      const n = wasm.trace_path(t), path = new Uint16Array(wasm.memory.buffer, wasm.path_ptr(), n);
      for (let i = 0; i < n; i++) { const wc = path[i], wcx = wc % C.BW, wcy = (wc / C.BW) | 0; put(imgs[scr(wcx, wcy)], wcx % C.GW, wcy % C.GH, PATH[t]); } }
    for (let t = 0; t < C.NT; t++) { const wcx = xy[2*t] >> 8, wcy = xy[2*t+1] >> 8; put(imgs[scr(wcx, wcy)], wcx % C.GW, wcy % C.GH, BODY[t]); }
    for (let s = 0; s < C.NS; s++) { ctxs[s].putImageData(imgs[s], 0, 0); canvases[s].classList.toggle("cam", s === camIdx); }
  } };
}

function mountWidgets(wasm, view, C, P) {
  const updaters = [], api = { update(info) { for (const u of updaters) u(info); } };
  const focused = () => document.activeElement;
  const at = (id) => document.getElementById(id);

  const cc = at("w-controls");
  if (cc) {
    cc.classList.add("row");
    const bPause = btn("Pause"), bStep = btn("Step"), bReset = btn("Reset"); cc.append(bPause, bStep, bReset);
    bPause.onclick = () => { const v = bPause.textContent === "Pause"; bPause.textContent = v ? "Resume" : "Pause"; api.onPause && api.onPause(v); };
    bStep.onclick = () => api.onStep && api.onStep();
    bReset.onclick = () => api.onReset && api.onReset();
  }

  /* the 4x4 world minimap (#w-minimap) is built by makeMiniMap in main() */

  const sc = at("w-stats");
  if (sc) { sc.classList.add("stats");
    updaters.push(({ fps, dt, instCount, cam }) => { sc.innerHTML =
      `frame <b>${wasm.frame()}</b> · ${(dt * 1000).toFixed(1)} ms · ${fps.toFixed(0)} fps · viewport <b>${cam.sx},${cam.sy}</b><br>` +
      `world <b>${C.BW}×${C.BH}</b> cells · edge points <b>${wasm.ep_count()}</b> · instances <b>${instCount}</b> · mem <b>${(wasm.memory.buffer.byteLength / 1048576).toFixed(1)}</b> MiB<br>` +
      `longest L1 distance <b>${wasm.l1_maxdist()}</b> — fits a byte (ceiling ${wasm.l1_dist_ceil()} &lt; 255)`; });
  }

  const kc = at("w-tanks");
  if (kc) {
    const rows = [];
    for (let t = 0; t < C.NT; t++) {
      const box = el("div", "tankbox t" + t); const title = el("div", "tt"); const live = el("div", "live");
      const b = btn("cycle state"); b.onclick = () => wasm.cycle_tank(t);
      box.append(title, live, b); kc.appendChild(box); rows.push({ title, live });
    }
    updaters.push(() => {
      const xy = view.xy(), ang = view.ang(), tstate = view.tstate(), phas = view.phas(),
        pds = view.pdScreen(), pdc = view.pdCell(), pg = view.pgoal(), pst = view.pstatus(), sel = wasm.selected();
      for (let t = 0; t < C.NT; t++) {
        const r = rows[t], wcx = xy[2 * t] >> 8, wcy = xy[2 * t + 1] >> 8;
        r.title.textContent = `tank ${t} — ${STATE[tstate[t]]}${t === sel ? " ◀" : ""}`;
        let s = `cell (${wcx},${wcy}) screen ${(wcx / C.GW) | 0},${(wcy / C.GH) | 0}  dir ${ang[t] >> 11}/32`;
        if (phas[t]) { const g = pg[t] === 0xFF ? "—" : DIR_GLYPH[pg[t]];
          s += `\ndest screen ${pds[t] % C.SX},${(pds[t] / C.SX) | 0} cell ${pdc[t] % C.GW},${(pdc[t] / C.GW) | 0}  ${STATUS[pst[t]]}  next ${g}`;
        } else s += `\nno destination`;
        r.live.textContent = s;
      }
    });
  }

  const tc = at("w-tunables");
  if (tc) {
    const speed = numField("move_speed (sub/tick)", 0, 256, 1, wasm.move_speed());
    const turn = numField("turn_rate (ang/tick)", 0, 8000, 64, wasm.turn_rate());
    const coll = numField("collide_scale (/256)", 0, 256, 16, wasm.collide_scale());
    speed.input.onchange = () => wasm.set_move_speed(parseInt(speed.input.value) | 0);
    turn.input.onchange = () => wasm.set_turn_rate(parseInt(turn.input.value) | 0);
    coll.input.onchange = () => wasm.set_collide_scale(parseInt(coll.input.value) | 0);
    tc.append(speed.wrap, turn.wrap, coll.wrap);
    updaters.push(() => { if (focused() !== speed.input) speed.input.value = wasm.move_speed();
      if (focused() !== turn.input) turn.input.value = wasm.turn_rate(); if (focused() !== coll.input) coll.input.value = wasm.collide_scale(); });
  }

  const gc = at("w-grid");
  if (gc) {
    gc.classList.add("cellgrid"); gc.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = [];
    for (let i = 0; i < C.NC; i++) { const c = el("div", "cell"); const lcx = i % C.GW, lcy = (i / C.GW) | 0;
      c.onclick = () => { const cam = lastCam; wasm.toggle_wall(cam.sx * C.GW + lcx, cam.sy * C.GH + lcy); lastKey = ""; };
      gc.appendChild(c); cells.push(c); }
    let lastKey = "", lastCam = { sx: 0, sy: 0 };
    updaters.push(({ cam }) => { lastCam = cam; const key = cam.sx + "," + cam.sy + "," + wasm.frame();
      const g = view.grid(), s = cam.sy * C.SX + cam.sx; const k2 = cam.sx + "," + cam.sy;
      if (k2 === lastKey) return; lastKey = k2;
      for (let i = 0; i < C.NC; i++) cells[i].classList.toggle("on", !!((g[s * C.GH + ((i / C.GW) | 0)] >>> (i % C.GW)) & 1)); });
  }

  const fc = at("w-flow");
  if (fc) {
    let fScreen = 0, fTarget = 7 * C.GW + 1, lastKey = "";
    const head = el("div", "row"); head.style.marginBottom = "6px";
    const selScr = el("select");
    for (let s = 0; s < C.NS; s++) { const o = el("option"); o.value = s; o.textContent = `screen ${s % C.SX},${(s / C.SX) | 0}`; selScr.appendChild(o); }
    selScr.onchange = () => { fScreen = parseInt(selScr.value) | 0; lastKey = ""; };
    const hint = el("span"); hint.style.cssText = "color:var(--dim);font-size:13px;align-self:center"; hint.textContent = "click a cell = target";
    head.append(selScr, hint);
    const grid = el("div", "cellgrid"); grid.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = [];
    for (let i = 0; i < C.NC; i++) { const c = el("div", "cell"); c.onclick = () => { fTarget = i; lastKey = ""; }; grid.appendChild(c); cells.push(c); }
    fc.append(head, grid);
    updaters.push(() => { const key = fScreen + "," + fTarget + "," + wasm.frame(); if (key === lastKey) return; lastKey = key;
      const l1 = view.l1();
      for (let i = 0; i < C.NC; i++) { const c = cells[i]; c.className = "cell";
        if (i === fTarget) { c.classList.add("tgt"); c.textContent = "◎"; continue; }
        if (P.l1get(l1, fScreen, i, i) === 0xFF) { c.classList.add("on"); c.textContent = ""; continue; }
        const d = P.l1get(l1, fScreen, i, fTarget);
        if (d === 0xFF) { c.textContent = "·"; continue; }
        const dir = P.l1nextdir(l1, fScreen, i, fTarget); c.classList.add("arr"); c.textContent = dir < 0 ? "·" : DIR_GLYPH[dir]; c.title = "dist " + d; } });
  }

  const ec = at("w-edges");
  if (ec) { ec.classList.add("elist"); let last = -1;
    updaters.push(() => { const n = wasm.ep_count(); if (n === last) return; last = n;
      const es = view.epScreen(), elc = view.epCell(), cr = view.epCross(), pa = view.epPart(); let h = "";
      for (let i = 0; i < n; i++) h += `<div><b>#${i}</b> screen ${es[i] % C.SX},${(es[i] / C.SX) | 0} cell ${elc[i] % C.GW},${(elc[i] / C.GW) | 0} · cross ${DIR_GLYPH[cr[i]]} · partner <b>#${pa[i] === 0xFF ? "—" : pa[i]}</b></div>`;
      ec.innerHTML = h || "<div>no edge points</div>"; });
  }

  const nc = at("w-nexthop");
  if (nc) {
    const head = el("div", "row"); head.style.marginBottom = "6px"; const sel = el("select"); const lab = el("span");
    lab.style.cssText = "color:var(--dim);font-size:13px;align-self:center"; lab.textContent = "next-hop / dist from edge point";
    head.append(lab, sel); const boxd = el("div", "matrix"); nc.append(head, boxd);
    let built = -1, src = 0;
    const fill = () => { const n = wasm.ep_count();
      if (built !== n) { sel.innerHTML = ""; for (let i = 0; i < n; i++) { const o = el("option"); o.value = i; o.textContent = "#" + i; sel.appendChild(o); } built = n; if (src >= n) src = 0; sel.value = src; }
      const d2 = view.dist2(), nh = view.nexthop(); let h = "<table class='mx'><tr><th>to B</th><th>dist2</th><th>next C</th></tr>";
      for (let b = 0; b < n; b++) { const dd = d2[src * C.NE + b], nn = nh[src * C.NE + b];
        h += `<tr><td>#${b}</td><td>${dd === 0xFFFF ? "∞" : dd}</td><td>${nn === 0xFF ? "—" : "#" + nn}</td></tr>`; } boxd.innerHTML = h + "</table>"; };
    sel.onchange = () => { src = parseInt(sel.value) | 0; fill(); };
    let last = -1; updaters.push(() => { const n = wasm.ep_count(); if (n !== last) { last = n; fill(); } });
  }
  return api;
}

function el(tag, cls) { const e = document.createElement(tag); if (cls) e.className = cls; return e; }
function btn(label) { const b = el("button"); b.textContent = label; return b; }
function numField(name, min, max, step, val) {
  const wrap = el("label", "nf"); const span = el("span"); span.textContent = name;
  const input = el("input"); input.type = "number"; input.min = min; input.max = max; input.step = step; input.value = val;
  wrap.append(span, input); return { wrap, input, span };
}
main();
