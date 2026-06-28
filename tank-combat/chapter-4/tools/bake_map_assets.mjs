// bake_map_assets.mjs — OFFLINE builder (run by hand against locally-downloaded Kenney
// CC0 kits: City-Kit Suburban/Commercial/Roads + Tower-Defense for the grass tile).
// Bakes the TOWN map mesh set — autotile road tiles, roof-recoloured buildings, a grass
// tile, a landmark — into the engine's compact per-vertex-coloured vertex format, and
// emits committed src/map_mesh_data.c + src/map_mesh_data.h. Source art is NOT committed.
//
// Each mesh is normalised PER-AXIS into the unit box [-1,1] (footprint x/y, height z with
// base at z=-1) and carries a height half-extent (subcells) so the static-map instance can
// restore real proportions. Nothing here runs at game time; render.c reads the committed
// tables, the static-map bake places instances, the page uploads once.
import { readFileSync, writeFileSync } from "node:fs";
import { inflateSync, deflateSync } from "node:zlib";
import { join } from "node:path";

// minimal PNG writer (RGBA8) — the imposter atlas is a committed deliverable, like game.wasm.
function encodePNG(w, h, rgba) {
  const crcTab = (() => { const t = new Int32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c; } return t; })();
  const crc = (buf) => { let c = ~0; for (let i = 0; i < buf.length; i++) c = crcTab[(c ^ buf[i]) & 255] ^ (c >>> 8); return ~c >>> 0; };
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;   // 8-bit RGBA
  const raw = Buffer.alloc(h * (w * 4 + 1)); for (let y = 0; y < h; y++) { raw[y * (w * 4 + 1)] = 0; rgba.copy ? rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, y * w * 4 + w * 4) : Buffer.from(rgba.buffer, y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1); }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const ASSETS = process.argv[2] || "assets", OUT = process.argv[3] || "src";
const SUB = 256;

function decodePNG(buf) {
  let p = 8, w = 0, h = 0, bd = 0, ct = 0; const idat = []; let plte = null;
  while (p < buf.length) { const len = buf.readUInt32BE(p), t = buf.toString("ascii", p + 4, p + 8), d = p + 8;
    if (t === "IHDR") { w = buf.readUInt32BE(d); h = buf.readUInt32BE(d + 4); bd = buf[d + 8]; ct = buf[d + 9]; }
    else if (t === "PLTE") { plte = []; for (let i = 0; i < len; i += 3) plte.push([buf[d + i], buf[d + i + 1], buf[d + i + 2]]); }
    else if (t === "IDAT") idat.push(buf.subarray(d, d + len)); else if (t === "IEND") break; p = d + len + 4; }
  if (bd !== 8) throw new Error("8-bit PNG only");
  const ch = ct === 6 ? 4 : ct === 2 ? 3 : 1, raw = inflateSync(Buffer.concat(idat)), stride = w * ch, out = Buffer.alloc(h * stride);
  const pae = (a, b, c) => { const pp = a + b - c, da = Math.abs(pp - a), db = Math.abs(pp - b), dc = Math.abs(pp - c); return da <= db && da <= dc ? a : db <= dc ? b : c; };
  for (let y = 0; y < h; y++) { const f = raw[y * (stride + 1)], rs = y * (stride + 1) + 1, os = y * stride;
    for (let x = 0; x < stride; x++) { const cur = raw[rs + x], a = x >= ch ? out[os + x - ch] : 0, b = y > 0 ? out[os - stride + x] : 0, c = x >= ch && y > 0 ? out[os - stride + x - ch] : 0;
      let v = cur; if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1; else if (f === 4) v += pae(a, b, c); out[os + x] = v & 255; } }
  return { w, h, ch, data: out, plte };
}
const sample = (img, u, v) => { const x = Math.min(img.w - 1, Math.max(0, Math.round(u * (img.w - 1)))), y = Math.min(img.h - 1, Math.max(0, Math.round((1 - v) * (img.h - 1)))), o = (y * img.w + x) * img.ch; return img.plte ? img.plte[img.data[o]] : [img.data[o], img.data[o + 1], img.data[o + 2]]; };
function parseOBJ(text) { const pos = [], uv = [], tris = [], seen = new Set();
  for (const line of text.split("\n")) { const a = line.trim().split(/\s+/);
    if (a[0] === "v") pos.push([+a[1], +a[2], +a[3]]); else if (a[0] === "vt") uv.push([+a[1], +a[2]]);
    else if (a[0] === "f") { const vs = a.slice(1).map((s) => { const [vi, ti] = s.split("/"); return [+vi - 1, ti ? +ti - 1 : -1]; });
      for (let i = 1; i + 1 < vs.length; i++) { const k = [vs[0][0], vs[i][0], vs[i + 1][0]].join(","); if (seen.has(k)) continue; seen.add(k); tris.push([vs[0], vs[i], vs[i + 1]]); } } }
  return { pos, uv, tris }; }
const qn = (f) => { let s = Math.round(f * 127); return s > 127 ? 127 : s < -127 ? -127 : s; };

// Kenney Y-up (footprint x/z, height +y) -> engine Z-up (footprint x/y, height +z): (x,-z,y).
// Per-axis normalise the bbox into [-1,1] (footprint centred, base at z=-1). `roof` (or
// null): recolour upward faces in the upper half (the kit's green roofs blend into grass).
function bake(kitDir, name, img, roof) {
  const { pos, uv, tris } = parseOBJ(readFileSync(join(kitDir, "Models", "OBJ format", name + ".obj"), "utf8"));
  const E = pos.map((p) => [p[0], -p[2], p[1]]);
  let lo = [9, 9, 9], hi = [-9, -9, -9];
  for (const p of E) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  const cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, sx = 2 / Math.max(1e-3, hi[0] - lo[0]), sy = 2 / Math.max(1e-3, hi[1] - lo[1]), hKen = hi[2] - lo[2], sz = 2 / Math.max(1e-3, hKen);
  const nm = (p) => [(p[0] - cx) * sx, (p[1] - cy) * sy, (p[2] - lo[2]) * sz - 1];
  const verts = [], tris3 = [];   // tris3: the float LOD0 triangles (pos + colour), for projecting the imposter
  for (const tri of tris) {
    const P = tri.map(([vi]) => nm(E[vi]));
    const e1 = [P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]], e2 = [P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]];
    let n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]; const L = Math.hypot(...n) || 1; n = n.map((v) => v / L);
    const cz = (P[0][2] + P[1][2] + P[2][2]) / 3, isRoof = roof && n[2] > 0.35 && cz > -0.35;
    const C = [];
    for (let i = 0; i < 3; i++) { const ti = tri[i][1]; const c = isRoof ? roof : (ti < 0 ? [200, 200, 200] : sample(img, uv[ti][0], uv[ti][1])); C.push(c);
      verts.push([qn(P[i][0]), qn(P[i][1]), qn(P[i][2]), 0, qn(n[0]), qn(n[1]), qn(n[2]), 0, c[0], c[1], c[2], 255]); }
    tris3.push({ P, C });
  }
  return { name, hz: Math.round(hKen * SUB / 2), verts, tris3 };
}

// mean sampled colour over the faces a predicate accepts (n = face normal, czF = face-centre
// height as a fraction of the model height). Used to colour-match the LOD silhouette.
function avgFaceColour(E, tris, uv, img, lo, H, pick) {
  let r = 0, g = 0, b = 0, k = 0;
  for (const tri of tris) {
    const P = tri.map(([vi]) => E[vi]);
    const e1 = [P[1][0]-P[0][0], P[1][1]-P[0][1], P[1][2]-P[0][2]], e2 = [P[2][0]-P[0][0], P[2][1]-P[0][1], P[2][2]-P[0][2]];
    let n = [e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]]; const L = Math.hypot(...n) || 1; n = n.map((v) => v/L);
    const czF = ((P[0][2]+P[1][2]+P[2][2]) / 3 - lo[2]) / Math.max(1e-3, H);
    if (!pick(n, czF)) continue;
    for (let i = 0; i < 3; i++) { const ti = tri[i][1]; const c = ti < 0 ? [180,180,180] : sample(img, uv[ti][0], uv[ti][1]); r += c[0]; g += c[1]; b += c[2]; k++; }
  }
  return k ? [Math.round(r/k), Math.round(g/k), Math.round(b/k)] : [170,170,170];
}

// The CAGE — the simplified box+roof silhouette shared by two LODs: a box body up to the eave
// + the MAIN roof shape (flat or gable), or one top quad for flat tiles. Returns the cage as a
// list of {pts, col} polygons (outward normal computed when emitted). The MASSING (LOD2) emits
// it vertex-coloured; the IMPOSTER (LOD1) emits it with the LOD0 art projected onto its faces.
// Normalised identically to bake(), so one instance renders any LOD at the same size.
function cage(kitDir, name, img, roofCol, group) {
  const { pos, uv, tris } = parseOBJ(readFileSync(join(kitDir, "Models", "OBJ format", name + ".obj"), "utf8"));
  const E = pos.map((p) => [p[0], -p[2], p[1]]);
  let lo = [9,9,9], hi = [-9,-9,-9];
  for (const p of E) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  const hKen = hi[2] - lo[2], hz = Math.round(hKen * SUB / 2);
  const faces = [];
  const face = (pts, col) => faces.push({ pts, col });   // a cage polygon; its outward normal is found at emit
  // flat tiles (roads/grass): a single top quad in the tile's average colour
  if (group === "ROAD" || group === "GRASS") {
    const top = avgFaceColour(E, tris, uv, img, lo, hKen, (n) => n[2] > 0.5);
    face([[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]], top);
    return { name, hz, faces, flat: true };
  }
  // buildings/landmark: classify the roof by the AREA-WEIGHTED slope of the upper upward faces.
  // Rooftop clutter (AC units, parapets, penthouses) is small in area, so it no longer fakes a
  // ridge the way the old top-12%-vertex span did — that false-gabled flat-roofed buildings.
  // roofNz ~1 = flat; lower = a real pitch; the ridge runs ACROSS the dominant slope direction.
  let aSum = 0, nzA = 0, nxA = 0, nyA = 0, roofMinZ = hi[2];
  for (const tri of tris) { const P = tri.map(([vi]) => E[vi]);
    const e1 = [P[1][0]-P[0][0],P[1][1]-P[0][1],P[1][2]-P[0][2]], e2 = [P[2][0]-P[0][0],P[2][1]-P[0][1],P[2][2]-P[0][2]];
    const cr = [e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]]; const aA = 0.5*Math.hypot(...cr), L = 2*aA||1, n = cr.map((v)=>v/L);
    const czF = ((P[0][2]+P[1][2]+P[2][2])/3 - lo[2]) / hKen;
    if (n[2] > 0.15 && czF > 0.45) { aSum += aA; nzA += n[2]*aA; nxA += Math.abs(n[0])*aA; nyA += Math.abs(n[1])*aA; for (const p of P) roofMinZ = Math.min(roofMinZ, p[2]); } }
  const roofNz = aSum ? nzA / aSum : 1;
  const gable = roofNz < 0.90, ridgeX = nyA >= nxA;   // sloped enough to read as pitched; slopes face +-Y => ridge along X
  const eaveFrac = gable ? Math.min(0.62, Math.max(0.32, (roofMinZ - lo[2]) / hKen)) : 0.82;   // flat: a thin top cap
  const ez = -1 + 2 * eaveFrac;
  if (process.argv.includes("debugroofs")) console.error(`  ${name.padEnd(22)} gable=${gable} roofNz=${roofNz.toFixed(3)} ridgeX=${ridgeX} eaveFrac=${eaveFrac.toFixed(2)}`);
  const wall = avgFaceColour(E, tris, uv, img, lo, hKen, (n) => Math.abs(n[2]) < 0.35);
  const roof = roofCol || avgFaceColour(E, tris, uv, img, lo, hKen, (n, czF) => n[2] > 0.35 && czF > 0.3);
  // box walls: FULL height for a flat roof (the top is just a cap), up to the eave for a gable
  const wt = gable ? ez : 1;
  face([[-1,-1,-1],[1,-1,-1],[1,-1,wt],[-1,-1,wt]], wall);
  face([[1,-1,-1],[1,1,-1],[1,1,wt],[1,-1,wt]], wall);
  face([[1,1,-1],[-1,1,-1],[-1,1,wt],[1,1,wt]], wall);
  face([[-1,1,-1],[-1,-1,-1],[-1,-1,wt],[-1,1,wt]], wall);
  if (!gable) {                                   // flat roof: just the top quad (roof colour)
    face([[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]], roof);
  } else if (ridgeX) {                            // gable, ridge along X (y=0, z=+1)
    face([[-1,1,ez],[1,1,ez],[1,0,1],[-1,0,1]], roof);
    face([[1,-1,ez],[-1,-1,ez],[-1,0,1],[1,0,1]], roof);
    face([[1,-1,ez],[1,1,ez],[1,0,1]], wall);
    face([[-1,1,ez],[-1,-1,ez],[-1,0,1]], wall);
  } else {                                        // gable, ridge along Y (x=0, z=+1)
    face([[1,-1,ez],[1,1,ez],[0,1,1],[0,-1,1]], roof);
    face([[-1,1,ez],[-1,-1,ez],[0,-1,1],[0,1,1]], roof);
    face([[-1,1,ez],[1,1,ez],[0,1,1]], wall);
    face([[1,-1,ez],[-1,-1,ez],[0,-1,1]], wall);
  }
  return { name, hz, faces, flat: false };
}

// outward normal of a cage polygon (flipped to point away from the box centre, like the old face())
function outwardN(pts) {
  const a = pts[0], b = pts[1], c = pts[2];
  const e1 = [b[0]-a[0],b[1]-a[1],b[2]-a[2]], e2 = [c[0]-a[0],c[1]-a[1],c[2]-a[2]];
  let n = [e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]]; const L = Math.hypot(...n) || 1; n = n.map((v) => v/L);
  let cen = [0,0,0]; for (const p of pts) { cen[0]+=p[0]; cen[1]+=p[1]; cen[2]+=p[2]; } cen = cen.map((v) => v/pts.length);
  if (n[0]*cen[0] + n[1]*cen[1] + n[2]*cen[2] < 0) n = n.map((v) => -v);
  return n;
}
// LOD2 — the colour-matched MASSING: the cage, vertex-coloured (no texture). ~40 verts. (Was LOD1.)
function bakeMassing(cg) {
  const verts = [];
  for (const f of cg.faces) { const n = outwardN(f.pts);
    for (let i = 1; i + 1 < f.pts.length; i++) for (const p of [f.pts[0], f.pts[i], f.pts[i+1]])
      verts.push([qn(p[0]),qn(p[1]),qn(p[2]),0, qn(n[0]),qn(n[1]),qn(n[2]),0, f.col[0],f.col[1],f.col[2],255]); }
  return { name: cg.name, hz: cg.hz, verts };
}

// ---- IMPOSTER (LOD1): the cage, with the full LOD0 art PROJECTED onto its faces ----------------
// A "low-resolution geometry imposter": the same cheap cage as the massing, but instead of a flat
// colour each face samples a TEXTURE baked by rendering the original LOD0 model orthographically
// from that face's direction (top + 4 sides). One CPU rasteriser bakes the projections into a
// shared atlas; the imposter's UVs index it. From a distance it reads like the full art for ~40
// verts. (The 3ds-Max "render-to-texture lowest-LOD imposter" workflow, done offline in JS.)
const TILE = 64, GUTTER = 2, STRIDE = TILE + 2 * GUTTER;
// each direction: how strongly a normal faces it (pick the face's tile), and the SAME ortho
// projection used to BOTH rasterise the tile and assign the imposter UVs (so they're pixel-exact).
const DIRS = [
  { k: "top", face: (n) => n[2],  proj: (p) => [(p[0]+1)/2, (p[1]+1)/2], depth: (p) => p[2]  },
  { k: "xp",  face: (n) => n[0],  proj: (p) => [(p[1]+1)/2, (p[2]+1)/2], depth: (p) => p[0]  },
  { k: "xn",  face: (n) => -n[0], proj: (p) => [(p[1]+1)/2, (p[2]+1)/2], depth: (p) => -p[0] },
  { k: "yp",  face: (n) => n[1],  proj: (p) => [(p[0]+1)/2, (p[2]+1)/2], depth: (p) => p[1]  },
  { k: "yn",  face: (n) => -n[1], proj: (p) => [(p[0]+1)/2, (p[2]+1)/2], depth: (p) => -p[1] },
];
// rasterise the LOD0 float triangles from one direction into a TILE×TILE RGBA tile (z-buffered,
// nearest-to-camera wins; barycentric vertex-colour fill; alpha 0 where empty).
function rasterTile(tris3, dir) {
  const col = new Uint8Array(TILE * TILE * 4), zb = new Float32Array(TILE * TILE).fill(-1e9);
  const ed = (a, b, px, py) => (b[0]-a[0])*(py-a[1]) - (b[1]-a[1])*(px-a[0]);
  for (const { P, C } of tris3) {
    const s = P.map((p) => { const uv = dir.proj(p); return [uv[0]*(TILE-1), uv[1]*(TILE-1)]; }), dz = P.map(dir.depth);
    let area = ed(s[0], s[1], s[2][0], s[2][1]); if (Math.abs(area) < 1e-6) continue; const sgn = area < 0 ? -1 : 1;
    const minx = Math.max(0, Math.floor(Math.min(s[0][0],s[1][0],s[2][0]))), maxx = Math.min(TILE-1, Math.ceil(Math.max(s[0][0],s[1][0],s[2][0])));
    const miny = Math.max(0, Math.floor(Math.min(s[0][1],s[1][1],s[2][1]))), maxy = Math.min(TILE-1, Math.ceil(Math.max(s[0][1],s[1][1],s[2][1])));
    for (let y = miny; y <= maxy; y++) for (let x = minx; x <= maxx; x++) {
      const w0 = ed(s[1],s[2],x+0.5,y+0.5)*sgn, w1 = ed(s[2],s[0],x+0.5,y+0.5)*sgn, w2 = ed(s[0],s[1],x+0.5,y+0.5)*sgn;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      const ia = 1/(Math.abs(area)), b0 = w0*ia, b1 = w1*ia, b2 = w2*ia, z = b0*dz[0]+b1*dz[1]+b2*dz[2], idx = y*TILE+x;
      if (z <= zb[idx]) continue; zb[idx] = z; const o = idx*4;
      col[o]   = Math.round(b0*C[0][0]+b1*C[1][0]+b2*C[2][0]);
      col[o+1] = Math.round(b0*C[0][1]+b1*C[1][1]+b2*C[2][1]);
      col[o+2] = Math.round(b0*C[0][2]+b1*C[1][2]+b2*C[2][2]); col[o+3] = 255;
    }
  }
  return col;
}
// the imposter atlas: one ROW of 5 tiles (top,xp,xn,yp,yn) per textured building.
const I_TILES = [];   // { row, col, rgba }
let ATLAS_W = 0, ATLAS_H = 0;       // set once the textured-building count is known
const i8 = (x) => (x & 255) > 127 ? (x & 255) - 256 : (x & 255);   // low/high byte of an int16 as signed int8
// emit the cage as the imposter. Building faces sample their projection tile (UV into the atlas);
// flat tiles + any uncovered gap fall back to the massing colour (UV sentinel -1, or atlas alpha 0).
// 16-byte verts: 12 (pos/normal/colour, int8) + a UV of two int16 split into 4 int8 bytes.
function bakeImposter(cg, tris3, row) {
  if (!cg.flat) DIRS.forEach((d, c) => I_TILES.push({ row, col: c, rgba: rasterTile(tris3, d) }));
  const verts = [];
  const emit = (pts, col, dir, dcol) => {
    const n = outwardN(pts);
    const uvOf = (p) => {
      if (!dir) return [-1, -1];                       // sentinel -> flat (vertex colour)
      const lu = dir.proj(p);                          // [0,1] within the tile (same proj that rasterised it)
      const px = dcol * STRIDE + GUTTER + 0.5 + lu[0] * (TILE - 2);   // inset 0.5 texel so bilinear never reaches the gutter
      const py = row  * STRIDE + GUTTER + 0.5 + lu[1] * (TILE - 2);
      return [Math.round(px / ATLAS_W * 32767), Math.round(py / ATLAS_H * 32767)];
    };
    for (let i = 1; i + 1 < pts.length; i++) for (const p of [pts[0], pts[i], pts[i+1]]) {
      const [u, v] = uvOf(p);
      verts.push([qn(p[0]),qn(p[1]),qn(p[2]),0, qn(n[0]),qn(n[1]),qn(n[2]),0, col[0],col[1],col[2],255,
                  i8(u & 255), i8((u >> 8) & 255), i8(v & 255), i8((v >> 8) & 255)]);
    }
  };
  for (const f of cg.faces) {
    if (cg.flat) { emit(f.pts, f.col, null, 0); continue; }
    const n = outwardN(f.pts); let best = 0, bv = -2;
    for (let d = 0; d < DIRS.length; d++) { const val = DIRS[d].face(n); if (val > bv) { bv = val; best = d; } }
    emit(f.pts, f.col, DIRS[best], best);   // f.col (massing colour) = the fallback where the projection has gaps
  }
  return { name: cg.name, hz: cg.hz, verts };
}

const KITS = { sub: "city-kit-suburban", com: "city-kit-commercial", road: "city-kit-roads", td: "kenney_tower-defense-kit" };
const dir = (k) => join(ASSETS, KITS[k]);
const imgs = {}; for (const k of Object.keys(KITS)) imgs[k] = decodePNG(readFileSync(join(dir(k), "Models", "OBJ format", "Textures", "colormap.png")));
// roof palette — varied + contrasting (the kit roofs are green, like the grass)
const ROOF = [[196,84,58],[96,116,156],[162,66,52],[74,78,92],[200,120,52],[150,140,128],[120,84,120],[70,100,120]];
// GROUPS, emitted in this order so render.c can address them by base+offset. The road
// group is in autotile order: square,end,straight,bend,intersection,crossroad.
const GROUPS = [
  ["ROAD", [["road","road-square"],["road","road-end"],["road","road-straight"],["road","road-bend"],["road","road-intersection"],["road","road-crossroad"]], false],
  ["BUILD", [["sub","building-type-a"],["sub","building-type-c"],["sub","building-type-e"],["sub","building-type-h"],["sub","building-type-k"],["sub","building-type-q"],["com","building-a"],["com","building-c"]], true],
  ["GRASS", [["td","tile"]], false],
  ["LANDMARK", [["com","building-skyscraper-a"]], false],
];

// Bake each town mesh at THREE levels of detail:
//   LOD0 — the full Kenney art (~3500 verts)
//   LOD1 — a textured IMPOSTER: the cheap cage with the LOD0 art PROJECTED onto its faces (~40
//          verts + a shared atlas), so it reads like the full art from a distance
//   LOD2 — the colour-matched MASSING: the same cage, flat vertex colour (~40 verts)
// The 12-byte table holds LOD0 in [0,N0) and LOD2 in [N0,2*N0) (LOD2 of mesh m = m + MAP_LOD2_OFFSET).
// The imposters are a SEPARATE 16-byte table (they carry UVs) indexed by base m. The static map
// references LOD0 ids; the host swaps to the imposter / massing per screen, by distance.
const meshes0 = [], cages = []; const bases = {};
for (const [g, list, roofs] of GROUPS) { bases[g] = meshes0.length;
  list.forEach(([k, name], i) => { const roofCol = roofs ? ROOF[i % ROOF.length] : null;
    const m0 = bake(dir(k), name, imgs[k], roofCol);
    meshes0.push(m0);
    cages.push({ cg: cage(dir(k), name, imgs[k], roofCol, g), tris3: m0.tris3 }); }); }
// atlas size is known once we count the textured (non-flat) buildings — one tile row each
const nTex = cages.filter((c) => !c.cg.flat).length;
ATLAS_W = 5 * STRIDE; ATLAS_H = Math.max(1, nTex) * STRIDE;
const massings = [], imposters = []; let arow = 0;
for (const { cg, tris3 } of cages) { massings.push(bakeMassing(cg)); imposters.push(bakeImposter(cg, tris3, cg.flat ? -1 : arow)); if (!cg.flat) arow++; }
const meshes = meshes0.concat(massings);            // 12-byte table: LOD0 ++ LOD2 massing
const LOD2_OFFSET = meshes0.length;
// compose the atlas image from the rasterised projection tiles (gutters stay transparent)
const atlas = new Uint8Array(ATLAS_W * ATLAS_H * 4);
for (const t of I_TILES) for (let y = 0; y < TILE; y++) for (let x = 0; x < TILE; x++) {
  const o = ((t.row * STRIDE + GUTTER + y) * ATLAS_W + (t.col * STRIDE + GUTTER + x)) * 4, s = (y * TILE + x) * 4;
  atlas[o] = t.rgba[s]; atlas[o+1] = t.rgba[s+1]; atlas[o+2] = t.rgba[s+2]; atlas[o+3] = t.rgba[s+3];
}

let voff = [], vcnt = [], hz = [], all = [];
for (const m of meshes) { voff.push(all.length / 12); vcnt.push(m.verts.length); hz.push(m.hz); for (const v of m.verts) all.push(...v); }
const N = meshes.length, NV = all.length / 12;
let ivoff = [], ivcnt = [], iall = [];               // the 16-byte imposter table
for (const m of imposters) { ivoff.push(iall.length / 16); ivcnt.push(m.verts.length); for (const v of m.verts) iall.push(...v); }
const NI = imposters.length, NIV = iall.length / 16;

// Derive each ROAD mesh's CANONICAL connection mask from the ART, so the autotile can't
// drift from the geometry (hand-guessing it is exactly how the streets came out 180-degrees
// wrong). An edge is OPEN where the tile's top surface there is ASPHALT (the darker road),
// CLOSED where it is the lighter sidewalk/curb. The split threshold is the midpoint between
// the darkest and brightest EDGE means across all road tiles (asphalt vs curb extremes), so
// it adapts to the kit's palette. Bits match the autotile's neighbour mask: N(-y)=1, E(+x)=2,
// S(+y)=4, W(-x)=8 (positions are snorm int8, so the ±1 edges are ±127, sampled past ±90).
function edgeBright(m) {
  const e = { N: [], E: [], S: [], W: [] };
  for (const v of m.verts) { const px = v[0], py = v[1], nz = v[6], br = (v[8] + v[9] + v[10]) / 3;
    if (nz < 90) continue;                       // only upward (top) faces carry the surface colour
    if (py < -90) e.N.push(br); if (py > 90) e.S.push(br); if (px > 90) e.E.push(br); if (px < -90) e.W.push(br); }
  const mean = (a) => a.length ? a.reduce((s, x) => s + x, 0) / a.length : 255;   // no faces at an edge => closed
  return { N: mean(e.N), E: mean(e.E), S: mean(e.S), W: mean(e.W) };
}
const road = meshes.slice(bases.ROAD, bases.ROAD + GROUPS[0][1].length);   // [square,end,straight,bend,intersection,crossroad]
const edges = road.flatMap((m) => { const e = edgeBright(m); return [e.N, e.E, e.S, e.W]; });
const thr = (Math.min(...edges) + Math.max(...edges)) / 2;                 // between the asphalt and curb extremes
const ROAD_CANON = road.map((m) => { const e = edgeBright(m);
  return (e.N < thr ? 1 : 0) | (e.E < thr ? 2 : 0) | (e.S < thr ? 4 : 0) | (e.W < thr ? 8 : 0); });

let c = `/* GENERATED by tools/bake_map_assets.mjs from Kenney CC0 kits (City-Kit + Tower-Defense).\n`;
c += ` * The TOWN map meshes: autotile roads, roof-recoloured buildings, grass, a landmark.\n`;
c += ` * Per-axis normalised into the unit box; MAP_MESH_HZ is the height half-extent (subcells). */\n`;
c += `#include "map_mesh_data.h"\n\n`;
c += `const int8_t MAP_MESH_VERT[] = {\n`;
for (const m of meshes) for (const v of m.verts) c += `  ${v.slice(0,8).join(",")}, ${v[8]},${v[9]},${v[10]},${v[11]},\n`;
c += `};\n`;
c += `const uint32_t MAP_MESH_VOFF[MAP_MESH_COUNT] = { ${voff.join(", ")} };\n`;
c += `const uint32_t MAP_MESH_VCNT[MAP_MESH_COUNT] = { ${vcnt.join(", ")} };\n`;
c += `const int16_t  MAP_MESH_HZ[MAP_MESH_COUNT]   = { ${hz.join(", ")} };\n`;
c += `const uint8_t  MAP_ROAD_CANON[MAP_ROAD_N]    = { ${ROAD_CANON.join(", ")} };  /* per-road connection mask, measured from the art */\n`;
c += `const uint32_t MAP_MESH_VERT_TOTAL = ${NV};\n\n`;
c += `/* the LOD1 IMPOSTERS: 16-byte verts (pos+normal+colour int8, then a UV of two int16) — UV\n`;
c += ` * indexes town_atlas.png (the LOD0 art projected from top + 4 sides); UV -1 => flat. */\n`;
c += `const int8_t MAP_IMPOSTER_VERT[] = {\n`;
for (const m of imposters) for (const v of m.verts) c += `  ${v.join(",")},\n`;
c += `};\n`;
c += `const uint32_t MAP_IMPOSTER_VOFF[MAP_IMPOSTER_COUNT] = { ${ivoff.join(", ")} };\n`;
c += `const uint32_t MAP_IMPOSTER_VCNT[MAP_IMPOSTER_COUNT] = { ${ivcnt.join(", ")} };\n`;
c += `const uint32_t MAP_IMPOSTER_VERT_TOTAL = ${NIV};\n`;
writeFileSync(join(OUT, "map_mesh_data.c"), c);

let h = `/* GENERATED by tools/bake_map_assets.mjs — the TOWN map mesh table + group indices.\n`;
h += ` * render.c addresses meshes as BASE + offset; the static-map bake places instances. */\n`;
h += `#ifndef TANK_MAP_MESH_DATA_H\n#define TANK_MAP_MESH_DATA_H\n#include <stdint.h>\n\n`;
h += `#define MAP_MESH_COUNT ${N}\n`;
h += `#define MAP_LOD2_OFFSET ${LOD2_OFFSET}   /* the LOD2 (flat massing) of town mesh m is m + MAP_LOD2_OFFSET */\n`;
h += `#define MAP_IMPOSTER_COUNT ${NI}   /* the LOD1 textured imposters — a SEPARATE 16-byte table, indexed by base m */\n`;
h += `#define MAP_ATLAS_W ${ATLAS_W}\n#define MAP_ATLAS_H ${ATLAS_H}\n`;
for (const [g, list] of GROUPS) { h += `#define MAP_${g}_BASE ${bases[g]}\n`; if (list.length > 1) h += `#define MAP_${g}_N ${list.length}\n`; }
h += `\nextern const int8_t   MAP_MESH_VERT[];\nextern const uint32_t MAP_MESH_VOFF[MAP_MESH_COUNT];\n`;
h += `extern const uint32_t MAP_MESH_VCNT[MAP_MESH_COUNT];\nextern const int16_t  MAP_MESH_HZ[MAP_MESH_COUNT];\n`;
h += `extern const uint8_t  MAP_ROAD_CANON[MAP_ROAD_N];  /* autotile: per-road canonical connection mask (N=1,E=2,S=4,W=8) */\n`;
h += `extern const uint32_t MAP_MESH_VERT_TOTAL;\n`;
h += `extern const int8_t   MAP_IMPOSTER_VERT[];\nextern const uint32_t MAP_IMPOSTER_VOFF[MAP_IMPOSTER_COUNT];\n`;
h += `extern const uint32_t MAP_IMPOSTER_VCNT[MAP_IMPOSTER_COUNT];\nextern const uint32_t MAP_IMPOSTER_VERT_TOTAL;\n#endif\n`;
writeFileSync(join(OUT, "map_mesh_data.h"), h);

writeFileSync(join(OUT, "..", "town_atlas.png"), encodePNG(ATLAS_W, ATLAS_H, Buffer.from(atlas)));

console.log(`baked ${N} town meshes (${NV} verts, ${(NV*12/1024).toFixed(1)} KB) + ${NI} imposters (${NIV} verts) -> ${OUT}/map_mesh_data.{c,h}`);
console.log(`atlas: ${ATLAS_W}x${ATLAS_H} (${nTex} buildings x 5 tiles) -> town_atlas.png`);
console.log(`groups: ${GROUPS.map(([g]) => `${g}@${bases[g]}`).join("  ")}`);
console.log(`road canon (measured from art): ${ROAD_CANON.join(", ")}  (square,end,straight,bend,intersection,crossroad)`);
