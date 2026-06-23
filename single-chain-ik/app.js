// single-chain-ik — host: WebGPU renderer + controls + live per-leg readout.
//
// All simulation (gait + the length/direction IK) lives in ik.wasm, in integer
// fixed point. The host ticks the sim, uploads the packed instance buffer the
// module builds, draws it, and reflects the per-leg solve results into the page.
// Instances arrive camera-relative (centred on the walking body), so the view
// transform only scales subcells -> clip and centres; there is no camera math.

const TICK_DT = 1 / 60;

// Instanced quad shader, identical in shape to the other projects here: each
// instance is a centre, a half-extent, a (cos,sin) rotation, and a colour. The
// integer fields are divided by their fixed-point scale here, at the GPU.
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

const el = (tag, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; return e; };

async function main() {
  const VERSION = (typeof window !== "undefined" && window.IK_VERSION) || "dev";
  const vEl = document.getElementById("version"); if (vEl) vEl.textContent = "v" + VERSION;
  const statusEl = document.getElementById("status");
  if (!navigator.gpu) { if (statusEl) statusEl.textContent = "WebGPU not available in this browser."; return; }

  const url = "ik.wasm?v=" + encodeURIComponent(VERSION);
  let instance;
  try { ({ instance } = await WebAssembly.instantiateStreaming(fetch(url), {})); }
  catch { ({ instance } = await WebAssembly.instantiate(await (await fetch(url)).arrayBuffer(), {})); }
  const wasm = instance.exports; wasm.init();

  const C = {
    NL: wasm.n_legs(), NS: wasm.n_seg(), NJ: wasm.n_joint(), SUB: wasm.subcell(),
    ANG: wasm.ang_full(), TRI: wasm.trig_one(),
    VW: wasm.view_w() / wasm.subcell(), VH: wasm.view_h() / wasm.subcell(),  // in cells
    STRIDE: wasm.inst_stride(), MAX_INST: wasm.inst_max(),
  };
  const mem = () => wasm.memory.buffer;
  const view = {
    inst: (n) => new Uint8Array(mem(), wasm.inst_ptr(), n * C.STRIDE),
    curl: () => new Uint16Array(mem(), wasm.leg_curl_ptr(), C.NL),
    reach: () => new Int32Array(mem(), wasm.leg_reach_ptr(), C.NL),
    goal: () => new Int32Array(mem(), wasm.leg_goal_ptr(), C.NL),
    clamp: () => new Uint8Array(mem(), wasm.leg_clamped_ptr(), C.NL),
    dcos: () => new Int16Array(mem(), wasm.leg_dcos_ptr(), C.NL),
    dsin: () => new Int16Array(mem(), wasm.leg_dsin_ptr(), C.NL),
    jx: () => new Int32Array(mem(), wasm.joint_x_ptr(), C.NL * C.NJ),
    jy: () => new Int32Array(mem(), wasm.joint_y_ptr(), C.NL * C.NJ),
    swing: () => new Uint8Array(mem(), wasm.swinging_ptr(), C.NL),
  };

  // --- WebGPU ---
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const canvas = document.getElementById("gpu");
  const ctx = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: "opaque" });
  const instBuf = device.createBuffer({ size: C.MAX_INST * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const viewBuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const module = device.createShaderModule({ code: WGSL });
  // alpha blending so the length-stage overlay can be semi-transparent
  const blend = { color: { srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha" },
                  alpha: { srcFactor: "one", dstFactor: "one-minus-src-alpha" } };
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: C.STRIDE, stepMode: "instance", attributes: [
      { shaderLocation: 0, offset: 0, format: "sint16x2" }, { shaderLocation: 1, offset: 4, format: "sint16x2" },
      { shaderLocation: 2, offset: 8, format: "sint16x2" }, { shaderLocation: 3, offset: 12, format: "unorm8x4" } ] }] },
    fragment: { module, entryPoint: "fs", targets: [{ format, blend }] },
    primitive: { topology: "triangle-list" },
  });
  const bind = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: viewBuf } }] });

  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr)), h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const p = Math.min(canvas.width / C.VW, canvas.height / C.VH);   // px per cell, fit + centre
    device.queue.writeBuffer(viewBuf, 0, new Float32Array([p * 2 / canvas.width, -p * 2 / canvas.height, 0, 0]));
  }
  new ResizeObserver(resize).observe(canvas); resize();

  // tap a leg to focus it (nearest hip, in cell space, camera-relative)
  canvas.addEventListener("click", (e) => {
    const r = canvas.getBoundingClientRect();
    const p = Math.min(r.width / C.VW, r.height / C.VH);
    const cxw = (e.clientX - r.left - r.width / 2) / p;   // cells from centre
    const cyw = (e.clientY - r.top - r.height / 2) / p;
    const jx = view.jx(), jy = view.jy(); const bx = wasm.body_x(), cy = wasm.cam_y();
    let best = 0, bestd = 1e18;
    for (let i = 0; i < C.NL; i++) {
      const hx = (jx[i * C.NJ] - bx) / C.SUB, hy = (jy[i * C.NJ] - cy) / C.SUB;  // camera-relative cells
      const d = (hx - cxw) ** 2 + (hy - cyw) ** 2; if (d < bestd) { bestd = d; best = i; }
    }
    wasm.set_focus(best); if (focusSel) focusSel.value = String(best);
  });

  const dbg = mountWidgets(wasm, view, C);
  if (statusEl) statusEl.style.display = "none";
  const focusSel = document.getElementById("s-focus");

  // --- frame loop ---
  let paused = false, stepOnce = false, last = performance.now(), fps = 60, acc = 0;
  dbg.onPause = (v) => { paused = v; }; dbg.onStep = () => { stepOnce = true; }; dbg.onReset = () => { wasm.init(); };

  function frame(now) {
    let dt = (now - last) / 1000; last = now; if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;

    if (stepOnce) { wasm.tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { wasm.tick(); acc -= TICK_DT; s++; } }
    else acc = 0;

    resize();
    const n = wasm.inst_count();
    device.queue.writeBuffer(instBuf, 0, view.inst(n));
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(),
      clearValue: { r: 0.74, g: 0.73, b: 0.69, a: 1 }, loadOp: "clear", storeOp: "store" }] });
    pass.setPipeline(pipeline); pass.setBindGroup(0, bind); pass.setVertexBuffer(0, instBuf);
    pass.draw(6, n); pass.end(); device.queue.submit([enc.finish()]);

    dbg.update({ fps, dt, instCount: n });
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

function mountWidgets(wasm, view, C) {
  const updaters = [], api = { update(info) { for (const u of updaters) u(info); } };
  const at = (id) => document.getElementById(id);
  const focused = () => document.activeElement;

  // controls
  const bP = at("b-pause"), bS = at("b-step"), bR = at("b-reset");
  if (bP) bP.onclick = () => { const v = bP.textContent === "Pause"; bP.textContent = v ? "Resume" : "Pause"; api.onPause && api.onPause(v); };
  if (bS) bS.onclick = () => api.onStep && api.onStep();
  if (bR) bR.onclick = () => api.onReset && api.onReset();

  const cStages = at("c-stages");
  if (cStages) { cStages.checked = !!wasm.show_stages(); cStages.onchange = () => wasm.set_show_stages(cStages.checked ? 1 : 0); }

  const focusSel = at("s-focus");
  if (focusSel) {
    for (let i = 0; i < C.NL; i++) { const o = el("option"); o.value = i; o.textContent = (i < C.NL / 2 ? "near " : "far ") + (i % (C.NL / 2)); focusSel.appendChild(o); }
    focusSel.value = String(wasm.focus());
    focusSel.onchange = () => wasm.set_focus(parseInt(focusSel.value) | 0);
  }

  // tunables (sliders over the wasm getters/setters, all integer subcells)
  const tc = at("w-tunables");
  if (tc) {
    const mk = (label, get, set, min, max, step, fmt) => {
      const wrap = el("label", "nf"); const span = el("span"); span.textContent = label;
      const input = el("input"); input.type = "range"; input.min = min; input.max = max; input.step = step; input.value = get();
      const val = el("span", "val"); val.textContent = fmt(get());
      input.oninput = () => { set(parseInt(input.value) | 0); val.textContent = fmt(get()); };
      wrap.append(span, input, val); tc.appendChild(wrap);
      updaters.push(() => { if (focused() !== input) { input.value = get(); val.textContent = fmt(get()); } });
    };
    const cell = (v) => (v / C.SUB).toFixed(2) + " c";
    mk("walk speed", () => wasm.walk_speed(), (v) => wasm.set_walk_speed(v), 0, 40, 1, (v) => v + " s/t");
    mk("segment length", () => wasm.seg_len(), (v) => wasm.set_seg_len(v), 70, 360, 5, cell);
    mk("stand height", () => wasm.stand_h(), (v) => wasm.set_stand_h(v), 120, 560, 10, cell);
    mk("stride", () => wasm.step_len(), (v) => wasm.set_step_len(v), 120, 520, 10, cell);
    mk("foot lift", () => wasm.step_lift(), (v) => wasm.set_step_lift(v), 0, 320, 10, cell);
  }

  // live stats
  const sc = at("w-stats");
  if (sc) { sc.classList.add("stats");
    updaters.push(({ fps, dt, instCount }) => { sc.innerHTML =
      `frame <b>${wasm.frame()}</b> · ${(dt * 1000).toFixed(1)} ms · ${fps.toFixed(0)} fps · ` +
      `body x <b>${wasm.body_x()}</b> · quads <b>${instCount}</b> · ` +
      `wasm <b>${(wasm.memory.buffer.byteLength / 1024).toFixed(0)}</b> KB · curl ceiling <b>${(wasm.curl_max() / C.ANG * 360).toFixed(0)}°</b>`; });
  }

  // per-leg solve table
  const lt = at("w-legs");
  if (lt) {
    lt.innerHTML = "<thead><tr><th>leg</th><th>curl</th><th>reach</th><th>goal</th><th>aim</th><th>state</th></tr></thead><tbody></tbody>";
    const body = lt.querySelector("tbody"); const rows = [];
    for (let i = 0; i < C.NL; i++) { const tr = el("tr"); for (let k = 0; k < 6; k++) tr.appendChild(el("td")); body.appendChild(tr); rows.push(tr); }
    const deg = (u) => (u / C.ANG * 360);
    updaters.push(() => {
      const curl = view.curl(), reach = view.reach(), goal = view.goal(), clamp = view.clamp(),
        dcos = view.dcos(), dsin = view.dsin(), sw = view.swing(); const f = wasm.focus();
      for (let i = 0; i < C.NL; i++) {
        const td = rows[i].children;
        const aim = Math.round(Math.atan2(dsin[i], dcos[i]) * 180 / Math.PI);
        td[0].textContent = (i < C.NL / 2 ? "near " : "far ") + (i % (C.NL / 2)); td[0].className = i < C.NL / 2 ? "near" : "far";
        td[1].textContent = deg(curl[i]).toFixed(0) + "°";
        td[2].textContent = reach[i]; td[3].textContent = goal[i];
        td[4].textContent = aim + "°";
        td[5].textContent = clamp[i] ? "CLAMPED" : (sw[i] ? "swing" : "stance");
        td[5].className = clamp[i] ? "clamp" : "";
        rows[i].className = i === f ? "focus" : "";
      }
    });
  }
  return api;
}

main();
