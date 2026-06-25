// tank-combat chapter 4 — host: ISOMETRIC WebGPU renderer + input + swarm debug panel.
//
// The simulation is BYTE-IDENTICAL to chapter 3 (game.wasm), integer fixed point,
// and does not know it is being drawn. Chapter 4 changes only the picture: the host
// reads the sim, uploads the packed 3-D instance buffer, and draws it ISOMETRIC —
// the dimetric projection, the z-buffer, and the flat face-shading all live in the
// shader; the camera is a uniform. render.c emits WORLD PLACEMENTS grouped by kind;
// the host draws one range per kind, binding the placeholder cube or (toggle) a
// baked low-poly mesh — same instances, late-bound art.
//
// The minimap stays TOP-DOWN: the same sim data drawn two ways at once, the
// chapter's lesson made visible. Every debug widget reads the running game's memory
// directly, exactly as before.

const TICK_DT = 1 / 60;

// View uniform: a = (scaleX, scaleY, offsetX, offsetY) maps screen units -> clip;
// b = (isoX, isoY, isoZ, depthSpan) is the dimetric basis (read from wasm, one
// source of truth) + the depth-key range. The vertex shader scales each baked mesh
// vertex by the instance's half-extents, rotates it by the facing about the
// vertical axis, projects it dimetric-2:1, and writes (wx+wy+wz) into the z-buffer;
// the opaque pass flat-shades by the face normal, the translucent pass blends.
const WGSL = `
struct View { a: vec4f, b: vec4f };
@group(0) @binding(0) var<uniform> view: View;
struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f, @location(1) nrm: vec3f };

@vertex fn vs(
    @location(0) mpos: vec4f, @location(1) mnrm: vec4f,        // baked mesh: pos + normal in [-1,1]
    @location(2) wxy: vec2<i32>, @location(3) wzhz: vec2<i32>, // instance: centre xy, centre-z + half-z
    @location(4) hxy: vec2<i32>, @location(5) rot: vec2<i32>,  // half-extent xy, facing (cos,sin) Q14
    @location(6) color: vec4f) -> VOut {
  let half = vec3f(f32(hxy.x), f32(hxy.y), f32(wzhz.y));       // (hx, hy, hz)
  let local = mpos.xyz * half;                                 // unit box -> subcells
  let c = f32(rot.x) * (1.0 / 16384.0); let s = f32(rot.y) * (1.0 / 16384.0);
  let rx = local.x * c - local.y * s; let ry = local.x * s + local.y * c;
  let world = vec3f(f32(wxy.x) + rx, f32(wxy.y) + ry, f32(wzhz.x) + local.z);
  let ix = view.b.x; let iy = view.b.y; let iz = view.b.z;
  let sx = (world.x - world.y) * ix;                           // dimetric 2:1 projection
  let sy = (world.x + world.y) * iy - world.z * iz;
  let depth = clamp((view.b.w - (world.x + world.y + world.z)) / view.b.w, 0.0, 1.0);
  var o: VOut;
  o.pos = vec4f(sx * view.a.x + view.a.z, sy * view.a.y + view.a.w, depth, 1.0);
  let nx = mnrm.x * c - mnrm.y * s; let ny = mnrm.x * s + mnrm.y * c;
  o.nrm = vec3f(nx, ny, mnrm.z); o.col = color;
  return o;
}
const LIGHT = vec3f(0.30, 0.50, 0.80);                         // one directional light, from above
@fragment fn fs_shade(i: VOut) -> @location(0) vec4f {          // opaque: flat per-face shading
  let d = clamp(dot(normalize(i.nrm), normalize(LIGHT)), 0.0, 1.0);
  return vec4f(i.col.rgb * (0.32 + 0.68 * d), i.col.a);         // top brightest, the two sides darker
}
@fragment fn fs_flat(i: VOut) -> @location(0) vec4f { return i.col; }  // translucent FX: full-bright, blended
`;

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
    // chapter-4 render protocol: the iso basis, the kinds, the baked meshes
    IX: wasm.iso_x(), IY: wasm.iso_y(), IZ: wasm.iso_z(), DSPAN: wasm.iso_depth_span(),
    KOC: wasm.k_opaque_count(), MVSTRIDE: wasm.mesh_vstride(), MVTOTAL: wasm.mesh_vert_total(),
    MCOUNT: wasm.mesh_count(), CUBE: wasm.mesh_cube(),
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
  device.addEventListener("uncapturederror", (e) => console.error("WEBGPU_UNCAPTURED:", e.error.message));
  const canvas = document.getElementById("gpu");
  const ctx = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: "opaque" });

  // the baked low-poly meshes, uploaded ONCE (frequency of change: once). The whole
  // packed buffer lives in wasm memory; each kind's draw picks its mesh's vertex
  // range with firstVertex. M_CUBE is the placeholder for every kind.
  const meshBuf = device.createBuffer({ size: C.MVTOTAL * C.MVSTRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(meshBuf, 0, new Uint8Array(mem(), wasm.mesh_data_ptr(), C.MVTOTAL * C.MVSTRIDE));
  const meshTable = []; for (let m = 0; m < C.MCOUNT; m++) meshTable.push({ off: wasm.mesh_voff(m), cnt: wasm.mesh_vcnt(m) });
  const meshForKind = []; for (let k = 0; k < C.KOC; k++) meshForKind.push(wasm.mesh_for_kind(k));

  const instBuf = device.createBuffer({ size: C.MAX_INST * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const viewBuf = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const module = device.createShaderModule({ code: WGSL });

  const bgl = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: "uniform" } }] });
  const layout = device.createPipelineLayout({ bindGroupLayouts: [bgl] });
  const vbuffers = [
    { arrayStride: C.MVSTRIDE, stepMode: "vertex", attributes: [
      { shaderLocation: 0, offset: 0, format: "snorm8x4" }, { shaderLocation: 1, offset: 4, format: "snorm8x4" } ] },
    { arrayStride: C.STRIDE, stepMode: "instance", attributes: [
      { shaderLocation: 2, offset: 0, format: "sint16x2" }, { shaderLocation: 3, offset: 4, format: "sint16x2" },
      { shaderLocation: 4, offset: 8, format: "sint16x2" }, { shaderLocation: 5, offset: 12, format: "sint16x2" },
      { shaderLocation: 6, offset: 16, format: "unorm8x4" } ] },
  ];
  // opaque pass: z-buffered, flat-shaded. translucent FX pass: depth-tested but not
  // written, alpha-blended (render.c already painter-sorted the range).
  const opaquePipe = device.createRenderPipeline({ layout,
    vertex: { module, entryPoint: "vs", buffers: vbuffers },
    fragment: { module, entryPoint: "fs_shade", targets: [{ format }] },
    primitive: { topology: "triangle-list", cullMode: "none" },
    depthStencil: { format: "depth24plus", depthWriteEnabled: true, depthCompare: "less" } });
  const transPipe = device.createRenderPipeline({ layout,
    vertex: { module, entryPoint: "vs", buffers: vbuffers },
    fragment: { module, entryPoint: "fs_flat", targets: [{ format, blend: {
      color: { srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha", operation: "add" },
      alpha: { srcFactor: "one", dstFactor: "one-minus-src-alpha", operation: "add" } } }] },
    primitive: { topology: "triangle-list", cullMode: "none" },
    depthStencil: { format: "depth24plus", depthWriteEnabled: false, depthCompare: "less" } });
  const bind = device.createBindGroup({ layout: bgl, entries: [{ binding: 0, resource: { buffer: viewBuf } }] });

  let depthTex = null;
  function ensureDepth() {
    if (depthTex && depthTex.width === canvas.width && depthTex.height === canvas.height) return;
    if (depthTex) depthTex.destroy();
    depthTex = device.createTexture({ size: [canvas.width, canvas.height], format: "depth24plus", usage: GPUTextureUsage.RENDER_ATTACHMENT });
  }
  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr)), h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  }
  new ResizeObserver(resize).observe(canvas); resize();

  // --- the free isometric camera: drag to pan, pinch / wheel to zoom -------------
  // The projection is a pure function in the shader; the camera is ONLY this uniform.
  // camX,camY is the world point at screen centre (subcells); zoom multiplies the
  // whole-world fit (zoom 1 shows all sixteen screens). viewWrite writes the uniform
  // and remembers the mapping (cam3d) so a pixel can be inverted back to a world cell.
  // Nothing here touches the simulation.
  let useAssets = false;
  let camX = C.BW * C.SUB / 2, camY = C.BH * C.SUB / 2, zoom = 1, follow = false;
  let cam3d = { sx: 1, sy: 1, ox: 0, oy: 0 };
  const ZMIN = 0.85, ZMAX = 20;
  function baseScale() {                              // px per screen-unit with the whole world fit
    const worldW = (C.BW + C.BH) * C.SUB * C.IX, worldH = (C.BW + C.BH) * C.SUB * C.IY + C.SUB * C.IZ;
    return Math.min(canvas.width * 0.96 / worldW, canvas.height * 0.92 / worldH);
  }
  function clampCam() {
    zoom = Math.max(ZMIN, Math.min(ZMAX, zoom));
    camX = Math.max(0, Math.min(C.BW * C.SUB, camX));
    camY = Math.max(0, Math.min(C.BH * C.SUB, camY));
  }
  function viewWrite() {
    const S = baseScale() * zoom;
    const isx = (camX - camY) * C.IX, isy = (camX + camY) * C.IY;       // iso of the camera centre (wz=0)
    const sclX = S * 2 / canvas.width, sclY = -S * 2 / canvas.height;
    cam3d = { sx: sclX, sy: sclY, ox: -isx * sclX, oy: -isy * sclY };
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([sclX, sclY, cam3d.ox, cam3d.oy, C.IX, C.IY, C.IZ, C.DSPAN]));
  }
  // invert screen pixels -> a world point on the ground plane (wz=0), in subcells
  function screenToWorld(clientX, clientY) {
    const r = canvas.getBoundingClientRect();
    const ndcx = (clientX - r.left) / r.width * 2 - 1, ndcy = -((clientY - r.top) / r.height * 2 - 1);
    const sxu = (ndcx - cam3d.ox) / cam3d.sx, syu = (ndcy - cam3d.oy) / cam3d.sy;
    const a = sxu / C.IX, b = syu / C.IY;
    return { x: (a + b) / 2, y: (b - a) / 2 };
  }
  function cellAt(clientX, clientY) {
    const p = screenToWorld(clientX, clientY);
    const wcx = Math.floor(p.x / C.SUB), wcy = Math.floor(p.y / C.SUB);
    if (wcx < 0 || wcx >= C.BW || wcy < 0 || wcy >= C.BH) return null;
    return { wcx, wcy };
  }
  // the world-space box the canvas shows (its four corners inverse-projected) — what
  // the renderer must emit; the cost scales with this, not the whole world.
  function visibleBox() {
    const r = canvas.getBoundingClientRect();
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const [cx, cy] of [[r.left, r.top], [r.right, r.top], [r.left, r.bottom], [r.right, r.bottom]]) {
      const p = screenToWorld(cx, cy);
      x0 = Math.min(x0, p.x); y0 = Math.min(y0, p.y); x1 = Math.max(x1, p.x); y1 = Math.max(y1, p.y);
    }
    return [Math.floor(x0), Math.floor(y0), Math.ceil(x1), Math.ceil(y1)];
  }

  const R = { wasm, view, C };
  let pickerOpen = false, lastMouse = null;
  function setPicker(open) {
    pickerOpen = open;
    document.getElementById("picker").style.display = open ? "grid" : "none";
    for (const a of document.querySelectorAll(".scrollbtn")) a.style.display = open ? "block" : "none";
  }
  function camScreen() { return { sx: Math.floor(camX / (C.GW * C.SUB)) % C.SX, sy: Math.floor(camY / (C.GH * C.SUB)) % C.SY }; }
  function gotoScreen(sx, sy) { camX = sx * C.GW * C.SUB + C.GW * C.SUB / 2; camY = sy * C.GH * C.SUB + C.GH * C.SUB / 2; follow = false; clampCam(); setPicker(false); }

  // pan by a screen-pixel delta (drag): convert to a world delta on the ground plane
  function panBy(dpx, dpy) {
    const r = canvas.getBoundingClientRect();
    const dsx = (dpx / r.width * 2) / cam3d.sx, dsy = (-dpy / r.height * 2) / cam3d.sy;   // px -> screen-units
    const a = dsx / C.IX, b = dsy / C.IY;
    camX -= (a + b) / 2; camY -= (b - a) / 2; clampCam();
  }
  // zoom by `factor`, keeping the world point under (clientX,clientY) fixed
  function zoomBy(factor, clientX, clientY) {
    const r = canvas.getBoundingClientRect();
    const ndcx = (clientX - r.left) / r.width * 2 - 1, ndcy = -((clientY - r.top) / r.height * 2 - 1);
    const P = screenToWorld(clientX, clientY);
    zoom = Math.max(ZMIN, Math.min(ZMAX, zoom * factor));
    const S = baseScale() * zoom, sclX = S * 2 / canvas.width, sclY = -S * 2 / canvas.height;
    const isoPx = (P.x - P.y) * C.IX, isoPy = (P.x + P.y) * C.IY;
    const isoCamX = isoPx - ndcx / sclX, isoCamY = isoPy - ndcy / sclY;
    const a = isoCamX / C.IX, b = isoCamY / C.IY;
    camX = (a + b) / 2; camY = (b - a) / 2; clampCam();
    if (zR) zR.value = zoom.toFixed(2);
  }

  // pointers: one-finger drag = pan, two-finger = pinch zoom, a tap = select / path.
  const pointers = new Map();
  let dragMoved = 0, pinchPrev = 0;
  const pdist = () => { const p = [...pointers.values()]; return Math.hypot(p[0].x - p[1].x, p[0].y - p[1].y); };
  const pmid  = () => { const p = [...pointers.values()]; return [(p[0].x + p[1].x) / 2, (p[0].y + p[1].y) / 2]; };
  canvas.addEventListener("pointerdown", (e) => {
    canvas.setPointerCapture(e.pointerId); pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    dragMoved = 0; if (pointers.size === 2) pinchPrev = pdist();
  });
  canvas.addEventListener("pointermove", (e) => {
    lastMouse = { x: e.clientX, y: e.clientY };
    const prev = pointers.get(e.pointerId); if (!prev) return;            // hovering, no button down here
    const dx = e.clientX - prev.x, dy = e.clientY - prev.y;
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY }); dragMoved += Math.abs(dx) + Math.abs(dy);
    if (pointers.size === 1) { panBy(dx, dy); follow = false; }
    else if (pointers.size >= 2) { const d = pdist(); if (pinchPrev > 0) { const [mx, my] = pmid(); zoomBy(d / pinchPrev, mx, my); } pinchPrev = d; follow = false; }
  });
  function endPointer(e) {
    const was = pointers.size; pointers.delete(e.pointerId);
    if (was === 1 && dragMoved < 8) clickAt(e.clientX, e.clientY);        // a tap, not a drag
    if (pointers.size < 2) pinchPrev = 0;
  }
  canvas.addEventListener("pointerup", endPointer);
  canvas.addEventListener("pointercancel", endPointer);
  canvas.addEventListener("pointerleave", () => { lastMouse = null; });
  canvas.addEventListener("wheel", (e) => { e.preventDefault(); zoomBy(Math.exp(-e.deltaY * 0.0015), e.clientX, e.clientY); follow = false; }, { passive: false });

  // a tap: a tank cell -> cycle its state (and follow it); else send the auto-path tank there
  function clickAt(clientX, clientY) {
    const c = cellAt(clientX, clientY); if (!c) return;
    const xy = view.xy();
    for (let t = 0; t < C.NT; t++)
      if ((xy[2 * t] >> 8) === c.wcx && (xy[2 * t + 1] >> 8) === c.wcy) { wasm.cycle_tank(t); follow = true; return; }
    const sel = wasm.selected();
    if (sel !== 255 && view.tstate()[sel] === 1 /*AUTOPATH*/) wasm.set_dest(sel, c.wcx, c.wcy);
  }

  document.getElementById("camlabel").addEventListener("click", () => setPicker(!pickerOpen));
  const arrowStep = (dx, dy) => { camX += dx * C.GW * C.SUB; camY += dy * C.GH * C.SUB; follow = false; clampCam(); };
  document.querySelector(".scrollbtn.su").onclick = () => arrowStep(0, -1);
  document.querySelector(".scrollbtn.sd").onclick = () => arrowStep(0, 1);
  document.querySelector(".scrollbtn.sl").onclick = () => arrowStep(-1, 0);
  document.querySelector(".scrollbtn.sr").onclick = () => arrowStep(1, 0);

  // render-only controls (presentation): low-poly art toggle + a camera zoom slider
  const aT = document.getElementById("assetsToggle"); if (aT) aT.onchange = () => { useAssets = aT.checked; };
  const zR = document.getElementById("zoom"); if (zR) zR.oninput = () => { zoom = parseFloat(zR.value) || 1; clampCam(); };

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
  dbg.onPause = (v) => { paused = v; }; dbg.onStep = () => { stepOnce = true; };
  dbg.onReset = () => { wasm.init(); camX = C.BW * C.SUB / 2; camY = C.BH * C.SUB / 2; zoom = 1; follow = false; setPicker(false); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now; if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;
    const sel = wasm.selected(), ts = view.tstate();
    const manual = sel !== 255 && ts[sel] === 2;

    let bits = 0; for (const c of held) bits |= KEYMAP[c]; bits |= touchBits;
    if (manual) wasm.set_input(sel, bits);
    document.querySelector(".pad").style.display = manual ? "grid" : "none";

    if (stepOnce) { tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { tick(); acc -= TICK_DT; s++; } }
    else acc = 0;

    // follow the selected tank (until you pan/zoom/pick) — snap on the toroidal wrap
    if (follow && sel !== 255) {
      const xy = view.xy(), tx = xy[2 * sel], ty = xy[2 * sel + 1];
      const dxw = tx - camX, dyw = ty - camY;
      camX = Math.abs(dxw) > C.BW * C.SUB / 2 ? tx : camX + dxw * 0.12;
      camY = Math.abs(dyw) > C.BH * C.SUB / 2 ? ty : camY + dyw * 0.12;
      clampCam();
    }
    viewWrite();
    // tell the wasm what the camera shows + the cursor cell; it builds only that
    const box = visibleBox();
    const hc = lastMouse ? cellAt(lastMouse.x, lastMouse.y) : null;
    wasm.set_view(box[0], box[1], box[2], box[3], hc ? hc.wcx : C.BW, hc ? hc.wcy : C.BH);

    const n = wasm.inst_count();
    device.queue.writeBuffer(instBuf, 0, view.instBytes(n));
    ensureDepth();
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [{ view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.055, g: 0.065, b: 0.085, a: 1 }, loadOp: "clear", storeOp: "store" }],
      depthStencilAttachment: { view: depthTex.createView(), depthClearValue: 1.0, depthLoadOp: "clear", depthStoreOp: "store" } });
    pass.setBindGroup(0, bind); pass.setVertexBuffer(0, meshBuf); pass.setVertexBuffer(1, instBuf);
    // opaque: one draw per kind, binding that kind's mesh (placeholder cube, or a baked mesh)
    pass.setPipeline(opaquePipe);
    let first = 0;
    for (let k = 0; k < C.KOC; k++) {
      const cnt = wasm.draw_opaque(k);
      if (cnt) { const m = meshTable[useAssets ? meshForKind[k] : C.CUBE]; pass.draw(m.cnt, cnt, m.off, first); }
      first += cnt;
    }
    // translucent FX (laser + bursts), drawn after the opaque pass, as sorted cubes
    const tc = wasm.draw_translucent();
    if (tc) { pass.setPipeline(transPipe); const m = meshTable[C.CUBE]; pass.draw(m.cnt, tc, m.off, first); }
    pass.end(); device.queue.submit([enc.finish()]);

    const cam = camScreen(), camIdx = cam.sy * C.SX + cam.sx;
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

// a minimap of the 4x4 world, kept TOP-DOWN while the main view is isometric — the
// same sim data drawn two ways at once. Each screen is a tiny canvas showing the
// walls, the nests, the tanks, and the whole swarm as downsampled dots (hunting =
// red, homing = the nest's colour, wandering = teal); click a screen to view it.
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
      for (let m = 0; m < C.NM; m++) { const cell = rc[m]; if (cell === C.MEMPTY) continue;
        if (rt[m] > best[cell]) best[cell] = rt[m]; }
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
        const rec = rc[m] === C.MEMPTY ? "—" : `(${rc[m] % C.BW},${(rc[m] / C.BW) | 0}) ${now - rt[m]}f`;
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
    const speed = numField("mite speed (sub/tick)", 0, 64, 1, wasm.mite_speed());
    const turn  = numField("mite turn (ang/tick)", 0, 8000, 64, wasm.mite_turn());
    // combat: the page edits human units (shots/sec, seconds); the sim stores ticks
    // (60/sec). fire rate 0 turns the turrets to aim-only (no firing).
    const rateOf = () => { const p = wasm.fire_period(); return p > 0 ? Math.round(600 / p) / 10 : 0; };
    const respOf = () => Math.round(wasm.mite_respawn() / 6) / 10;
    const fire    = numField("fire rate (shots/sec, 0=off)", 0, 20, 0.5, rateOf());
    const respawn = numField("respawn delay (sec)", 0.5, 120, 0.5, respOf());
    const trate   = numField("turret turn (ang/tick)", 64, 8000, 64, wasm.turret_rate());
    const size  = numField("mite radius (subcells)", C.MR, C.MR, 1, C.MR); size.input.disabled = true;
    seed.input.onchange  = () => wasm.set_seed(parseInt(seed.input.value) | 0);
    sense.input.onchange = () => wasm.set_mite_sense(parseInt(sense.input.value) | 0);
    cap.input.onchange   = () => wasm.set_mite_cap(parseInt(cap.input.value) | 0);
    phunt.input.onchange = () => wasm.set_mite_phunt(parseInt(phunt.input.value) | 0);
    speed.input.onchange = () => wasm.set_mite_speed(parseInt(speed.input.value) | 0);
    turn.input.onchange  = () => wasm.set_mite_turn(parseInt(turn.input.value) | 0);
    fire.input.onchange    = () => { const r = parseFloat(fire.input.value) || 0; wasm.set_fire_period(r > 0 ? Math.max(1, Math.round(60 / r)) : 0); };
    respawn.input.onchange = () => { const s = parseFloat(respawn.input.value) || 0; wasm.set_mite_respawn(Math.max(1, Math.round(s * 60))); };
    trate.input.onchange   = () => wasm.set_turret_rate(parseInt(trate.input.value) | 0);
    tc.append(seed.wrap, sense.wrap, cap.wrap, phunt.wrap, speed.wrap, turn.wrap, fire.wrap, respawn.wrap, trate.wrap, size.wrap);

    // nests are structural: one per screen except the tank start (0,0), at each screen
    // centre. That's C.NEST of them — too many to expose as x/y fields, so just note it.
    const nestNote = document.createElement("div");
    nestNote.style.cssText = "grid-column:1/-1;opacity:.6;font-size:12px;margin-top:2px";
    nestNote.textContent = `${C.NEST} nests — one per screen except the tank start (0,0), at each screen centre`;
    tc.append(nestNote);

    const sync = (f, get) => { if (focused() !== f.input) f.input.value = get(); };
    updaters.push(() => { sync(seed, () => wasm.mite_seed()); sync(sense, () => wasm.mite_sense());
      sync(cap, () => wasm.mite_cap()); sync(phunt, () => wasm.mite_phunt());
      sync(speed, () => wasm.mite_speed()); sync(turn, () => wasm.mite_turn());
      sync(fire, rateOf); sync(respawn, respOf); sync(trate, () => wasm.turret_rate()); });
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
