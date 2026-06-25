// tank-combat chapter 3 — host: WebGPU renderer + input + swarm debug panel.
//
// All simulation + transforms live in game.wasm (integer fixed point). The host
// reads input, ticks the sim, uploads the packed instance buffer, draws it, and
// reflects wasm memory into the live panel. The wasm builds the view (one screen,
// or two during a slide) in screen-local subcells; the host FOLLOWS the selected
// tank, SLIDES the new screen in at a border, and a PICKER lets you look around.
//
// New in chapter 3: the renderer draws only the mites the camera shows (cost
// scales with visibility), the minimap shows the whole swarm as dots, and the
// debug widgets read the mite pool, the per-cell occupancy index, the diffusing
// belief field, and the editable seed + tunables.

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

const STATE = ["unselected", "auto-path", "manual"];
const STATUS = ["idle", "routing", "arrived", "no path"];
const MODE = ["wander", "hunt", "home"];
// nest colours, a hue wheel, one per screen but (0,0) (match COL_NEST in render.c)
const NESTC = [[230,146,110],[230,194,110],[218,230,110],[170,230,110],[122,230,110],[110,230,146],[110,230,194],[110,218,230],[110,170,230],[110,122,230],[146,110,230],[194,110,230],[230,110,218],[230,110,170],[230,110,122]];
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
    SUB: wasm.subcell(), STRIDE: wasm.inst_stride(), MAX_INST: wasm.inst_max(),
    NM: wasm.n_mites(), MR: wasm.mite_r(), MCAP: wasm.mite_cap_max(), NWC: wasm.n_world_cells(),
    MEMPTY: wasm.mite_empty(), NEST: wasm.nest_count(), NF: wasm.n_fields(),
    GONE: 0x8000,   // high bit of a record cell flags a "gone" (X is empty) vs a sighting
  };
  const mem = () => wasm.memory.buffer;
  const view = {
    grid: () => new Uint32Array(mem(), wasm.grid_ptr(), C.NS * C.GH),
    xy:   () => new Int16Array(mem(), wasm.tank_xy_ptr(), C.NT * 2),
    ang:  () => new Uint16Array(mem(), wasm.tank_angle_ptr(), C.NT),
    tstate: () => new Uint8Array(mem(), wasm.tstate_ptr(), C.NT),
    pdScreen: () => new Uint8Array(mem(), wasm.pdest_screen_ptr(), C.NT),
    pdCell:   () => new Uint16Array(mem(), wasm.pdest_cell_ptr(), C.NT),
    phas:     () => new Uint8Array(mem(), wasm.phas_ptr(), C.NT),
    pstatus:  () => new Uint8Array(mem(), wasm.pstatus_ptr(), C.NT),
    // the swarm
    mxy:   () => new Int16Array(mem(), wasm.mite_xy_ptr(), C.NM * 2),
    mang:  () => new Uint16Array(mem(), wasm.mite_angle_ptr(), C.NM),
    mmode: () => new Uint8Array(mem(), wasm.mite_mode_ptr(), C.NM),
    mdest: () => new Uint16Array(mem(), wasm.mite_dest_ptr(), C.NM),
    nest:  () => new Uint16Array(mem(), wasm.nest_cell_ptr(), C.NEST),
    mcnt:  () => new Uint8Array(mem(), wasm.mite_cnt_ptr(), C.NWC),
    mrecCell: () => new Uint16Array(mem(), wasm.mite_rec_cell_ptr(), C.NM),  // CURRENT buffer (post-tick)
    mrecTime: () => new Uint32Array(mem(), wasm.mite_rec_time_ptr(), C.NM),
    mresp: () => new Uint16Array(mem(), wasm.mite_resp_ptr(), C.NM),         // 0 = alive; >0 = dead (respawn countdown)
    instBytes: (n) => new Uint8Array(mem(), wasm.inst_ptr(), n * C.STRIDE),
  };

  // --- WebGPU -------------------------------------------------------------
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) throw new Error("no WebGPU adapter (unsupported GPU/driver?)");
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
  function setView(vox, voy) {
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([scaleX, scaleY,
      -scaleX * (C.GW / 2 + vox), -scaleY * (C.GH / 2 + voy)]));
  }

  // --- camera (follow + slide) + picker -----------------------------------
  const CAM = () => ({ sx: wasm.cam_sx(), sy: wasm.cam_sy() });
  const R = { wasm, view, C };
  let slide = null;
  let pickerOpen = false, manualView = false, lastSel = 255;
  function startSlide(toSx, toSy, dx, dy) {
    if (dx === 0 && dy === 0) { wasm.set_camera(toSx, toSy); return; }
    wasm.set_slide(toSx, toSy, dx, dy); slide = { dx, dy, toSx, toSy, t0: performance.now() / 1000 };
  }
  function setPicker(open) {
    pickerOpen = open;
    document.getElementById("picker").style.display = open ? "grid" : "none";
    for (const a of document.querySelectorAll(".scrollbtn")) a.style.display = open ? "block" : "none";
  }
  function gotoScreen(sx, sy) { const c = CAM(); startSlide(sx, sy, sdir(c.sx, sx, C.SX), sdir(c.sy, sy, C.SY)); manualView = true; setPicker(false); }

  // canvas click: a tank cell -> cycle its state; else set the auto-path tank's destination.
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

  document.getElementById("camlabel").addEventListener("click", () => setPicker(!pickerOpen));
  const arrowStep = (dx, dy) => { const c = CAM(); gotoScreen((c.sx + dx + C.SX) % C.SX, (c.sy + dy + C.SY) % C.SY); };
  document.querySelector(".scrollbtn.su").onclick = () => arrowStep(0, -1);
  document.querySelector(".scrollbtn.sd").onclick = () => arrowStep(0, 1);
  document.querySelector(".scrollbtn.sl").onclick = () => arrowStep(-1, 0);
  document.querySelector(".scrollbtn.sr").onclick = () => arrowStep(1, 0);

  buildTouchPad(document.getElementById("map"));
  const dbg = mountWidgets(wasm, view, C);
  const pickerMap = makeMiniMap(document.getElementById("picker"), gotoScreen, R);
  const articleMap = makeMiniMap(document.getElementById("w-minimap"), gotoScreen, R);
  if (statusEl) statusEl.style.display = "none";

  // --- frame loop ---------------------------------------------------------
  let paused = false, stepOnce = false, last = performance.now(), fps = 60, acc = 0, updMs = 0;
  // one timed sim step: the whole per-tick CPU cost (index rebuild, gossip, fields,
  // movement, combat) — EMA-smoothed so the readout is stable across frames.
  const tick = () => { const t0 = performance.now(); wasm.tick(); updMs = updMs * 0.9 + (performance.now() - t0) * 0.1; };
  dbg.onPause = (v) => { paused = v; }; dbg.onStep = () => { stepOnce = true; }; dbg.onReset = () => { wasm.init(); slide = null; setPicker(false); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now; if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;
    const sel = wasm.selected(), ts = view.tstate();
    if (sel !== lastSel) { manualView = false; lastSel = sel; }
    const manual = sel !== 255 && ts[sel] === 2;

    let bits = 0; for (const c of held) bits |= KEYMAP[c]; bits |= touchBits;
    if (manual) wasm.set_input(sel, bits);
    document.querySelector(".pad").style.display = manual ? "grid" : "none";

    if (stepOnce) { tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { tick(); acc -= TICK_DT; s++; } }
    else acc = 0;

    if (!pickerOpen && !manualView && sel !== 255 && !slide) {
      const xy = view.xy(), c = CAM();
      const tsx = (xy[2 * sel] >> 8) / C.GW | 0, tsy = (xy[2 * sel + 1] >> 8) / C.GH | 0;
      if (tsx !== c.sx || tsy !== c.sy) startSlide(tsx, tsy, sdir(c.sx, tsx, C.SX), sdir(c.sy, tsy, C.SY));
    }
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
    dbg.update({ fps, dt, updMs, instCount: n, cam });
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

// a minimap of the 4x4 world: each screen is a tiny canvas showing the walls, the
// nests, the tanks, and the whole swarm as downsampled dots (hunting = red, homing
// = the nest's colour, wandering = teal); click a screen to view it. Shows the
// population at the world scale while the viewport stays on one screen.
function makeMiniMap(container, onPick, R) {
  const { wasm, view, C } = R;
  const BODY = [[242,158,41],[77,179,230],[120,205,120],[196,140,235]];
  const BG = [22,24,30], WALL = [70,75,90], MITE = [96,150,138], MITE_HUNT = [224,110,80], NESTB = [255,245,210];
  const canvases = [], ctxs = [], imgs = [];
  for (let i = 0; i < C.NS; i++) {
    const cv = document.createElement("canvas"); cv.width = C.GW; cv.height = C.GH; cv.className = "mmcell";
    const sx = i % C.SX, sy = (i / C.SX) | 0; cv.onclick = () => onPick(sx, sy);
    container.appendChild(cv); canvases.push(cv); ctxs.push(cv.getContext("2d")); imgs.push(ctxs[i].createImageData(C.GW, C.GH));
  }
  const put = (img, lcx, lcy, c) => { const o = (lcy * C.GW + lcx) * 4, d = img.data; d[o] = c[0]; d[o+1] = c[1]; d[o+2] = c[2]; d[o+3] = 255; };
  const scr = (wcx, wcy) => ((wcy / C.GH) | 0) * C.SX + ((wcx / C.GW) | 0);
  return { update(camIdx) {
    const grid = view.grid(), xy = view.xy(), mxy = view.mxy(), mmode = view.mmode(), resp = view.mresp(), nest = view.nest();
    for (let s = 0; s < C.NS; s++) { const img = imgs[s];
      for (let cy = 0; cy < C.GH; cy++) { const w = grid[s * C.GH + cy];
        for (let cx = 0; cx < C.GW; cx++) put(img, cx, cy, ((w >> cx) & 1) ? WALL : BG); } }
    for (let m = 0; m < C.NM; m++) { if (resp[m]) continue;             // dead mites aren't on the board
      const wcx = mxy[2*m] >> 8, wcy = mxy[2*m+1] >> 8;
      const md = mmode[m], c = md === 1 ? MITE_HUNT : md === 2 ? NESTC[m % C.NEST] : MITE;
      put(imgs[scr(wcx, wcy)], wcx % C.GW, wcy % C.GH, c); }
    for (let n = 0; n < C.NEST; n++) { const wcx = nest[n] % C.BW, wcy = (nest[n] / C.BW) | 0; put(imgs[scr(wcx, wcy)], wcx % C.GW, wcy % C.GH, NESTB); }
    for (let t = 0; t < C.NT; t++) { const wcx = xy[2*t] >> 8, wcy = xy[2*t+1] >> 8; put(imgs[scr(wcx, wcy)], wcx % C.GW, wcy % C.GH, BODY[t]); }
    for (let s = 0; s < C.NS; s++) { ctxs[s].putImageData(imgs[s], 0, 0); canvases[s].classList.toggle("cam", s === camIdx); }
  } };
}

function mountWidgets(wasm, view, C) {
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

  const sc = at("w-stats");
  if (sc) { sc.classList.add("stats");
    updaters.push(({ fps, dt, updMs, instCount, cam }) => {
      const mode = view.mmode(), resp = view.mresp();
      let hunt = 0, home = 0, dead = 0;
      for (let m = 0; m < C.NM; m++) { if (resp[m]) { dead++; continue; } if (mode[m] === 1) hunt++; else if (mode[m] === 2) home++; }
      const alive = C.NM - dead;
      sc.innerHTML =
        `frame <b>${wasm.frame()}</b> · update <b>${updMs.toFixed(2)}</b> ms · ${(dt * 1000).toFixed(1)} ms/frame · ${fps.toFixed(0)} fps · viewport <b>${cam.sx},${cam.sy}</b><br>` +
        `mites <b>${alive}</b> alive · hunting <b>${hunt}</b> · homing <b>${home}</b> · wandering <b>${alive - hunt - home}</b> · dead <b>${dead}</b><br>` +
        `shared routes <b>${wasm.field_active()}</b>/${C.NF} active · peak <b>${wasm.field_peak()}</b> · cap <b>${wasm.mite_cap()}</b>/cell<br>` +
        `drawn instances <b>${instCount}</b> · mem <b>${(wasm.memory.buffer.byteLength / 1048576).toFixed(1)}</b> MiB`;
    });
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
        pds = view.pdScreen(), pdc = view.pdCell(), pst = view.pstatus(), sel = wasm.selected();
      for (let t = 0; t < C.NT; t++) {
        const r = rows[t], wcx = xy[2 * t] >> 8, wcy = xy[2 * t + 1] >> 8;
        r.title.textContent = `tank ${t} — ${STATE[tstate[t]]}${t === sel ? " ◀" : ""}`;
        let s = `cell (${wcx},${wcy}) screen ${(wcx / C.GW) | 0},${(wcy / C.GH) | 0}  dir ${ang[t] >> 11}/32`;
        if (phas[t]) s += `\ndest screen ${pds[t] % C.SX},${(pds[t] / C.SX) | 0} cell ${pdc[t] % C.GW},${(pdc[t] / C.GW) | 0}  ${STATUS[pst[t]]}`;
        else s += `\nno destination`;
        r.live.textContent = s;
      }
    });
  }

  // wall editor (current screen) — toggling rebuilds the path tables (rare)
  const gc = at("w-grid");
  if (gc) {
    gc.classList.add("cellgrid"); gc.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = [];
    for (let i = 0; i < C.NC; i++) { const c = el("div", "cell"); const lcx = i % C.GW, lcy = (i / C.GW) | 0;
      c.onclick = () => { const cam = lastCam; wasm.toggle_wall(cam.sx * C.GW + lcx, cam.sy * C.GH + lcy); lastKey = ""; };
      gc.appendChild(c); cells.push(c); }
    let lastKey = "", lastCam = { sx: 0, sy: 0 };
    updaters.push(({ cam }) => { lastCam = cam; const key = cam.sx + "," + cam.sy;
      if (key === lastKey) return; lastKey = key;   // walls change only on edit (onclick resets lastKey) or screen change
      const g = view.grid(), s = cam.sy * C.SX + cam.sx;
      for (let i = 0; i < C.NC; i++) cells[i].classList.toggle("on", !!((g[s * C.GH + ((i / C.GW) | 0)] >>> (i % C.GW)) & 1)); });
  }

  // per-cell occupancy heatmap (current screen) — rebuilt every tick from the index
  const oc = at("w-occupancy");
  if (oc) {
    oc.classList.add("cellgrid"); oc.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = []; for (let i = 0; i < C.NC; i++) { const c = el("div", "cell"); oc.appendChild(c); cells.push(c); }
    updaters.push(({ cam }) => {
      const cnt = view.mcnt(), xy = view.xy();
      const tankCell = new Set();
      for (let t = 0; t < C.NT; t++) tankCell.add(((xy[2*t+1] >> 8)) * C.BW + (xy[2*t] >> 8));
      for (let i = 0; i < C.NC; i++) {
        const lcx = i % C.GW, lcy = (i / C.GW) | 0, wc = (cam.sy * C.GH + lcy) * C.BW + (cam.sx * C.GW + lcx);
        const n = cnt[wc]; const c = cells[i];
        c.className = "cell" + (n > 0 ? " o" + Math.min(n, 4) : "") + (tankCell.has(wc) ? " tank" : "");
        c.textContent = n > 0 ? n : "";
      }
    });
  }

  // the belief field (current screen): cells where mites believe a tank is, tinted
  // by the freshest sighting's age — watch it diffuse from a sighting and dissolve.
  const bc = at("w-belief");
  if (bc) {
    bc.classList.add("cellgrid"); bc.style.gridTemplateColumns = `repeat(${C.GW}, 1fr)`;
    const cells = []; for (let i = 0; i < C.NC; i++) { const c = el("div", "cell"); bc.appendChild(c); cells.push(c); }
    const best = new Int32Array(C.NWC);            // freshest record time per world cell (-1 = none)
    updaters.push(({ cam }) => {
      const rc = view.mrecCell(), rt = view.mrecTime(), now = wasm.frame();
      best.fill(-1);
      for (let m = 0; m < C.NM; m++) { const raw = rc[m]; if (raw === C.MEMPTY || (raw & C.GONE)) continue;
        if (rt[m] > best[raw]) best[raw] = rt[m]; }   // only sightings light the belief map ("gone" = believed empty)
      const xy = view.xy(); const tankCell = new Set();
      for (let t = 0; t < C.NT; t++) tankCell.add(((xy[2*t+1] >> 8)) * C.BW + (xy[2*t] >> 8));
      for (let i = 0; i < C.NC; i++) {
        const lcx = i % C.GW, lcy = (i / C.GW) | 0, wc = (cam.sy * C.GH + lcy) * C.BW + (cam.sx * C.GW + lcx);
        const c = cells[i]; const t = best[wc];
        let cls = "cell";
        if (t >= 0) { const age = now - t; cls += age < 60 ? " bel-hot" : age < 200 ? " bel-warm" : " bel-cold"; }
        if (tankCell.has(wc)) cls += " tank";
        c.className = cls; c.textContent = t >= 0 ? "◎" : "";
      }
    });
  }

  // the mite pool: live count + a few structure-of-arrays rows
  const mp = at("w-mite-pool");
  if (mp) {
    mp.classList.add("pool");
    const head = el("div", "hd"); const body = el("div"); mp.append(head, body);
    head.textContent = `pool — ${C.NM} mites, fixed SoA (no allocation) · first 8:`;
    let frames = 0;
    updaters.push(() => {
      if (frames++ % 6) return;                    // throttle the text rows
      const mxy = view.mxy(), mang = view.mang(), mmode = view.mmode(), rc = view.mrecCell(), rt = view.mrecTime(), now = wasm.frame();
      let h = `<div class="rowm"> #   cell      dir  mode    record</div>`;
      for (let m = 0; m < 8; m++) {
        const wcx = mxy[2*m] >> 8, wcy = mxy[2*m+1] >> 8, dir = mang[m] >> 11;
        const raw = rc[m], cellof = raw & 0x7FFF;
        const rec = raw === C.MEMPTY ? "—"
          : (raw & C.GONE) ? `gone(${cellof % C.BW},${(cellof / C.BW) | 0}) ${now - rt[m]}f`
          : `(${cellof % C.BW},${(cellof / C.BW) | 0}) ${now - rt[m]}f`;
        h += `<div class="rowm">${String(m).padStart(2)}  (${String(wcx).padStart(2)},${String(wcy).padStart(2)})  ${String(dir).padStart(2)}/32  ${MODE[mmode[m]].padEnd(6)}  ${rec}</div>`;
      }
      body.innerHTML = h;
    });
  }

  // the editable seed + the live tunables
  const tc = at("w-mite-tunables");
  if (tc) {
    const seed  = numField("seed (re-scatters swarm)", 0, 65535, 1, wasm.mite_seed());
    const sense = numField("sensing range (cells)", 0, 6, 1, wasm.mite_sense());
    const cap   = numField(`crowding cap (1..${C.MCAP})`, 1, C.MCAP, 1, wasm.mite_cap());
    const phunt = numField("P(hunt) on adopt (%)", 0, 100, 5, wasm.mite_phunt());
    const wbias = numField("wander outward bias (%)", 0, 100, 5, wasm.wander_bias());
    const speed = numField("mite speed (sub/tick)", 0, 64, 1, wasm.mite_speed());
    const turn  = numField("mite turn (ang/tick)", 0, 8000, 64, wasm.mite_turn());
    // combat: the page edits human units (shots/sec, seconds); the sim stores ticks
    // (60/sec). fire rate 0 turns the turrets to aim-only (no firing).
    const rateOf = () => { const p = wasm.fire_period(); return p > 0 ? Math.round(600 / p) / 10 : 0; };
    const respOf = () => Math.round(wasm.mite_respawn() / 6) / 10;
    const ttlOf  = () => Math.round(wasm.nest_ttl() / 6) / 10;
    const fire    = numField("fire rate (shots/sec, 0=off)", 0, 20, 0.5, rateOf());
    const respawn = numField("respawn delay (sec)", 0.5, 120, 0.5, respOf());
    const nttl    = numField("nest memory (sec, 0=never)", 0, 600, 1, ttlOf());
    const trate   = numField("turret turn (ang/tick)", 64, 8000, 64, wasm.turret_rate());
    const size  = numField("mite radius (subcells)", C.MR, C.MR, 1, C.MR); size.input.disabled = true;
    seed.input.onchange  = () => wasm.set_seed(parseInt(seed.input.value) | 0);
    sense.input.onchange = () => wasm.set_mite_sense(parseInt(sense.input.value) | 0);
    cap.input.onchange   = () => wasm.set_mite_cap(parseInt(cap.input.value) | 0);
    phunt.input.onchange = () => wasm.set_mite_phunt(parseInt(phunt.input.value) | 0);
    wbias.input.onchange = () => wasm.set_wander_bias(parseInt(wbias.input.value) | 0);
    speed.input.onchange = () => wasm.set_mite_speed(parseInt(speed.input.value) | 0);
    turn.input.onchange  = () => wasm.set_mite_turn(parseInt(turn.input.value) | 0);
    fire.input.onchange    = () => { const r = parseFloat(fire.input.value) || 0; wasm.set_fire_period(r > 0 ? Math.max(1, Math.round(60 / r)) : 0); };
    respawn.input.onchange = () => { const s = parseFloat(respawn.input.value) || 0; wasm.set_mite_respawn(Math.max(1, Math.round(s * 60))); };
    nttl.input.onchange    = () => { const s = parseFloat(nttl.input.value) || 0; wasm.set_nest_ttl(Math.max(0, Math.round(s * 60))); };
    trate.input.onchange   = () => wasm.set_turret_rate(parseInt(trate.input.value) | 0);
    tc.append(seed.wrap, sense.wrap, cap.wrap, phunt.wrap, wbias.wrap, speed.wrap, turn.wrap, fire.wrap, respawn.wrap, nttl.wrap, trate.wrap, size.wrap);

    // nests are structural now: one per screen except the tank start (0,0), placed at each
    // screen's centre. That's C.NEST of them — too many to expose as x/y fields, so just note it.
    const nestNote = document.createElement('div');
    nestNote.style.cssText = 'grid-column:1/-1;opacity:.6;font-size:12px;margin-top:2px';
    nestNote.textContent = `${C.NEST} nests — one per screen except the tank start (0,0), at each screen centre`;
    tc.append(nestNote);

    const sync = (f, get) => { if (focused() !== f.input) f.input.value = get(); };
    updaters.push(() => { sync(seed, () => wasm.mite_seed()); sync(sense, () => wasm.mite_sense());
      sync(cap, () => wasm.mite_cap()); sync(phunt, () => wasm.mite_phunt()); sync(wbias, () => wasm.wander_bias());
      sync(speed, () => wasm.mite_speed()); sync(turn, () => wasm.mite_turn());
      sync(fire, rateOf); sync(respawn, respOf); sync(nttl, ttlOf); sync(trate, () => wasm.turret_rate()); });
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

// Never leave the page silently on "loading…": surface any startup failure (the
// transient state needs an explicit exit). main() is async, so a throw anywhere in
// setup rejects here.
main().catch((e) => {
  const s = document.getElementById("status");
  if (s) { s.style.display = ""; s.textContent = "Failed to start: " + ((e && e.message) || e); }
  console.error(e);
});
