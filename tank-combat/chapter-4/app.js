// tank-combat chapter 4 — host: TOP-DOWN PERSPECTIVE WebGPU renderer + input + swarm debug panel.
//
// The simulation is BYTE-IDENTICAL to chapter 3 (game.wasm), integer fixed point,
// and does not know it is being drawn. Chapter 4 changes only the picture: the host
// reads the sim, uploads the packed 3-D instance buffer, and draws it in TOP-DOWN
// PERSPECTIVE — the projection (a model-view-projection matrix), the z-buffer, and the
// flat face-shading all live in the shader; the camera is a uniform. render.c emits
// WORLD PLACEMENTS grouped by kind;
// the host draws one range per kind, binding the placeholder cube or (toggle) a
// baked low-poly mesh — same instances, late-bound art.
//
// The minimap stays TOP-DOWN: the same sim data drawn two ways at once, the
// chapter's lesson made visible. Every debug widget reads the running game's memory
// directly, exactly as before.

const TICK_DT = 1 / 60;

// View uniform: a single 4x4 model-view-projection matrix (mvp), built host-side each
// frame from a top-down perspective camera (lookAt + perspective) and uploaded as the
// one source of truth for the projection. The vertex shader scales each baked mesh
// vertex by the instance's half-extents, rotates it by the facing about the vertical
// axis, then projects it with the mvp — clip.z/w is the real depth written to the
// z-buffer; the opaque pass flat-shades by the face normal, the translucent pass blends.
const WGSL = `
struct View { mvp: mat4x4f };                                  // the model-view-projection (subcells -> clip)
@group(0) @binding(0) var<uniform> view: View;
struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f, @location(1) nrm: vec3f, @location(2) world: vec3f };
struct GOut { @location(0) color: vec4f, @location(1) normal: vec4f, @location(2) position: vec4f };   // G-buffer: albedo+mask, normal, world position

@vertex fn vs(
    @location(0) mpos: vec4f, @location(1) mnrm: vec4f,        // baked mesh: pos + normal in [-1,1]
    @location(7) mcol: vec4f,                                  // baked mesh: per-vertex colour (white = tintable)
    @location(2) wxy: vec2<i32>, @location(3) wzhz: vec2<i32>, // instance: centre xy, centre-z + half-z
    @location(4) hxy: vec2<i32>, @location(5) rot: vec2<i32>,  // half-extent xy, facing (cos,sin) Q14
    @location(6) color: vec4f) -> VOut {
  let half = vec3f(f32(hxy.x), f32(hxy.y), f32(wzhz.y));       // (hx, hy, hz)
  let local = mpos.xyz * half;                                 // unit box -> subcells
  let c = f32(rot.x) * (1.0 / 16384.0); let s = f32(rot.y) * (1.0 / 16384.0);
  let rx = local.x * c - local.y * s; let ry = local.x * s + local.y * c;
  let world = vec3f(f32(wxy.x) + rx, f32(wxy.y) + ry, f32(wzhz.x) + local.z);
  var o: VOut;
  o.pos = view.mvp * vec4f(world, 1.0);                        // top-down perspective; depth is clip.z/w
  let nx = mnrm.x * c - mnrm.y * s; let ny = mnrm.x * s + mnrm.y * c;
  o.nrm = vec3f(nx, ny, mnrm.z); o.col = mcol * color;         // world-space face normal; mesh colour x tint
  o.world = world;
  return o;
}
// SHADOW BAKE: the same instance transform as vs, but projected by the SUN's orthographic light
// matrix (bound to the view uniform) and emitting depth ONLY. Run once over the static town to
// bake a sun depth map — the town is frozen, so its sun shadow is too (re-baked only on a map
// edit, exactly like the static instance buffer). No fragment stage: the rasteriser writes depth.
@vertex fn vs_shadow(
    @location(0) mpos: vec4f,
    @location(2) wxy: vec2<i32>, @location(3) wzhz: vec2<i32>,
    @location(4) hxy: vec2<i32>, @location(5) rot: vec2<i32>) -> @builtin(position) vec4f {
  let half = vec3f(f32(hxy.x), f32(hxy.y), f32(wzhz.y));
  let local = mpos.xyz * half;
  let c = f32(rot.x) * (1.0 / 16384.0); let s = f32(rot.y) * (1.0 / 16384.0);
  let rx = local.x * c - local.y * s; let ry = local.x * s + local.y * c;
  let world = vec3f(f32(wxy.x) + rx, f32(wxy.y) + ry, f32(wzhz.x) + local.z);
  return view.mvp * vec4f(world, 1.0);                         // sun light-space clip; depth is z/w
}
// DEFERRED geometry: write the SURFACE into the G-buffer (albedo + world normal) instead of
// shading it here. gColor.a = 1 marks a surface; the cleared background stays 0 (sky). The
// lighting is a later SCREEN pass, so one light or a thousand cost the same fill here — that
// is the whole reason for deferring: lighting scales with lit pixels, not objects x lights.
@fragment fn fs_gbuffer(i: VOut) -> GOut {
  var o: GOut;
  o.color    = vec4f(i.col.rgb, 1.0);
  o.normal   = vec4f(normalize(i.nrm) * 0.5 + 0.5, 1.0);
  o.position = vec4f(i.world, 1.0);                            // world position (subcells), for the point lights
  return o;
}
@fragment fn fs_flat(i: VOut) -> @location(0) vec4f { return i.col; }  // forward FX, blended into the lit HDR

// LOD1 IMPOSTER: the cheap cage, but each face samples town_atlas.png — the LOD0 art projected
// (offline) from that face's direction. Same instance transform + same G-buffer write as the full
// art, so the deferred lighting/shadows treat it identically; only the albedo comes from a texture.
struct VOutI { @builtin(position) pos: vec4f, @location(0) mcol: vec4f, @location(1) nrm: vec3f,
               @location(2) world: vec3f, @location(3) uv: vec2f, @location(4) tint: vec4f };
@group(1) @binding(0) var atlasTex: texture_2d<f32>;
@group(1) @binding(1) var atlasSamp: sampler;
@vertex fn vs_imp(
    @location(0) mpos: vec4f, @location(1) mnrm: vec4f, @location(7) mcol: vec4f,
    @location(8) muv: vec2<i32>,                                 // imposter UV (i16; <0 = no texture, use mcol)
    @location(2) wxy: vec2<i32>, @location(3) wzhz: vec2<i32>,
    @location(4) hxy: vec2<i32>, @location(5) rot: vec2<i32>, @location(6) color: vec4f) -> VOutI {
  let half = vec3f(f32(hxy.x), f32(hxy.y), f32(wzhz.y));
  let local = mpos.xyz * half;
  let c = f32(rot.x) * (1.0 / 16384.0); let s = f32(rot.y) * (1.0 / 16384.0);
  let rx = local.x * c - local.y * s; let ry = local.x * s + local.y * c;
  let world = vec3f(f32(wxy.x) + rx, f32(wxy.y) + ry, f32(wzhz.x) + local.z);
  var o: VOutI;
  o.pos = view.mvp * vec4f(world, 1.0);
  let nx = mnrm.x * c - mnrm.y * s; let ny = mnrm.x * s + mnrm.y * c;
  o.nrm = vec3f(nx, ny, mnrm.z); o.world = world;
  o.mcol = mcol; o.tint = color; o.uv = vec2f(f32(muv.x), f32(muv.y));   // raw i16 UV; /32767 -> [0,1]
  return o;
}
@fragment fn fs_imp(i: VOutI) -> GOut {
  let t = textureSampleLevel(atlasTex, atlasSamp, i.uv / 32767.0, 0.0);   // sampled unconditionally (no derivatives)
  var base = i.mcol.rgb;                                         // fallback: the massing colour (cage gaps / flat tiles)
  if (i.uv.x >= 0.0 && t.a > 0.5) { base = t.rgb; }              // textured where the projection covered this face
  var o: GOut;
  o.color    = vec4f(base * i.tint.rgb, 1.0);                    // baked albedo x instance tint (nest hue / white)
  o.normal   = vec4f(normalize(i.nrm) * 0.5 + 0.5, 1.0);
  o.position = vec4f(i.world, 1.0);
  return o;
}

// ---- DEFERRED point lights ----------------------------------------------------------
// One per visible mite / FX (build_lights). Each is drawn as a small cube VOLUME (front
// faces only, no depth test); for every pixel the volume covers it reads the G-buffer
// surface, falls off with distance, and ADDS its colour into the HDR. So a light pays only
// for the pixels it actually covers — a thousand of them stay cheap.
@group(0) @binding(1) var gColorTex: texture_2d<f32>;
@group(0) @binding(2) var gNormalTex: texture_2d<f32>;
@group(0) @binding(3) var gPosTex: texture_2d<f32>;
struct LVOut {
  @builtin(position) pos: vec4f,
  @location(0) @interpolate(flat) center: vec3f,    // light centre (world)
  @location(1) @interpolate(flat) lcol: vec4f,      // rgb colour, a = radius
  @location(2) @interpolate(flat) inten: f32,       // intensity (instance alpha)
};
@vertex fn vs_light(
    @location(0) mpos: vec4f,
    @location(2) wxy: vec2<i32>, @location(3) wzhz: vec2<i32>,
    @location(4) hxy: vec2<i32>, @location(6) color: vec4f) -> LVOut {
  let r = f32(hxy.x);                                          // radius == cube half-extent (hx=hy=hz)
  let local = mpos.xyz * vec3f(r, r, f32(wzhz.y));
  let world = vec3f(f32(wxy.x) + local.x, f32(wxy.y) + local.y, f32(wzhz.x) + local.z);
  var o: LVOut;
  o.pos = view.mvp * vec4f(world, 1.0);
  o.center = vec3f(f32(wxy.x), f32(wxy.y), f32(wzhz.x));
  o.lcol = vec4f(color.rgb, r);
  o.inten = color.a;
  return o;
}
@fragment fn fs_lightvol(i: LVOut) -> @location(0) vec4f {
  let p = vec2<i32>(i32(i.pos.x), i32(i.pos.y));
  let alb = textureLoad(gColorTex, p, 0);
  if (alb.a < 0.5) { discard; }                               // sky: nothing to light
  let surf = textureLoad(gPosTex, p, 0).xyz;
  let n = normalize(textureLoad(gNormalTex, p, 0).xyz * 2.0 - 1.0);
  let toL = i.center - surf;
  let dist = length(toL);
  let r = i.lcol.a;
  if (dist >= r) { discard; }                                 // outside the light's reach
  let f = 1.0 - dist / r;
  let ndl = clamp(dot(n, toL / max(dist, 1.0)), 0.0, 1.0);
  return vec4f(alb.rgb * i.lcol.rgb * (f * f * ndl * i.inten * 8.0), 1.0);   // additive (one,one); a scales 0..8x in HDR
}
`;

// The SCREEN passes live in their own module: their bind group 0 (the light + the G-buffer)
// is a different layout from the geometry view uniform, so it can't share one module binding.
// fs_light reads the G-buffer and writes environmental lighting into an HDR target; fs_tonemap
// rolls that HDR down to the display. textureLoad needs no sampler (per-pixel, no filtering).
const WGSL_SCREEN = `
struct Env {
  sun: vec4f, sunCol: vec4f, sky: vec4f, ground: vec4f,   // sun.xyz dir + .w intensity; sky/ground = hemisphere ambient
  shp: vec4f,                                             // screen-space shadow: maxDist, steps, thickness, bias (subcells)
  eye: vec4f,                                             // camera world position (subcells); .w = shadow-map normal-offset bias (subcells)
  mvp: mat4x4f,                                           // world (subcells) -> clip, to project the shadow ray to screen
  lightMvp: mat4x4f,                                      // world (subcells) -> SUN light clip, to sample the baked shadow map
};
@group(0) @binding(0) var<uniform> env: Env;
@group(0) @binding(1) var gColorTex: texture_2d<f32>;
@group(0) @binding(2) var gNormalTex: texture_2d<f32>;
@group(0) @binding(3) var gPosTex: texture_2d<f32>;
@group(0) @binding(4) var hdrTex: texture_2d<f32>;
@group(0) @binding(5) var shadowMap: texture_depth_2d;        // the baked sun depth map (static town)
@group(0) @binding(6) var shadowSamp: sampler_comparison;     // hardware PCF compare
@vertex fn vs_full(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));   // one screen-covering triangle
  return vec4f(p[vi], 0.0, 1.0);
}
// SCREEN-SPACE SUN SHADOW (Bend Studio's method): march from the surface toward the sun in
// screen space, reading the G-buffer the geometry already wrote. Each step is a world point on
// the ray, projected to its pixel; if a STORED surface there sits in front of the ray within a
// thickness window, the sun is blocked. No shadow map — the on-screen depth IS the occluder.
// (Bend's contribution is the wavefront SCHEDULING that shares depth reads; the test is this.)
// Medium-range + many small steps gives a clean HARD grounding shadow; no dither (an undenoised
// jitter just reads as grain, and a long march only smears a tall occluder's truncated silhouette).
fn sun_shadow(pos: vec3f, n: vec3f) -> f32 {
  let steps = i32(env.shp.y);
  let stepv = normalize(env.sun.xyz) * (env.shp.x / env.shp.y);          // one ray step (subcells)
  let res = vec2f(textureDimensions(gColorTex));
  var rp = pos + n * env.shp.w;                                          // bias off the surface, then march
  for (var s = 0; s < steps; s = s + 1) {
    rp = rp + stepv;
    let clip = env.mvp * vec4f(rp, 1.0);
    if (clip.w <= 0.0) { break; }
    let uv = (clip.xy / clip.w) * vec2f(0.5, -0.5) + vec2f(0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { break; }   // marched off-screen
    let px = vec2<i32>(i32(uv.x * res.x), i32(uv.y * res.y));
    if (textureLoad(gColorTex, px, 0).a < 0.5) { continue; }               // sky there — no occluder
    let occ = textureLoad(gPosTex, px, 0).xyz;
    let delta = length(rp - env.eye.xyz) - length(occ - env.eye.xyz);     // >0: the stored surface is in front of the ray
    if (delta > env.shp.w && delta < env.shp.z) { return 0.0; }            // blocked within the thickness window
  }
  return 1.0;
}
// BAKED SUN SHADOW MAP: the static town's depth from the sun, rendered once. Project this
// receiver into the sun's light clip, then PCF-compare its depth against the stored nearest-to-
// sun depth — if something static is closer to the sun, this pixel is in shadow. Handles the LONG
// building shadows a single-layer screen-space march can't (it can't see an occluder's far side);
// the SSS still adds contact + the DYNAMIC actors, which aren't in this static bake. A normal-
// offset bias (push the receiver toward the sun along its normal) kills the self-shadow acne.
fn shadow_map(pos: vec3f, n: vec3f) -> f32 {
  let clip = env.lightMvp * vec4f(pos + n * env.eye.w, 1.0);
  let ndc = clip.xyz / clip.w;
  let uv = ndc.xy * vec2f(0.5, -0.5) + vec2f(0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) { return 1.0; }
  return textureSampleCompareLevel(shadowMap, shadowSamp, uv, ndc.z - 0.0008);   // lit where receiver is nearer than the stored occluder
}
// ENVIRONMENTAL lighting: a hemisphere ambient (sky overhead, a dim ground bounce below,
// lerped by how "up" the face points — +z is up) plus one directional sun, the sun SHADOWED in
// screen space. The background (mask 0) is the sky itself.
@fragment fn fs_light(@builtin(position) fc: vec4f) -> @location(0) vec4f {
  let p = vec2<i32>(i32(fc.x), i32(fc.y));
  let alb = textureLoad(gColorTex, p, 0);
  if (alb.a < 0.5) { return vec4f(env.sky.rgb, 1.0); }
  let pos = textureLoad(gPosTex, p, 0).xyz;
  let n = normalize(textureLoad(gNormalTex, p, 0).xyz * 2.0 - 1.0);
  let up = clamp(n.z * 0.5 + 0.5, 0.0, 1.0);
  let ambient = mix(env.ground.rgb, env.sky.rgb, up);
  var sun = clamp(dot(n, normalize(env.sun.xyz)), 0.0, 1.0) * env.sun.w;
  if (sun > 0.0) { sun = sun * shadow_map(pos, n) * sun_shadow(pos, n); }  // baked town shadow + screen-space (dynamic + contact); ambient still fills
  return vec4f(alb.rgb * (ambient + env.sunCol.rgb * sun), 1.0);
}
// TONEMAP: exposure roll-off (1 - e^-cx) — near-linear in the mid-tones, never clips, so the
// additive point lights stay graceful when they push pixels past 1.
@fragment fn fs_tonemap(@builtin(position) fc: vec4f) -> @location(0) vec4f {
  let c = textureLoad(hdrTex, vec2<i32>(i32(fc.x), i32(fc.y)), 0).rgb;
  return vec4f(vec3f(1.0) - exp(-c * 1.25), 1.0);
}
`;

// --- tiny column-major 4x4 matrix helpers (WGSL mat4x4f is column-major) ---------
const v3sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const v3cross = (a, b) => [a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]];
const v3dot = (a, b) => a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
const v3norm = (a) => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0]/l, a[1]/l, a[2]/l]; };
function m4mul(a, b) {                                    // a*b, both column-major[16]
  const o = new Array(16);
  for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    let s = 0; for (let k = 0; k < 4; k++) s += a[k*4+r] * b[c*4+k];
    o[c*4+r] = s;
  }
  return o;
}
function m4vec(m, v) {                                    // m * v (v=[x,y,z,w])
  return [ m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12]*v[3], m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13]*v[3],
           m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]*v[3], m[3]*v[0]+m[7]*v[1]+m[11]*v[2]+m[15]*v[3] ];
}
function m4scale(s) { return [s,0,0,0, 0,s,0,0, 0,0,s,0, 0,0,0,1]; }
function m4perspective(fovy, aspect, near, far) {        // WebGPU clip: z in [0,1]
  const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
  return [ f/aspect,0,0,0,  0,f,0,0,  0,0,far*nf,-1,  0,0,near*far*nf,0 ];
}
function m4lookAt(eye, center, up) {                     // right-handed view matrix
  const z = v3norm(v3sub(eye, center)), x = v3norm(v3cross(up, z)), y = v3cross(z, x);
  return [ x[0],y[0],z[0],0,  x[1],y[1],z[1],0,  x[2],y[2],z[2],0,
           -v3dot(x,eye),-v3dot(y,eye),-v3dot(z,eye),1 ];
}
function m4ortho(l, r, b, t, near, far) {                // WebGPU clip: z in [0,1] (parallel projection)
  return [ 2/(r-l),0,0,0,  0,2/(t-b),0,0,  0,0,1/(near-far),0,
           -(r+l)/(r-l), -(t+b)/(t-b), near/(near-far), 1 ];
}
function m4invert(m) {                                    // general 4x4 inverse
  const a00=m[0],a01=m[1],a02=m[2],a03=m[3], a10=m[4],a11=m[5],a12=m[6],a13=m[7],
        a20=m[8],a21=m[9],a22=m[10],a23=m[11], a30=m[12],a31=m[13],a32=m[14],a33=m[15];
  const b00=a00*a11-a01*a10, b01=a00*a12-a02*a10, b02=a00*a13-a03*a10, b03=a01*a12-a02*a11,
        b04=a01*a13-a03*a11, b05=a02*a13-a03*a12, b06=a20*a31-a21*a30, b07=a20*a32-a22*a30,
        b08=a20*a33-a23*a30, b09=a21*a32-a22*a31, b10=a21*a33-a23*a31, b11=a22*a33-a23*a32;
  let det = b00*b11-b01*b10+b02*b09+b03*b08-b04*b07+b05*b06;
  if (!det) return null; det = 1/det;
  return [
    (a11*b11-a12*b10+a13*b09)*det, (a02*b10-a01*b11-a03*b09)*det, (a31*b05-a32*b04+a33*b03)*det, (a22*b04-a21*b05-a23*b03)*det,
    (a12*b08-a10*b11-a13*b07)*det, (a00*b11-a02*b08+a03*b07)*det, (a32*b02-a30*b05-a33*b01)*det, (a20*b05-a22*b02+a23*b01)*det,
    (a10*b10-a11*b08+a13*b06)*det, (a01*b08-a00*b10-a03*b06)*det, (a30*b04-a31*b02+a33*b00)*det, (a21*b02-a20*b04-a23*b00)*det,
    (a11*b07-a10*b09-a12*b06)*det, (a00*b09-a01*b07+a02*b06)*det, (a31*b01-a30*b03-a32*b00)*det, (a20*b03-a21*b01+a22*b00)*det ];
}

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
// one distinct tint per nest (NEST_COUNT on the 8x8 map); a full hue wheel, matches COL_NEST in render.c
const NESTC = [[230,146,110],[230,157,110],[230,169,110],[230,180,110],[230,192,110],[230,203,110],[230,215,110],[230,226,110],[223,230,110],[211,230,110],[200,230,110],[188,230,110],[177,230,110],[165,230,110],[154,230,110],[143,230,110],[131,230,110],[120,230,110],[110,230,112],[110,230,123],[110,230,135],[110,230,146],[110,230,157],[110,230,169],[110,230,180],[110,230,192],[110,230,203],[110,230,215],[110,230,226],[110,223,230],[110,211,230],[110,200,230],[110,188,230],[110,177,230],[110,165,230],[110,154,230],[110,143,230],[110,131,230],[110,120,230],[112,110,230],[123,110,230],[135,110,230],[146,110,230],[157,110,230],[169,110,230],[180,110,230],[192,110,230],[203,110,230],[215,110,230],[226,110,230],[230,110,223],[230,110,211],[230,110,200],[230,110,188],[230,110,177],[230,110,165],[230,110,154],[230,110,143],[230,110,131],[230,110,120],[230,112,110],[230,123,110],[230,135,110]];

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
    // chapter-4 render protocol: the kinds + the baked meshes (the projection is a host MVP)
    KOC: wasm.k_opaque_count(), MVSTRIDE: wasm.mesh_vstride(), MVTOTAL: wasm.mesh_vert_total(),
    MCOUNT: wasm.mesh_count(), CUBE: wasm.mesh_cube(),
    LOD2OFF: wasm.map_lod2_offset(),     // LOD2 (flat massing) mesh id = LOD0 id + LOD2OFF
    MPROC: wasm.mesh_proc_count(), IMPCOUNT: wasm.map_imposter_count(),   // LOD1 imposters: separate table, base = id - MPROC
    ATLASW: wasm.map_atlas_w(), ATLASH: wasm.map_atlas_h(),
    // the STATIC TOWN: a run-table stride (the instance stride == inst_stride == STRIDE)
    SRSTRIDE: wasm.static_run_stride(),
    // wall-shake: a struck building's static instance jolts (the sim computes the buzz)
    SHAKEMAX: wasm.wall_shake_max(), SHAKEDUR: wasm.wall_shake_dur(), SHAKEAMP: wasm.wall_shake_amp(),
  };
  const mem = () => wasm.memory.buffer;
  const view = {
    grid: () => new Uint32Array(mem(), wasm.grid_ptr(), C.NS * C.GH),
    xy:   () => new Uint16Array(mem(), wasm.tank_xy_ptr(), C.NT * 2),  // positions are unsigned (8x8 arena > int16)
    ang:  () => new Uint16Array(mem(), wasm.tank_angle_ptr(), C.NT),
    tstate: () => new Uint8Array(mem(), wasm.tstate_ptr(), C.NT),
    pdScreen: () => new Uint8Array(mem(), wasm.pdest_screen_ptr(), C.NT),
    pdCell:   () => new Uint16Array(mem(), wasm.pdest_cell_ptr(), C.NT),
    phas:     () => new Uint8Array(mem(), wasm.phas_ptr(), C.NT),
    pstatus:  () => new Uint8Array(mem(), wasm.pstatus_ptr(), C.NT),
    // the swarm
    mxy:   () => new Uint16Array(mem(), wasm.mite_xy_ptr(), C.NM * 2),  // positions are unsigned (8x8 arena > int16)
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
  // timestamp-query gives real per-pass GPU time; not every platform exposes it (notably iOS
  // Safari today), so it is optional — the profiler falls back to CPU timing + the layer toggles.
  const canTimestamp = adapter.features.has("timestamp-query");
  const device = await adapter.requestDevice({ requiredFeatures: canTimestamp ? ["timestamp-query"] : [] });
  device.addEventListener("uncapturederror", (e) => console.error("WEBGPU_UNCAPTURED:", e.error.message));
  const canvas = document.getElementById("gpu");
  const ctx = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: "opaque" });

  // the baked low-poly meshes, uploaded ONCE (frequency of change: once). Two wasm
  // buffers — the procedural meshes then the Kenney map meshes — go back to back into
  // one GPU buffer; each kind's draw picks its mesh's vertex range with firstVertex
  // (offsets from mesh_voff() are already into this combined buffer).
  const meshBuf = device.createBuffer({ size: C.MVTOTAL * C.MVSTRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const mProc = wasm.mesh_proc_total();
  device.queue.writeBuffer(meshBuf, 0, new Uint8Array(mem(), wasm.mesh_data_ptr(), mProc * C.MVSTRIDE));
  device.queue.writeBuffer(meshBuf, mProc * C.MVSTRIDE, new Uint8Array(mem(), wasm.map_mesh_data_ptr(), (C.MVTOTAL - mProc) * C.MVSTRIDE));
  const meshTable = []; for (let m = 0; m < C.MCOUNT; m++) meshTable.push({ off: wasm.mesh_voff(m), cnt: wasm.mesh_vcnt(m) });
  const meshForKind = []; for (let k = 0; k < C.KOC; k++) meshForKind.push(wasm.mesh_for_kind(k));
  // the LOD1 IMPOSTERS: their own 16-byte vertex buffer (they carry UVs) + a table indexed by base m.
  const IMP_VSTRIDE = 16, impTotal = wasm.map_imposter_vert_total();
  const imposterBuf = device.createBuffer({ size: Math.max(IMP_VSTRIDE, impTotal * IMP_VSTRIDE), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(imposterBuf, 0, new Uint8Array(mem(), wasm.map_imposter_data_ptr(), impTotal * IMP_VSTRIDE));
  const imposterTable = []; for (let m = 0; m < C.IMPCOUNT; m++) imposterTable.push({ off: wasm.map_imposter_voff(m), cnt: wasm.map_imposter_vcnt(m) });
  // town_atlas.png — the LOD0 art projected from top + 4 sides, sampled by the imposter. Loaded
  // async: until it arrives the imposter samples empty (alpha 0) and falls back to the massing colour.
  const atlasTex = device.createTexture({ size: [C.ATLASW, C.ATLASH], format: "rgba8unorm",
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
  const atlasSampler = device.createSampler({ magFilter: "linear", minFilter: "linear" });
  fetch("town_atlas.png?v=" + VERSION).then((r) => r.blob()).then(createImageBitmap).then((bmp) => {
    device.queue.copyExternalImageToTexture({ source: bmp }, { texture: atlasTex }, [C.ATLASW, C.ATLASH]);
  }).catch((e) => console.warn("town_atlas.png load failed:", e));

  const instBuf = device.createBuffer({ size: C.MAX_INST * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const viewBuf = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });

  // the STATIC TOWN instance buffer: the bake (one Inst per cell, build_static_map),
  // uploaded ONCE and re-uploaded only when the map re-bakes. Same 24-byte Inst layout as
  // the dynamic buffer, so it feeds the SAME pipeline — we just bind THIS buffer for the
  // static draws. Sized to the cell count (the bake never exceeds one instance per cell).
  const staticBuf = device.createBuffer({ size: C.NWC * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  // the static PROPS (trees on grass corners): a second baked-once buffer, same Inst layout,
  // uploaded alongside the town on the same version tick. Drawn with the M_TREE procedural mesh
  // (no LOD — a tree is cheap and small), frustum-culled per screen exactly like the town runs.
  const propBuf = device.createBuffer({ size: C.NWC * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  let propRuns = [];
  // each screen's world AABB (subcells), constant — the frustum cull tests the visible box
  // against these to pick which screens' runs to draw ("at most, a filter for culling").
  const screenBox = [];
  for (let s = 0; s < C.NS; s++) { const sx = s % C.SX, sy = (s / C.SX) | 0;
    screenBox.push([sx * C.GW * C.SUB, sy * C.GH * C.SUB, (sx + 1) * C.GW * C.SUB, (sy + 1) * C.GH * C.SUB]); }
  // re-upload the static instances + re-read the run table ONLY when the bake changes (a
  // wall toggled, a nest moved); static_version ticks then. Per frame we touch neither.
  let staticVersion = -1, staticRuns = [], cell2static = new Int32Array(C.NWC).fill(-1), shakePatched = new Set();
  function syncStatic() {
    const v = wasm.static_version(); if (v === staticVersion) return;
    staticVersion = v;
    const nInst = wasm.static_inst_count();
    device.queue.writeBuffer(staticBuf, 0, new Uint8Array(mem(), wasm.static_inst_ptr(), nInst * C.STRIDE));
    const nr = wasm.static_run_count(), dv = new DataView(mem(), wasm.static_run_ptr(), nr * C.SRSTRIDE);
    staticRuns = [];
    for (let r = 0; r < nr; r++) { const o = r * C.SRSTRIDE;
      staticRuns.push({ screen: dv.getUint16(o, true), mesh: dv.getUint16(o + 2, true),
                        first: dv.getUint32(o + 4, true), count: dv.getUint32(o + 8, true) }); }
    // the trees re-bake on the same tick: upload them + read their per-screen runs (one M_TREE
    // run per screen). Same Inst/StaticRun layout, so they share the run stride and pipeline.
    const npi = wasm.prop_inst_count();
    device.queue.writeBuffer(propBuf, 0, new Uint8Array(mem(), wasm.prop_inst_ptr(), npi * C.STRIDE));
    const npr = wasm.prop_run_count(), pdv = new DataView(mem(), wasm.prop_run_ptr(), npr * C.SRSTRIDE);
    propRuns = [];
    for (let r = 0; r < npr; r++) { const o = r * C.SRSTRIDE;
      propRuns.push({ screen: pdv.getUint16(o, true), mesh: pdv.getUint16(o + 2, true),
                      first: pdv.getUint32(o + 4, true), count: pdv.getUint32(o + 8, true) }); }
    // the town changed, so its baked SUN shadow is stale: refit the light frustum to the tallest
    // instance (wz + hz, subcells) and re-render the depth map. Once, here — never per frame.
    const idv = new DataView(mem(), wasm.static_inst_ptr(), nInst * C.STRIDE);
    let maxTop = 0;
    cell2static.fill(-1); shakePatched.clear();   // one instance per cell -> index, for the wall-shake jolt
    for (let i = 0; i < nInst; i++) { const o = i * C.STRIDE;
      const top = idv.getInt16(o + 8, true) + idv.getInt16(o + 10, true); if (top > maxTop) maxTop = top;
      const wcx = (idv.getInt32(o, true) / C.SUB) | 0, wcy = (idv.getInt32(o + 4, true) / C.SUB) | 0;
      cell2static[wcy * C.BW + wcx] = i; }
    // trees cast shadows too, so the light frustum must reach the tallest tree's top (wz+hz).
    const pdv2 = new DataView(mem(), wasm.prop_inst_ptr(), npi * C.STRIDE);
    for (let i = 0; i < npi; i++) { const top = pdv2.getInt16(i * C.STRIDE + 8, true) + pdv2.getInt16(i * C.STRIDE + 10, true); if (top > maxTop) maxTop = top; }
    lightMVP = sunLightMatrix(maxTop);
    device.queue.writeBuffer(lightViewBuf, 0, new Float32Array(lightMVP));   // for the bake (vs_shadow)
    device.queue.writeBuffer(envBuf, 160, new Float32Array(lightMVP));       // for the sample (fs_light)
    renderShadowMap();
  }
  // jolt the STRUCK buildings: read the sim's wall_shake and offset those static instances' wx/wy
  // with chapter 3's buzz (amplitude decays over the shake's life, flipping each tick), restoring
  // any that stopped. At most WALL_SHAKE_MAX cells while a bolt is fresh — a tiny per-instance patch,
  // not a re-bake; the upload-once town is otherwise untouched. (Chapter 3 jittered the wall in
  // render.c; here the wall IS the static town, so the host applies the same buzz to its instance.)
  function applyWallShake() {
    const n = C.SHAKEMAX, cells = new Uint16Array(mem(), wasm.wall_shake_cell_ptr(), n), ts = new Uint8Array(mem(), wasm.wall_shake_t_ptr(), n);
    const now = new Set(), base = (cell) => { const wcx = cell % C.BW, wcy = (cell / C.BW) | 0; return [wcx * C.SUB + C.SUB / 2, wcy * C.SUB + C.SUB / 2]; };
    for (let e = 0; e < n; e++) {
      const t = ts[e]; if (!t) continue;
      const cell = cells[e], idx = cell2static[cell]; if (idx < 0) continue;
      const amp = (C.SHAKEAMP * t / C.SHAKEDUR) | 0, jx = (t & 1) ? amp : -amp, jy = (t & 2) ? amp : -amp, b = base(cell);
      device.queue.writeBuffer(staticBuf, idx * C.STRIDE, new Int32Array([b[0] + jx, b[1] + jy]));
      now.add(cell);
    }
    for (const cell of shakePatched) if (!now.has(cell)) { const idx = cell2static[cell]; if (idx >= 0) device.queue.writeBuffer(staticBuf, idx * C.STRIDE, new Int32Array(base(cell))); }
    shakePatched = now;
  }
  const module = device.createShaderModule({ code: WGSL });
  const screenModule = device.createShaderModule({ code: WGSL_SCREEN });

  const bgl = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: "uniform" } }] });
  const layout = device.createPipelineLayout({ bindGroupLayouts: [bgl] });
  const vbuffers = [
    { arrayStride: C.MVSTRIDE, stepMode: "vertex", attributes: [
      { shaderLocation: 0, offset: 0, format: "snorm8x4" }, { shaderLocation: 1, offset: 4, format: "snorm8x4" },
      { shaderLocation: 7, offset: 8, format: "unorm8x4" } ] },
    { arrayStride: C.STRIDE, stepMode: "instance", attributes: [
      { shaderLocation: 2, offset: 0, format: "sint32x2" }, { shaderLocation: 3, offset: 8, format: "sint16x2" },
      { shaderLocation: 4, offset: 12, format: "sint16x2" }, { shaderLocation: 5, offset: 16, format: "sint16x2" },
      { shaderLocation: 6, offset: 20, format: "unorm8x4" } ] },
  ];
  // ---- DEFERRED renderer ----------------------------------------------------------
  // The opaque scene is rendered ONCE into a G-buffer (albedo + world normal + depth); the
  // lighting is then a screen pass that reads the G-buffer. That is what makes per-mite /
  // per-FX point lights affordable later: their cost is the lit pixels they cover, not the
  // object count times the light count. For now the screen pass is just the environment.
  const GBUF_COLOR = "rgba8unorm", GBUF_NORMAL = "rgba8unorm", GBUF_POS = "rgba32float", HDR = "rgba16float", DEPTH = "depth24plus";

  // GEOMETRY: fill the G-buffer (albedo, normal, world position) + depth. No lighting here.
  const geometryPipe = device.createRenderPipeline({ layout,
    vertex: { module, entryPoint: "vs", buffers: vbuffers },
    fragment: { module, entryPoint: "fs_gbuffer", targets: [{ format: GBUF_COLOR }, { format: GBUF_NORMAL }, { format: GBUF_POS }] },
    primitive: { topology: "triangle-list", cullMode: "none" },
    depthStencil: { format: DEPTH, depthWriteEnabled: true, depthCompare: "less" } });
  // IMPOSTER (LOD1): same G-buffer write, but a 16-byte vertex (adds a UV) and an extra bind group
  // for the projected-art atlas. Writes the same targets, so the deferred lighting treats it as art.
  const vbuffersImp = [
    { arrayStride: IMP_VSTRIDE, stepMode: "vertex", attributes: [
      { shaderLocation: 0, offset: 0, format: "snorm8x4" }, { shaderLocation: 1, offset: 4, format: "snorm8x4" },
      { shaderLocation: 7, offset: 8, format: "unorm8x4" }, { shaderLocation: 8, offset: 12, format: "sint16x2" } ] },
    vbuffers[1] ];   // the same 24-byte instance layout
  const atlasBGL = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "float" } },
    { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "filtering" } } ] });
  const atlasBind = device.createBindGroup({ layout: atlasBGL, entries: [
    { binding: 0, resource: atlasTex.createView() }, { binding: 1, resource: atlasSampler } ] });
  const imposterPipe = device.createRenderPipeline({ layout: device.createPipelineLayout({ bindGroupLayouts: [bgl, atlasBGL] }),
    vertex: { module, entryPoint: "vs_imp", buffers: vbuffersImp },
    fragment: { module, entryPoint: "fs_imp", targets: [{ format: GBUF_COLOR }, { format: GBUF_NORMAL }, { format: GBUF_POS }] },
    primitive: { topology: "triangle-list", cullMode: "none" },
    depthStencil: { format: DEPTH, depthWriteEnabled: true, depthCompare: "less" } });
  // FORWARD FX: the translucent pass, now blended into the lit HDR, depth-tested against the
  // geometry but not writing it (render.c already painter-sorted the range).
  const transPipe = device.createRenderPipeline({ layout,
    vertex: { module, entryPoint: "vs", buffers: vbuffers },
    fragment: { module, entryPoint: "fs_flat", targets: [{ format: HDR, blend: {
      color: { srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha", operation: "add" },
      alpha: { srcFactor: "one", dstFactor: "one-minus-src-alpha", operation: "add" } } }] },
    primitive: { topology: "triangle-list", cullMode: "none" },
    depthStencil: { format: DEPTH, depthWriteEnabled: false, depthCompare: "less" } });
  const bind = device.createBindGroup({ layout: bgl, entries: [{ binding: 0, resource: { buffer: viewBuf } }] });

  // the ENVIRONMENT light: a directional sun (xyz dir + .w intensity), its colour, and the
  // hemisphere ambient (sky overhead / ground bounce below). Tunable; uploaded when changed.
  const env = {
    sunDir: [0.50, 0.66, 0.40], sunI: 1.15, sunCol: [1.0, 0.70, 0.42],   // EVENING: low raking warm sun, intensity raised — the screen-space shadows carry the contrast
    sky: [0.15, 0.17, 0.27], ground: [0.10, 0.08, 0.09],                 // dim dusk-blue sky ambient, dark warm ground
    sh: [560, 40, 170, 14],                                              // screen-space sun shadow: maxDist, steps, thickness, bias (subcells; SUB=256).
                                                                         // KEPT MEDIUM-RANGE ON PURPOSE: single-layer SSS can't know an occluder's far
                                                                         // side, so a LONG march past a tall building reconstructs only a sheared,
                                                                         // truncated silhouette. ~2.2 cells is where the thickness assumption holds —
                                                                         // a clean grounding shadow. 40 small steps keep the edge crisp without dither.
  };
  const norm3 = (v) => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
  // the BAKED SUN SHADOW MAP: one orthographic depth render of the static town from the sun,
  // re-baked only when the map re-bakes. SHADOW_RES is a one-time cost (the per-frame cost is one
  // PCF sample, independent of resolution); SHADOW_NBIAS is the receiver normal-offset (subcells).
  const SHADOW_RES = 2048, SHADOW_NBIAS = 28;
  let lightMVP = null;   // world (subcells) -> sun light clip; recomputed on re-bake (sun + world are otherwise fixed)
  // 224 bytes: the STATIC sun + ambient + shadow params (0..80); the per-frame eye + camera MVP
  // appended by viewWrite (80..160); the STATIC sun light MVP written by syncStatic (160..224).
  const envBuf = device.createBuffer({ size: 224, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  function writeEnv() {
    const s = norm3(env.sunDir);
    device.queue.writeBuffer(envBuf, 0, new Float32Array([
      s[0], s[1], s[2], env.sunI,
      env.sunCol[0], env.sunCol[1], env.sunCol[2], 0,
      env.sky[0], env.sky[1], env.sky[2], 0,
      env.ground[0], env.ground[1], env.ground[2], 0,
      env.sh[0], env.sh[1], env.sh[2], env.sh[3] ]));
  }
  writeEnv();
  // The SUN's orthographic light matrix (world subcells -> light clip). The sun is a parallel
  // light, so its shadow is an orthographic depth render. Fit the ortho box to the whole town's
  // world AABB (in CELLS): place the eye far back along the sun direction, transform the 8 box
  // corners into light space, and bound them — a tight box maximises depth precision. maxTopSub
  // is the tallest baked instance (subcells), so the frustum just clears the skyscraper.
  function sunLightMatrix(maxTopSub) {
    const dir = norm3(env.sunDir);                       // toward the sun
    const maxZc = Math.max(1, maxTopSub / C.SUB);        // tallest building, in cells
    const ctr = [C.BW / 2, C.BH / 2, maxZc / 2];
    const D = (C.BW + C.BH) * 1.5;                       // push the eye well outside the town
    const eye = [ctr[0] + dir[0] * D, ctr[1] + dir[1] * D, ctr[2] + dir[2] * D];
    const view = m4lookAt(eye, ctr, [0, 0, 1]);
    let lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (let cx = 0; cx < 2; cx++) for (let cy = 0; cy < 2; cy++) for (let cz = 0; cz < 2; cz++) {
      const v = m4vec(view, [cx ? C.BW : 0, cy ? C.BH : 0, cz ? maxZc : 0, 1]);  // corner -> light view space
      for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], v[k]); hi[k] = Math.max(hi[k], v[k]); }
    }
    const pad = 1;   // one-cell margin so a shadow at the very edge isn't clipped
    // view z is negative in front; near = nearest = -hi.z, far = -lo.z (both positive, near < far)
    const ortho = m4ortho(lo[0] - pad, hi[0] + pad, lo[1] - pad, hi[1] + pad, -hi[2] - pad, -lo[2] + pad);
    return m4mul(m4mul(ortho, view), m4scale(1 / C.SUB));   // accept world in SUBCELLS, like the camera MVP
  }
  // LIGHTING: env uniform + the two G-buffer textures (read via textureLoad, no sampler).
  const lightBGL = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "uniform" } },
    { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } },
    { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } },
    { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } },   // gPosition (for the shadow march)
    { binding: 5, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "depth" } },                // the baked sun depth map
    { binding: 6, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "comparison" } } ] });             // PCF compare sampler
  const lightingPipe = device.createRenderPipeline({ layout: device.createPipelineLayout({ bindGroupLayouts: [lightBGL] }),
    vertex: { module: screenModule, entryPoint: "vs_full" },
    fragment: { module: screenModule, entryPoint: "fs_light", targets: [{ format: HDR }] },
    primitive: { topology: "triangle-list" } });
  // TONEMAP: the HDR light target -> the display.
  const tonemapBGL = device.createBindGroupLayout({ entries: [
    { binding: 4, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } } ] });
  const tonemapPipe = device.createRenderPipeline({ layout: device.createPipelineLayout({ bindGroupLayouts: [tonemapBGL] }),
    vertex: { module: screenModule, entryPoint: "vs_full" },
    fragment: { module: screenModule, entryPoint: "fs_tonemap", targets: [{ format }] },
    primitive: { topology: "triangle-list" } });

  // POINT LIGHTS: each light is a cube VOLUME (the mesh cube, instanced from the light buffer),
  // additively blended into the HDR. Front faces only + no depth test, so the volume just marks
  // the pixels to light; the fragment reads the G-buffer there. The vs needs the view uniform,
  // the fs reads the three G-buffer textures — one bind group across both.
  const lightVolBGL = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: "uniform" } },
    { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } },
    { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } },
    { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } } ] });
  const lightVolPipe = device.createRenderPipeline({ layout: device.createPipelineLayout({ bindGroupLayouts: [lightVolBGL] }),
    vertex: { module, entryPoint: "vs_light", buffers: vbuffers },
    fragment: { module, entryPoint: "fs_lightvol", targets: [{ format: HDR, blend: {
      color: { srcFactor: "one", dstFactor: "one", operation: "add" },
      alpha: { srcFactor: "one", dstFactor: "one", operation: "add" } } }] },
    // cull FRONT: the camera's FLIPX reflection inverts winding, so this keeps the volume's
    // near (camera-side) faces — one fragment per covered pixel, no depth needed.
    primitive: { topology: "triangle-list", cullMode: "front" } });
  const lightBuf = device.createBuffer({ size: wasm.light_max() * C.STRIDE, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });

  // SHADOW BAKE resources: a depth-only render of the static town from the sun, into one fixed-size
  // ortho depth map. The light matrix rides its own small uniform; the depth-only pipeline reuses
  // the geometry vertex layout (vs_shadow) and writes depth alone — no colour, no fragment shader.
  const shadowTex = device.createTexture({ size: [SHADOW_RES, SHADOW_RES], format: "depth32float",
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
  const shadowView = shadowTex.createView();
  const shadowSampler = device.createSampler({ compare: "less", magFilter: "linear", minFilter: "linear" });   // 2x2 hardware PCF
  const lightViewBuf = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const shadowBind = device.createBindGroup({ layout: bgl, entries: [{ binding: 0, resource: { buffer: lightViewBuf } }] });
  const shadowPipe = device.createRenderPipeline({ layout,
    vertex: { module, entryPoint: "vs_shadow", buffers: vbuffers },
    // cull FRONT faces -> store only the occluders' BACK (far-from-sun) depth (second-depth shadow
    // mapping). A light-facing surface is then never its own occluder, so it can't self-shadow: that
    // kills the acne on sun-lit roofs AND the ragged self-shadow where the cheap LOD1 caster's roof
    // disagrees with the LOD0 roof the camera draws up close. The cast shadows are unchanged (a solid
    // building's silhouette is the same from either face). Buildings are closed, so no light leaks.
    primitive: { topology: "triangle-list", cullMode: "front" },
    depthStencil: { format: "depth32float", depthWriteEnabled: true, depthCompare: "less" } });   // no fragment: depth only
  // (Re)bake the sun depth map: draw the whole static town (LOD1 massing — the bulk is all a shadow
  // needs) from the light matrix. Called only when the town changes, exactly like the instance bake.
  function renderShadowMap() {
    const enc = device.createCommandEncoder();
    const sp = enc.beginRenderPass({ colorAttachments: [],
      depthStencilAttachment: { view: shadowView, depthClearValue: 1.0, depthLoadOp: "clear", depthStoreOp: "store" } });
    sp.setPipeline(shadowPipe); sp.setBindGroup(0, shadowBind);
    sp.setVertexBuffer(0, meshBuf); sp.setVertexBuffer(1, staticBuf);
    for (const run of staticRuns) { const m = meshTable[run.mesh + C.LOD2OFF]; sp.draw(m.cnt, run.count, m.off, run.first); }
    // the trees cast into the same baked map (their real M_TREE mesh — no LOD massing to swap).
    sp.setVertexBuffer(1, propBuf);
    for (const run of propRuns) { const m = meshTable[run.mesh]; sp.draw(m.cnt, run.count, m.off, run.first); }
    sp.end();
    device.queue.submit([enc.finish()]);
  }

  // the viewport-sized targets (G-buffer + HDR + depth), rebuilt on resize; the screen-pass and
  // light-volume bind groups reference them, so they rebuild together.
  let gColorTex = null, gNormalTex = null, gPosTex = null, hdrTex = null, depthTex = null, lightBind = null, lightVolBind = null, tonemapBind = null;
  function ensureTargets() {
    if (gColorTex && gColorTex.width === canvas.width && gColorTex.height === canvas.height) return;
    for (const t of [gColorTex, gNormalTex, gPosTex, hdrTex, depthTex]) if (t) t.destroy();
    const size = [canvas.width, canvas.height], RA = GPUTextureUsage.RENDER_ATTACHMENT, TB = GPUTextureUsage.TEXTURE_BINDING;
    gColorTex  = device.createTexture({ size, format: GBUF_COLOR,  usage: RA | TB });
    gNormalTex = device.createTexture({ size, format: GBUF_NORMAL, usage: RA | TB });
    gPosTex    = device.createTexture({ size, format: GBUF_POS,    usage: RA | TB });
    hdrTex     = device.createTexture({ size, format: HDR,         usage: RA | TB });
    depthTex   = device.createTexture({ size, format: DEPTH,       usage: RA });
    lightBind = device.createBindGroup({ layout: lightBGL, entries: [
      { binding: 0, resource: { buffer: envBuf } },
      { binding: 1, resource: gColorTex.createView() },
      { binding: 2, resource: gNormalTex.createView() },
      { binding: 3, resource: gPosTex.createView() },
      { binding: 5, resource: shadowView },                  // the baked sun depth map (static)
      { binding: 6, resource: shadowSampler } ] });
    lightVolBind = device.createBindGroup({ layout: lightVolBGL, entries: [
      { binding: 0, resource: { buffer: viewBuf } },
      { binding: 1, resource: gColorTex.createView() },
      { binding: 2, resource: gNormalTex.createView() },
      { binding: 3, resource: gPosTex.createView() } ] });
    tonemapBind = device.createBindGroup({ layout: tonemapBGL, entries: [{ binding: 4, resource: hdrTex.createView() }] });
  }
  // internal render scale (presentation-only): the deferred passes (lighting, point lights, FX,
  // tonemap) are full-screen and fill/bandwidth-bound, so their cost scales with the pixel count.
  // Dropping this below 1 renders the G-buffer + HDR smaller and lets CSS upscale the canvas — the
  // single biggest lever on a fill-bound mobile GPU, and a live A/B test of "is it resolution?".
  let renderScale = 1;
  function resize() {
    const dpr = Math.min(devicePixelRatio || 1, 2) * renderScale;
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr)), h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  }
  new ResizeObserver(resize).observe(canvas); resize();

  // --- the top-down PERSPECTIVE camera: drag = pan · shift/right/2-finger drag = orbit · pinch/wheel = zoom
  // The projection is a host-built model-view-projection matrix (the shader just
  // applies it); the camera is ONLY that uniform. camX,camY is the ground point the
  // camera looks at (subcells); zoom sets how close it is. A slight PITCH from
  // straight-down gives the angled, perspective look (leaning south, so screen-up is
  // north). viewWrite writes the MVP and remembers its inverse so a pixel can be cast
  // back onto the ground. Nothing here touches the simulation.
  let useAssets = true;   // default: show the baked town + meshes; toggle OFF -> the placeholder massing model
  // render-profiling: live A/B toggles for the heavy layers (watch the frame time move on the
  // actual device — the surest way to attribute GPU cost where timestamp-query isn't available).
  let drawTown = true, drawTrees = true, drawLights = true;

  // GPU timing via timestamp-query (when supported): one write at the start + end of each pass.
  const GP = ["geom", "light", "plight", "fx", "tone"];   // the per-pass slots
  const tsCount = GP.length * 2;
  let tsQuery = null, tsResolve = null; const tsFree = [];
  if (canTimestamp) {
    tsQuery = device.createQuerySet({ type: "timestamp", count: tsCount });
    tsResolve = device.createBuffer({ size: tsCount * 8, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
    for (let i = 0; i < 4; i++) tsFree.push(device.createBuffer({ size: tsCount * 8, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }));
  }
  // timestampWrites descriptor for pass `name` (or undefined when unsupported -> the pass is plain)
  const tw = (name) => canTimestamp ? { querySet: tsQuery, beginningOfPassWriteIndex: GP.indexOf(name) * 2, endOfPassWriteIndex: GP.indexOf(name) * 2 + 1 } : undefined;
  // the running profile: EMA-smoothed CPU sections (ms), per-pass GPU time (ms), and draw counts.
  const prof = { sim: 0, enc: 0, cpu: 0, gpu: {}, n: {}, ran: {}, ts: canTimestamp };
  const ema = (o, k, v) => { o[k] = o[k] ? o[k] * 0.9 + v * 0.1 : v; };
  const profEl = document.getElementById("prof");
  let profFrame = 0;

  // ---- the controlled diagnostic sweep -------------------------------------------------
  // The per-pass GPU timestamps overlap on tile GPUs (their Σ exceeds the real frame), so the
  // RELIABLE signal is the wall-clock frame time itself. This runs a fixed A/B sweep — toggling
  // one layer / the resolution at a time and measuring the achieved frame time (median / p95 /
  // min over many frames) — then emits a text report to copy. Deltas between rows attribute cost.
  const DIAG_WARM = 20, DIAG_N = 80;   // discard 20 frames after each switch, then sample 80
  const DIAG_CONFIGS = [
    { k: "baseline (all, 1.0x)",     town: 1, trees: 1, lights: 1, scale: 1.0 },
    { k: "no trees",                 town: 1, trees: 0, lights: 1, scale: 1.0 },
    { k: "no point lights",          town: 1, trees: 1, lights: 0, scale: 1.0 },
    { k: "no town",                  town: 0, trees: 1, lights: 1, scale: 1.0 },
    { k: "no trees+lights",          town: 1, trees: 0, lights: 0, scale: 1.0 },
    { k: "0.5x res (all)",           town: 1, trees: 1, lights: 1, scale: 0.5 },
    { k: "0.5x res, no lights",      town: 1, trees: 1, lights: 0, scale: 0.5 },
    { k: "0.5x res, minimal",        town: 1, trees: 0, lights: 0, scale: 0.5 },
    { k: "0.75x res (all)",          town: 1, trees: 1, lights: 1, scale: 0.75 },
  ];
  const diag = {
    active: false, done: false, ci: 0, phase: "warm", warm: 0, coll: [], results: [],
    snap: null, gmin: Infinity, report: "", view: "",
    start() {
      if (this.active) return;
      this.snap = { town: drawTown, trees: drawTrees, lights: drawLights, scale: renderScale };
      this.active = true; this.done = false; this.ci = 0; this.results = []; this.gmin = Infinity;
      try { const c = camScreen(); this.view = `screen ${c.sx},${c.sy} · zoom ${zoom.toFixed(2)} · pitch ${Math.round(pitch / DEG)}°`; }
      catch (e) { this.view = "?"; }
      this.apply(0);
    },
    apply(i) { const c = DIAG_CONFIGS[i]; drawTown = !!c.town; drawTrees = !!c.trees; drawLights = !!c.lights;
      if (renderScale !== c.scale) { renderScale = c.scale; resize(); }
      this.phase = "warm"; this.warm = DIAG_WARM; this.coll = []; },
    step(dt) {                                    // called at the top of each frame while active
      const ms = dt * 1000; if (ms > 0.2 && ms < this.gmin) this.gmin = ms;
      if (this.phase === "warm") { if (--this.warm <= 0) this.phase = "coll"; return; }
      this.coll.push(ms);
      if (this.coll.length >= DIAG_N) {
        const s = this.coll.slice().sort((a, b) => a - b), n = s.length;
        const r = { k: DIAG_CONFIGS[this.ci].k, med: s[n >> 1], p95: s[Math.min(n - 1, Math.floor(0.95 * n))], min: s[0] };
        r.gpu = {}; for (const pn of GP) r.gpu[pn] = (prof.ran[pn] && prof.gpu[pn]) ? prof.gpu[pn] : 0;   // per-pass GPU this config
        this.results.push(r);
        if (++this.ci >= DIAG_CONFIGS.length) this.finish(); else this.apply(this.ci);
      }
    },
    finish() {
      drawTown = this.snap.town; drawTrees = this.snap.trees; drawLights = this.snap.lights;
      if (renderScale !== this.snap.scale) { renderScale = this.snap.scale; resize(); }
      const sT = document.getElementById("t_town"), sR = document.getElementById("t_trees"), sL = document.getElementById("t_lights");
      if (sT) sT.checked = drawTown; if (sR) sR.checked = drawTrees; if (sL) sL.checked = drawLights;
      this.active = false; this.done = true; this.report = this.build();
      const cp = document.getElementById("diagCopy"), bt = document.getElementById("diagBtn");
      if (cp) cp.disabled = false; if (bt) bt.disabled = false;
      if (profEl) profEl.textContent = this.report;
    },
    build() {
      const f = (v) => (v ? v.toFixed(2) : "–"), dprCap = Math.min(devicePixelRatio || 1, 2);
      const back = `${Math.floor(canvas.clientWidth * dprCap)}x${Math.floor(canvas.clientHeight * dprCap)}`;
      const info = adapter.info || {}, N = prof.n || {};
      const L = [];
      L.push("=== tank-combat ch4 — render diagnostics ===");
      L.push("version: " + (window.TANK_VERSION || "?"));
      L.push("ua: " + navigator.userAgent);
      L.push("gpu: " + [info.vendor, info.architecture, info.device, info.description].filter(Boolean).join(" / ") + "  | timestamp-query: " + (canTimestamp ? "yes" : "no"));
      L.push(`dpr: ${devicePixelRatio} (capped ${dprCap})  css: ${canvas.clientWidth}x${canvas.clientHeight}  backing@1.0x: ${back}`);
      L.push(`refresh est: ~${this.gmin < Infinity ? Math.round(1000 / this.gmin) : "?"} Hz (fastest frame ${this.gmin.toFixed(1)} ms)`);
      L.push(`view: ${this.view}`);
      L.push(`draws@baseline: town ${N.staticRuns} runs · trees ${N.trees} (${N.treeRuns} runs) · opaque ${N.inst} inst · lights ${N.lights}`);
      L.push(`cpu@baseline: sim ${prof.sim.toFixed(2)} ms · encode ${prof.enc.toFixed(2)} ms`);
      L.push("");
      L.push(`frame time per config — median / p95 / min ms  (~median fps), ${DIAG_N} samples each.`);
      if (canTimestamp) L.push(`(gpu pass ms below are RELATIVE only — passes overlap on tile GPUs, so they over-sum; compare a pass across configs, not its absolute value.)`);
      for (const r of this.results) {
        L.push(`  ${r.k.padEnd(24)} ${r.med.toFixed(1)} / ${r.p95.toFixed(1)} / ${r.min.toFixed(1)}  (~${Math.round(1000 / r.med)} fps)`);
        if (canTimestamp) L.push(`      gpu: geom ${f(r.gpu.geom)} light ${f(r.gpu.light)} plights ${f(r.gpu.plight)} fx ${f(r.gpu.fx)} tone ${f(r.gpu.tone)}`);
      }
      return L.join("\n");
    },
  };
  let camX = C.BW * C.SUB / 2, camY = C.BH * C.SUB / 2, zoom = 1, follow = false, followTank = -1, followOffX = 0, followOffY = 0;
  let camEye = [0, 0, 0];   // the eye in world subcells (set by viewWrite) — feeds the LOD distance test
  let invMVP = null;
  const ZMIN = 0.85, ZMAX = 20;
  const DEG = Math.PI / 180;
  // pitch = tilt off straight-down, yaw = orbit around the look-at point, fov = lens.
  // All three are presentation-only and live (sliders + rotate-drag); the defaults are
  // the shipped "slight angled offset, looking north" framing. The simulation never
  // sees any of them — they only rebuild the MVP uniform.
  const PITCH0 = 30 * DEG, FOV0 = 36 * DEG;
  const PITCH_MIN = 5 * DEG, PITCH_MAX = 80 * DEG, FOV_MIN = 15 * DEG, FOV_MAX = 70 * DEG;
  let pitch = PITCH0, yaw = 0, fov = FOV0;
  let lodScale = 1;   // tuning: scales the LOD0/LOD1 crossover DISTANCE (1 = the reference framing;
                      // <1 pulls the cheaper LODs closer, >1 keeps full art farther out)
  const LOD_FAR = 2.5;   // the LOD1(imposter)/LOD2(flat massing) crossover, as a multiple of the LOD0/LOD1 distance
  // Keep the camera below the horizon: the TOP edge of the frustum must stay under the
  // skyline (angle below horizontal = (90° − pitch) − fov/2 > 0), else the ground runs to
  // infinity and clips. So the usable pitch tightens as fov widens; a small margin keeps
  // the far ground from racing off to infinity (and wrecking depth precision).
  const HALF_PI = Math.PI / 2;
  const maxPitch = () => Math.min(PITCH_MAX, HALF_PI - fov / 2 - 5 * DEG);
  const clampPitch = (p) => Math.max(PITCH_MIN, Math.min(maxPitch(), p));
  const clampFov = (f) => Math.max(FOV_MIN, Math.min(FOV_MAX, f));
  function clampCam() {
    zoom = Math.max(ZMIN, Math.min(ZMAX, zoom));
    camX = Math.max(0, Math.min(C.BW * C.SUB, camX));
    camY = Math.max(0, Math.min(C.BH * C.SUB, camY));
  }
  function baseDist() {                  // camera distance (cells) that fits the whole world at zoom 1
    const t = Math.tan(fov / 2), aspect = canvas.width / canvas.height;
    return Math.max((C.BH * 0.62) / (t / Math.cos(pitch)), (C.BW * 0.62) / (t * aspect));
  }
  function viewWrite() {
    const dist = baseDist() / zoom;                      // cells; zoom in -> closer
    const t = [camX / C.SUB, camY / C.SUB, 0];           // look-at target, in cells
    // the eye orbits the target: pitch tilts it off straight-down, yaw swings it around
    // (yaw 0 == due south, looking north). up is the eye's ground heading, so screen-up
    // always points away from the camera and turns with the orbit.
    const sp = Math.sin(pitch), cp = Math.cos(pitch), sy = Math.sin(yaw), cy = Math.cos(yaw);
    const eye = [t[0] + dist * sp * sy, t[1] + dist * sp * cy, t[2] + dist * cp];
    const view = m4lookAt(eye, t, [-sy, -cy, 0]);
    // Far plane reaches the farthest visible ground — the point under the TOP frustum edge,
    // which recedes toward the horizon as the camera tilts. (eye height / sin(edge angle))
    // + a world-sized margin. near stays a small fraction of the look distance.
    const topA = Math.max(3 * DEG, (HALF_PI - pitch) - fov / 2);
    const far = (dist * cp) / Math.sin(topA) + dist + (C.BW + C.BH);
    const proj = m4perspective(fov, canvas.width / canvas.height, Math.max(0.5, dist * 0.05), far);
    // The sim grid is y-DOWN (+y = south), so a plain north-up perspective camera would
    // mirror east/west versus the flat minimap. Reflect clip-x to put east on the right,
    // matching the minimap. (Pipelines cull nothing, so the flipped winding is harmless;
    // the inverse below stays consistent, so pick/pan/hover need no special-casing.)
    const flipX = [-1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1];
    const mvp = m4mul(flipX, m4mul(m4mul(proj, view), m4scale(1 / C.SUB)));   // subcells -> clip
    invMVP = m4invert(mvp);
    device.queue.writeBuffer(viewBuf, 0, new Float32Array(mvp));
    camEye = [eye[0] * C.SUB, eye[1] * C.SUB, eye[2] * C.SUB];   // subcells — for the shadow march + the LOD distance
    // append the per-frame camera (eye in subcells) + MVP for the screen-space shadow march
    device.queue.writeBuffer(envBuf, 80, new Float32Array([ ...camEye, SHADOW_NBIAS, ...mvp ]));   // eye.w = shadow-map normal-offset bias
  }
  // The LOD distance: at MAX zoom + 56-degree pitch, EVERYTHING visible is highest detail (the
  // user's reference), so the LOD0/LOD1 split is the eye-to-farthest-visible-ground distance of
  // that reference view. A town SCREEN closer than this draws the full art; farther draws the
  // simplified colour-matched massing. Squared (subcells^2) to skip the per-screen sqrt.
  function lodDist2() {
    const p = 56 * DEG, f = FOV0, t = Math.tan(f / 2), aspect = canvas.width / canvas.height;
    const bd = Math.max((C.BH * 0.62) / (t / Math.cos(p)), (C.BW * 0.62) / (t * aspect));
    const dist = bd / ZMAX, eyeH = dist * Math.cos(p);
    const topA = Math.max(3 * DEG, (HALF_PI - p) - f / 2), farGround = eyeH / Math.sin(topA);
    const d = Math.hypot(farGround, eyeH) * C.SUB;
    return d * d;
  }
  // cast a screen NDC point onto the ground plane (z=0) -> world subcells
  function groundAt(ndcx, ndcy) {
    if (!invMVP) return { x: camX, y: camY };
    const a = m4vec(invMVP, [ndcx, ndcy, 0, 1]), b = m4vec(invMVP, [ndcx, ndcy, 1, 1]);
    const ax = a[0]/a[3], ay = a[1]/a[3], az = a[2]/a[3], bx = b[0]/b[3], by = b[1]/b[3], bz = b[2]/b[3];
    const k = az - bz; const tt = Math.abs(k) < 1e-6 ? 0 : az / k;          // where z=0 along the ray
    return { x: ax + tt * (bx - ax), y: ay + tt * (by - ay) };
  }
  function screenToWorld(clientX, clientY) {
    const r = canvas.getBoundingClientRect();
    return groundAt((clientX - r.left) / r.width * 2 - 1, -((clientY - r.top) / r.height * 2 - 1));
  }
  function cellAt(clientX, clientY) {
    const p = screenToWorld(clientX, clientY);
    const wcx = Math.floor(p.x / C.SUB), wcy = Math.floor(p.y / C.SUB);
    if (wcx < 0 || wcx >= C.BW || wcy < 0 || wcy >= C.BH) return null;
    return { wcx, wcy };
  }
  // the world box the camera shows (its four corners cast to the ground) — what the
  // renderer must emit; the cost scales with this, not the whole world.
  function visibleBox() {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const [nx, ny] of [[-1, 1], [1, 1], [-1, -1], [1, -1]]) {
      const p = groundAt(nx, ny);
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

  // while following, the camera looks at (tank + offset); keep the offset within ~a screen
  function clampOffset() {
    const mx = C.GW * C.SUB, my = C.GH * C.SUB;
    followOffX = Math.max(-mx, Math.min(mx, followOffX));
    followOffY = Math.max(-my, Math.min(my, followOffY));
  }
  // drag-pan: keep the world point you grabbed under the cursor (correct in perspective).
  // While following a tank, pan shifts the follow OFFSET instead, so it stays a follow-cam.
  function panBy(prevX, prevY, curX, curY) {
    const a = screenToWorld(prevX, prevY), b = screenToWorld(curX, curY);
    const ddx = b.x - a.x, ddy = b.y - a.y;
    if (follow && followTank >= 0) { followOffX -= ddx; followOffY -= ddy; clampOffset(); }
    else { camX -= ddx; camY -= ddy; clampCam(); }
  }
  // zoom by `factor`. Free-cam keeps the world point under (clientX,clientY) fixed; while
  // following, zoom just changes the level and stays centred on the tank.
  function zoomBy(factor, clientX, clientY) {
    const free = !(follow && followTank >= 0);
    const before = free ? screenToWorld(clientX, clientY) : null;
    zoom = Math.max(ZMIN, Math.min(ZMAX, zoom * factor));
    if (free) {
      viewWrite();                                      // refresh the matrix (+ inverse) for the new zoom
      const after = screenToWorld(clientX, clientY);
      camX += before.x - after.x; camY += before.y - after.y; clampCam();
    }
    if (zR) zR.value = zoom.toFixed(2);
  }
  // orbit the camera around the look-at point: horizontal drag swings yaw, vertical drag
  // tilts pitch (drag up = more oblique / more 3-D, drag down = flatter / more overhead).
  // Render-only, exactly like pan and zoom — it only rebuilds the MVP uniform.
  function rotateBy(dx, dy) {
    yaw -= dx * 0.005;          // clip-x is reflected (east on the right), so negate to keep
                                // drag-right swinging the view the natural way
    pitch = clampPitch(pitch - dy * 0.005);
    const pd = (pitch / DEG).toFixed(0);
    if (pR) pR.value = pd; if (pV) pV.textContent = pd + "°";
  }

  // pointers: one-finger / left drag = pan; shift- or right/middle-drag = orbit (rotate
  // + tilt); two fingers = pinch zoom AND drag-to-orbit; a tap = select / path.
  const pointers = new Map();
  let dragMoved = 0, pinchPrev = 0, midPrev = null, rotateDrag = false;
  const pdist = () => { const p = [...pointers.values()]; return Math.hypot(p[0].x - p[1].x, p[0].y - p[1].y); };
  const pmid  = () => { const p = [...pointers.values()]; return [(p[0].x + p[1].x) / 2, (p[0].y + p[1].y) / 2]; };
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());      // right-drag orbits, so no menu
  canvas.addEventListener("dragstart", (e) => e.preventDefault());        // never native-drag the canvas
  canvas.addEventListener("pointerdown", (e) => {
    // own the gesture: stop the browser starting a text-selection or image-drag, which
    // otherwise leaves the cursor stuck as "no-drop" and swallows the pan.
    e.preventDefault();
    try { canvas.setPointerCapture(e.pointerId); } catch { /* pointer already gone */ }
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    dragMoved = 0;
    // a mouse drag rotates when Shift is held or the right button is used
    rotateDrag = e.pointerType === "mouse" && (e.shiftKey || e.button === 2);
    if (pointers.size === 2) { pinchPrev = pdist(); midPrev = pmid(); }
  });
  canvas.addEventListener("pointermove", (e) => {
    lastMouse = { x: e.clientX, y: e.clientY };
    const prev = pointers.get(e.pointerId); if (!prev) return;            // hovering, no button down here
    const dx = e.clientX - prev.x, dy = e.clientY - prev.y;
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY }); dragMoved += Math.abs(dx) + Math.abs(dy);
    if (pointers.size === 1) {
      if (rotateDrag) rotateBy(dx, dy); else panBy(prev.x, prev.y, e.clientX, e.clientY);
    } else if (pointers.size >= 2) {                                      // pinch -> zoom, centroid drag -> orbit
      const d = pdist(), [mx, my] = pmid();
      if (pinchPrev > 0) zoomBy(d / pinchPrev, mx, my);
      if (midPrev) rotateBy(mx - midPrev[0], my - midPrev[1]);
      pinchPrev = d; midPrev = [mx, my];
    }
  });
  function endPointer(e) {
    const was = pointers.size, wasRotate = rotateDrag; pointers.delete(e.pointerId);
    if (was === 1 && dragMoved < 8 && !wasRotate) clickAt(e.clientX, e.clientY);   // a tap, not a drag/orbit
    if (pointers.size < 2) { pinchPrev = 0; midPrev = null; }
    if (pointers.size === 0) rotateDrag = false;
  }
  canvas.addEventListener("pointerup", endPointer);
  canvas.addEventListener("pointercancel", endPointer);
  canvas.addEventListener("pointerleave", () => { lastMouse = null; });
  canvas.addEventListener("wheel", (e) => { e.preventDefault(); zoomBy(Math.exp(-e.deltaY * 0.0015), e.clientX, e.clientY); }, { passive: false });

  // a tap: a tank cell -> cycle its state (and follow it); else send the auto-path tank there
  function clickAt(clientX, clientY) {
    const c = cellAt(clientX, clientY); if (!c) return;
    const xy = view.xy();
    for (let t = 0; t < C.NT; t++)
      if ((xy[2 * t] >> 8) === c.wcx && (xy[2 * t + 1] >> 8) === c.wcy) {
        wasm.cycle_tank(t);
        if (view.tstate()[t] !== 0) { followTank = t; follow = true; followOffX = followOffY = 0; }  // now selected -> follow it
        else { follow = false; followTank = -1; }                        // cycled to unselected -> free
        return;
      }
    const sel = wasm.selected();
    if (sel !== 255 && view.tstate()[sel] === 1 /*AUTOPATH*/) wasm.set_dest(sel, c.wcx, c.wcy);
  }

  document.getElementById("camlabel").addEventListener("click", () => setPicker(!pickerOpen));
  const arrowStep = (dx, dy) => { camX += dx * C.GW * C.SUB; camY += dy * C.GH * C.SUB; follow = false; clampCam(); };
  document.querySelector(".scrollbtn.su").onclick = () => arrowStep(0, -1);
  document.querySelector(".scrollbtn.sd").onclick = () => arrowStep(0, 1);
  document.querySelector(".scrollbtn.sl").onclick = () => arrowStep(-1, 0);
  document.querySelector(".scrollbtn.sr").onclick = () => arrowStep(1, 0);

  // toolbar camera buttons: per-tank follow-cam + a free-view button
  for (const b of document.querySelectorAll(".followbtn"))
    b.onclick = () => { const t = +b.dataset.tank; wasm.select_tank(t); followTank = t; follow = true; followOffX = followOffY = 0; };
  const freeBtn = document.getElementById("freecam");
  if (freeBtn) freeBtn.onclick = () => { follow = false; followTank = -1; };
  function updateCamUI() {
    const onTank = follow && followTank >= 0;
    for (const b of document.querySelectorAll(".followbtn"))
      b.classList.toggle("active", onTank && +b.dataset.tank === followTank);
    if (freeBtn) freeBtn.classList.toggle("active", !onTank);
  }

  // render-only controls (presentation): low-poly art toggle + a camera zoom slider
  const aT = document.getElementById("assetsToggle"); if (aT) aT.onchange = () => { useAssets = aT.checked; };
  // render-profile layer toggles: drop a layer to see its frame-time cost live
  const tTown = document.getElementById("t_town"); if (tTown) tTown.onchange = () => { drawTown = tTown.checked; diag.done = false; };
  const tTrees = document.getElementById("t_trees"); if (tTrees) tTrees.onchange = () => { drawTrees = tTrees.checked; diag.done = false; };
  const tLights = document.getElementById("t_lights"); if (tLights) tLights.onchange = () => { drawLights = tLights.checked; diag.done = false; };
  // the controlled diagnostic sweep + copy-to-clipboard
  const diagBtn = document.getElementById("diagBtn"), diagCopy = document.getElementById("diagCopy");
  if (diagBtn) diagBtn.onclick = () => { if (diag.active) return; diagBtn.disabled = true; diagCopy.disabled = true; diag.start(); };
  if (diagCopy) diagCopy.onclick = async () => {
    try { await navigator.clipboard.writeText(diag.report); }
    catch (e) {   // clipboard API blocked (insecure ctx / permissions) — fall back to a selection
      const ta = document.createElement("textarea"); ta.value = diag.report; document.body.appendChild(ta);
      ta.select(); try { document.execCommand("copy"); } catch (_) {} ta.remove();
    }
    const o = diagCopy.textContent; diagCopy.textContent = "copied!"; setTimeout(() => { diagCopy.textContent = o; }, 1200);
  };
  const zR = document.getElementById("zoom"); if (zR) zR.oninput = () => { zoom = parseFloat(zR.value) || 1; clampCam(); };
  // live pitch (tilt off straight-down) + fov (lens) — presentation-only, like zoom
  const pR = document.getElementById("pitch"), pV = document.getElementById("pitchval");
  if (pR) pR.oninput = () => { pitch = clampPitch((parseFloat(pR.value) || 30) * DEG); pR.value = (pitch / DEG).toFixed(0); if (pV) pV.textContent = pR.value + "°"; };
  const fR = document.getElementById("fov"), fV = document.getElementById("fovval");
  // widening fov lowers the horizon, so re-clamp pitch and refresh its slider range
  if (fR) fR.oninput = () => { fov = clampFov((parseFloat(fR.value) || 36) * DEG); if (fV) fV.textContent = fR.value + "°"; pitch = clampPitch(pitch); syncViewUI(); };
  // live LOD distance: scales the LOD0->LOD1 crossover (presentation-only, like pitch/fov)
  const lR = document.getElementById("loddist"), lV = document.getElementById("loddistval");
  if (lR) lR.oninput = () => { lodScale = parseFloat(lR.value) || 1; if (lV) lV.textContent = lodScale.toFixed(2) + "×"; };
  if (lV && lR) lV.textContent = (parseFloat(lR.value) || 1).toFixed(2) + "×";
  function syncViewUI() {
    if (zR) zR.value = zoom.toFixed(2);
    if (pR) { pR.max = Math.round(maxPitch() / DEG); pR.value = (pitch / DEG).toFixed(0); } if (pV) pV.textContent = (pitch / DEG).toFixed(0) + "°";
    if (fR) fR.value = (fov / DEG).toFixed(0);   if (fV) fV.textContent = (fov / DEG).toFixed(0) + "°";
  }
  syncViewUI();   // set the pitch slider's fov-dependent max + initial readouts

  // 'f' frames the selected tank WITHOUT changing the camera mode: in the free camera it
  // centres the view on the tank; in a follow camera it (re)targets that tank and recentres.
  const FRAME_ZOOM = 6;
  function frameSelected() {
    const sel = wasm.selected(); if (sel === 255) return;
    zoom = Math.max(zoom, FRAME_ZOOM);
    if (follow) { followTank = sel; followOffX = followOffY = 0; }       // stay following — frame the selection
    else { const xy = view.xy(); camX = xy[2 * sel]; camY = xy[2 * sel + 1]; clampCam(); }
    if (zR) zR.value = zoom.toFixed(2);
  }
  addEventListener("keydown", (e) => {
    if (e.target && /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName)) return;
    if (e.code === "KeyF") { frameSelected(); e.preventDefault(); }
  });

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
  dbg.onReset = () => {
    wasm.init(); camX = C.BW * C.SUB / 2; camY = C.BH * C.SUB / 2; zoom = 1; follow = false; followTank = -1; followOffX = followOffY = 0;
    yaw = 0; pitch = PITCH0; fov = FOV0; syncViewUI(); setPicker(false);
  };

  // the render-profile readout: frame time, CPU sections, per-pass GPU time (when available),
  // and the live draw counts. The GEOM pass time bundles town + trees + dynamic opaque, so to
  // read a single layer's GPU cost, toggle it off and watch GEOM (and the frame) drop.
  function updateProfPanel() {
    const g = prof.gpu, R = prof.ran || {}, ms = (p) => (R[p] && g[p] ? g[p].toFixed(2) : "–"), N = prof.n;
    const gpuTotal = GP.reduce((a, p) => a + (R[p] ? g[p] || 0 : 0), 0);
    let s = `<b>frame</b> ${((prof.dt || 0) * 1000).toFixed(1)} ms (${(prof.fps || 0).toFixed(0)} fps)    `
          + `<b>CPU</b> sim ${prof.sim.toFixed(2)} · encode ${prof.enc.toFixed(2)} · total ${prof.cpu.toFixed(2)} ms\n`;
    s += canTimestamp
      ? `<b>GPU</b> geom ${ms("geom")} · light ${ms("light")} · plights ${ms("plight")} · fx ${ms("fx")} · tone ${ms("tone")} · <b>Σ ${gpuTotal.toFixed(2)} ms</b> (passes overlap on tile GPUs)\n`
      : `<b>GPU</b> per-pass timing unavailable here — toggle town / trees / point lights to A/B each layer's frame cost\n`;
    s += `<b>draws</b> town ${N.staticRuns || 0} runs · trees ${N.trees || 0} (${N.treeRuns || 0} runs) · opaque ${N.inst || 0} inst · lights ${N.lights || 0}`;
    profEl.innerHTML = s;
  }

  function frame(now) {
    let dt = (now - last) / 1000; last = now; if (dt > 0.25) dt = 0.25;
    fps = fps * 0.9 + (1 / Math.max(dt, 1e-4)) * 0.1;
    if (diag.active) diag.step(dt);   // the sweep sets the layer flags + render scale for THIS frame
    const sel = wasm.selected(), ts = view.tstate();
    const manual = sel !== 255 && ts[sel] === 2;

    let bits = 0; for (const c of held) bits |= KEYMAP[c]; bits |= touchBits;
    if (manual) wasm.set_input(sel, bits);
    document.querySelector(".pad").style.display = manual ? "grid" : "none";

    if (stepOnce) { tick(); stepOnce = false; acc = 0; }
    else if (!paused) { acc += dt; let s = 0; while (acc >= TICK_DT && s < 6) { tick(); acc -= TICK_DT; s++; } }
    else acc = 0;

    // follow-cam: track the chosen tank + your pan offset; pan/zoom/orbit stay relative to
    // the follow (they never drop it). Snap on the toroidal wrap.
    if (follow && followTank >= 0) {
      const xy = view.xy(), tx = xy[2 * followTank] + followOffX, ty = xy[2 * followTank + 1] + followOffY;
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

    const cpu0 = performance.now();     // CPU profile: prep + uploads + the encode below
    const n = wasm.inst_count();
    device.queue.writeBuffer(instBuf, 0, view.instBytes(n));
    const nLights = wasm.light_count();   // the deferred point lights (mites + FX), rebuilt each frame
    if (nLights) device.queue.writeBuffer(lightBuf, 0, new Uint8Array(mem(), wasm.light_ptr(), nLights * C.STRIDE));
    syncStatic();                       // upload-once the baked town (no-op unless it re-baked)
    applyWallShake();                   // jolt any struck building this frame (patches a few instances)
    ensureTargets();
    const enc0 = performance.now();     // CPU profile: just the command-encoder build (the draw loops)
    const enc = device.createCommandEncoder();
    let dStaticRuns = 0, dTreeRuns = 0, dTrees = 0;   // draw counts, for the profiler

    // 1) GEOMETRY -> the G-buffer (albedo + world normal + world position) + depth. Drawn once.
    const gpass = enc.beginRenderPass({
      colorAttachments: [
        { view: gColorTex.createView(),  clearValue: { r: 0, g: 0, b: 0, a: 0 }, loadOp: "clear", storeOp: "store" },
        { view: gNormalTex.createView(), clearValue: { r: 0, g: 0, b: 0, a: 0 }, loadOp: "clear", storeOp: "store" },
        { view: gPosTex.createView(),    clearValue: { r: 0, g: 0, b: 0, a: 0 }, loadOp: "clear", storeOp: "store" } ],
      depthStencilAttachment: { view: depthTex.createView(), depthClearValue: 1.0, depthLoadOp: "clear", depthStoreOp: "store" },
      timestampWrites: tw("geom") });
    gpass.setBindGroup(0, bind); gpass.setVertexBuffer(0, meshBuf); gpass.setPipeline(geometryPipe);
    // per-SCREEN LOD, THREE tiers by distance: LOD0 full art (near) -> LOD1 textured imposter (mid)
    // -> LOD2 flat massing (far). d1 = lodDist2()*lodScale^2 is the LOD0/LOD1 crossover; LOD_FAR
    // widens it to the LOD1/LOD2 crossover. Squared distances; the eye is up at camEye.
    const d1 = lodDist2() * lodScale * lodScale, d2 = d1 * (LOD_FAR * LOD_FAR), ez2 = camEye[2] * camEye[2], screenLod = [];
    for (let s = 0; s < C.NS; s++) { const b = screenBox[s];
      const nx = Math.max(b[0], Math.min(camEye[0], b[2])) - camEye[0], ny = Math.max(b[1], Math.min(camEye[1], b[3])) - camEye[1];
      const dd = nx * nx + ny * ny + ez2; screenLod[s] = dd <= d1 ? 0 : dd <= d2 ? 1 : 2; }
    const inView = (run) => { const sb = screenBox[run.screen]; return box[0] <= sb[2] && box[2] >= sb[0] && box[1] <= sb[3] && box[3] >= sb[1]; };
    // the STATIC TOWN, frustum-culled per screen (no rebuild). Asset toggle OFF -> the placeholder
    // cube. With art: LOD0 + LOD2 share the flat geometry pipeline; LOD1 is the textured imposter.
    if (drawTown) {
      gpass.setVertexBuffer(1, staticBuf);
      for (const run of staticRuns) {
        if (!inView(run)) continue;
        if (!useAssets) { const m = meshTable[C.CUBE]; gpass.draw(m.cnt, run.count, m.off, run.first); dStaticRuns++; continue; }
        const lvl = screenLod[run.screen]; if (lvl === 1) continue;          // imposters draw in the pass below
        const m = meshTable[lvl === 2 ? run.mesh + C.LOD2OFF : run.mesh]; gpass.draw(m.cnt, run.count, m.off, run.first); dStaticRuns++;
      }
      if (useAssets) {   // LOD1 IMPOSTERS: the textured pipeline + the projected-art atlas, same instances
        gpass.setPipeline(imposterPipe); gpass.setBindGroup(1, atlasBind); gpass.setVertexBuffer(0, imposterBuf);
        for (const run of staticRuns) { if (screenLod[run.screen] !== 1 || !inView(run)) continue;
          const m = imposterTable[run.mesh - C.MPROC]; gpass.draw(m.cnt, run.count, m.off, run.first); dStaticRuns++; }
        gpass.setPipeline(geometryPipe); gpass.setVertexBuffer(0, meshBuf);  // restore for the dynamic kinds
      }
    }
    // the static PROPS (trees): art-pass only, frustum-culled per screen, drawn with the M_TREE
    // procedural mesh at every distance (no LOD — a tree is a handful of triangles).
    if (useAssets && drawTrees) {
      gpass.setVertexBuffer(1, propBuf);
      for (const run of propRuns) { if (!inView(run)) continue; const m = meshTable[run.mesh]; gpass.draw(m.cnt, run.count, m.off, run.first); dTreeRuns++; dTrees += run.count; }
    }
    // the DYNAMIC opaque kinds from their own buffer: one draw per kind, that kind's mesh.
    gpass.setVertexBuffer(1, instBuf);
    let first = 0;
    for (let k = 0; k < C.KOC; k++) {
      const cnt = wasm.draw_opaque(k);
      if (cnt) { const m = meshTable[useAssets ? meshForKind[k] : C.CUBE]; gpass.draw(m.cnt, cnt, m.off, first); }
      first += cnt;
    }
    gpass.end();

    // 2) LIGHTING -> HDR. One screen pass reads the G-buffer and applies the environment
    // (hemisphere ambient + a directional sun); the background (mask 0) becomes the sky. This
    // is where the next per-mite / per-FX point lights will additively accumulate.
    const lpass = enc.beginRenderPass({
      colorAttachments: [{ view: hdrTex.createView(), clearValue: { r: 0, g: 0, b: 0, a: 1 }, loadOp: "clear", storeOp: "store" }],
      timestampWrites: tw("light") });
    lpass.setPipeline(lightingPipe); lpass.setBindGroup(0, lightBind); lpass.draw(3);
    lpass.end();

    // 2b) POINT LIGHTS -> HDR (additive). Each visible mite / FX is a small cube volume that
    // lights only the G-buffer pixels it covers, so the swarm's glow scales with lit pixels, not
    // with the mite count times the scene. One instanced draw over the whole light buffer.
    const runLights = nLights && drawLights;
    if (runLights) {
      const plpass = enc.beginRenderPass({
        colorAttachments: [{ view: hdrTex.createView(), loadOp: "load", storeOp: "store" }],
        timestampWrites: tw("plight") });
      plpass.setPipeline(lightVolPipe); plpass.setBindGroup(0, lightVolBind);
      plpass.setVertexBuffer(0, meshBuf); plpass.setVertexBuffer(1, lightBuf);
      const cube = meshTable[C.CUBE]; plpass.draw(cube.cnt, nLights, cube.off, 0);
      plpass.end();
    }

    // 3) FORWARD FX -> HDR, blended over the lit scene, depth-tested against the geometry.
    const tc = wasm.draw_translucent();
    if (tc) {
      const fpass = enc.beginRenderPass({
        colorAttachments: [{ view: hdrTex.createView(), loadOp: "load", storeOp: "store" }],
        depthStencilAttachment: { view: depthTex.createView(), depthLoadOp: "load", depthStoreOp: "store" },
        timestampWrites: tw("fx") });
      fpass.setBindGroup(0, bind); fpass.setVertexBuffer(0, meshBuf); fpass.setVertexBuffer(1, instBuf);
      fpass.setPipeline(transPipe); const m = meshTable[C.CUBE]; fpass.draw(m.cnt, tc, m.off, first);
      fpass.end();
    }

    // 4) TONEMAP -> the display. Roll the HDR down (exposure curve) into the swapchain.
    const tpass = enc.beginRenderPass({
      colorAttachments: [{ view: ctx.getCurrentTexture().createView(), clearValue: { r: 0, g: 0, b: 0, a: 1 }, loadOp: "clear", storeOp: "store" }],
      timestampWrites: tw("tone") });
    tpass.setPipeline(tonemapPipe); tpass.setBindGroup(0, tonemapBind); tpass.draw(3);
    tpass.end();

    // resolve the GPU timestamps into a mappable buffer to read back next frame (when supported)
    let tsRead = null;
    if (canTimestamp) {
      enc.resolveQuerySet(tsQuery, 0, tsCount, tsResolve, 0);
      tsRead = tsFree.pop() || device.createBuffer({ size: tsCount * 8, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      enc.copyBufferToBuffer(tsResolve, 0, tsRead, 0, tsCount * 8);
    }
    device.queue.submit([enc.finish()]);
    ema(prof, "enc", performance.now() - enc0);   // CPU: the command-encoder build
    ema(prof, "cpu", performance.now() - cpu0);    // CPU: prep + uploads + encode
    prof.sim = updMs; prof.dt = dt; prof.fps = fps;
    prof.n = { staticRuns: dStaticRuns, treeRuns: dTreeRuns, trees: dTrees, inst: n, lights: runLights ? nLights : 0 };
    prof.ran = { geom: 1, light: 1, tone: 1, plight: runLights ? 1 : 0, fx: tc ? 1 : 0 };

    // read back the per-pass GPU times (a frame or two behind; that's fine for a running average)
    if (tsRead) {
      const ran = { geom: 1, light: 1, tone: 1, plight: runLights ? 1 : 0, fx: tc ? 1 : 0 };
      tsRead.mapAsync(GPUMapMode.READ).then(() => {
        const t = new BigInt64Array(tsRead.getMappedRange());
        for (const p of GP) { if (!ran[p]) continue;
          const ns = Number(t[GP.indexOf(p) * 2 + 1] - t[GP.indexOf(p) * 2]);
          if (ns >= 0 && ns < 1e9) ema(prof.gpu, p, ns / 1e6); }
        tsRead.unmap(); tsFree.push(tsRead);
      }).catch(() => {});   // buffer can be discarded on teardown; ignore
    }

    const cam = camScreen(), camIdx = cam.sy * C.SX + cam.sx;
    articleMap.update(camIdx);
    if (pickerOpen) pickerMap.update(camIdx);
    document.getElementById("camlabel").textContent = pickerOpen ? "pick a screen" : `screen ${cam.sx},${cam.sy} ▾`;
    updateCamUI();
    dbg.update({ fps, dt, updMs, instCount: n, cam });
    if (profEl) {
      if (diag.active) profEl.textContent = `diagnostics… ${diag.ci + 1}/${DIAG_CONFIGS.length}  "${DIAG_CONFIGS[diag.ci].k}"  (${diag.phase === "warm" ? "settling" : diag.coll.length + "/" + DIAG_N})`;
      else if (!diag.done && (++profFrame % 8 === 0)) updateProfPanel();   // diag.done keeps the report on screen
    }
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

// a minimap of the 4x4 world, kept FLAT TOP-DOWN while the main view is perspective 3-D — the
// same sim data drawn two ways at once. Each screen is a tiny canvas showing the
// walls, the nests, the tanks, and the whole swarm as downsampled dots (hunting =
// red, homing = the nest's colour, wandering = teal); click a screen to view it.
function makeMiniMap(container, onPick, R) {
  const { wasm, view, C } = R;
  container.style.gridTemplateColumns = `repeat(${C.SX}, 1fr)`;   // the world's screen layout (8x8)
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
    // bolt speed shown in cells/tick (sim stores subcells/tick; 256 = 1 cell). Slow default ~2.25.
    const boltOf  = () => Math.round(wasm.proj_speed() / 64) / 4;
    const bolt    = numField("bolt speed (cells/tick)", 0.25, 6, 0.25, boltOf());
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
    bolt.input.onchange    = () => { const c = parseFloat(bolt.input.value) || 0; wasm.set_proj_speed(Math.max(16, Math.round(c * 256))); };
    tc.append(seed.wrap, sense.wrap, cap.wrap, phunt.wrap, speed.wrap, turn.wrap, fire.wrap, respawn.wrap, trate.wrap, bolt.wrap, size.wrap);

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
      sync(fire, rateOf); sync(respawn, respOf); sync(trate, () => wasm.turret_rate()); sync(bolt, boltOf); });
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
