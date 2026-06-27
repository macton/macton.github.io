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
import { inflateSync } from "node:zlib";
import { join } from "node:path";
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
  const verts = [];
  for (const tri of tris) {
    const P = tri.map(([vi]) => nm(E[vi]));
    const e1 = [P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]], e2 = [P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]];
    let n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]; const L = Math.hypot(...n) || 1; n = n.map((v) => v / L);
    const cz = (P[0][2] + P[1][2] + P[2][2]) / 3, isRoof = roof && n[2] > 0.35 && cz > -0.35;
    for (let i = 0; i < 3; i++) { const ti = tri[i][1]; const c = isRoof ? roof : (ti < 0 ? [200, 200, 200] : sample(img, uv[ti][0], uv[ti][1]));
      verts.push([qn(P[i][0]), qn(P[i][1]), qn(P[i][2]), 0, qn(n[0]), qn(n[1]), qn(n[2]), 0, c[0], c[1], c[2], 255]); }
  }
  return { name, hz: Math.round(hKen * SUB / 2), verts };
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

// LOD1 — a SIMPLIFIED, COLOUR-MATCHED massing mesh: a box body up to the eave (wall colour) +
// the MAIN roof shape (flat or gable, roof colour), or a single top quad for flat tiles. It is
// normalised identically to bake(), so one instance renders LOD0 or LOD1 at the same size. ~40
// verts vs ~3500, for distant buildings. `roofCol` (when set) matches LOD0's recoloured roof.
function bakeLOD1(kitDir, name, img, roofCol, group) {
  const { pos, uv, tris } = parseOBJ(readFileSync(join(kitDir, "Models", "OBJ format", name + ".obj"), "utf8"));
  const E = pos.map((p) => [p[0], -p[2], p[1]]);
  let lo = [9,9,9], hi = [-9,-9,-9];
  for (const p of E) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  const hKen = hi[2] - lo[2], hz = Math.round(hKen * SUB / 2);
  const verts = [];
  const qv = (px,py,pz, nx,ny,nz, c) => verts.push([qn(px),qn(py),qn(pz),0, qn(nx),qn(ny),qn(nz),0, c[0],c[1],c[2],255]);
  // emit a flat quad / tri with its OUTWARD normal (forced away from the building's central axis)
  function face(pts, col) {
    const a = pts[0], b = pts[1], c = pts[2];
    const e1 = [b[0]-a[0],b[1]-a[1],b[2]-a[2]], e2 = [c[0]-a[0],c[1]-a[1],c[2]-a[2]];
    let n = [e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]]; const L = Math.hypot(...n) || 1; n = n.map((v) => v/L);
    let cen = [0,0,0]; for (const p of pts) { cen[0]+=p[0]; cen[1]+=p[1]; cen[2]+=p[2]; } cen = cen.map((v) => v/pts.length);
    if (n[0]*cen[0] + n[1]*cen[1] + n[2]*cen[2] < 0) n = n.map((v) => -v);   // outward from origin (inside the box)
    for (let i = 1; i + 1 < pts.length; i++) { qv(...pts[0],...n,col); qv(...pts[i],...n,col); qv(...pts[i+1],...n,col); }
  }
  // flat tiles (roads/grass): a single top quad in the tile's average colour
  if (group === "ROAD" || group === "GRASS") {
    const top = avgFaceColour(E, tris, uv, img, lo, hKen, (n) => n[2] > 0.5);
    face([[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]], top);
    return { name, hz, verts };
  }
  // buildings/landmark: detect the eave height + roof shape, sample wall/roof colours
  let roofMinZ = hi[2], rx = [], ry = [];
  for (const tri of tris) { const P = tri.map(([vi]) => E[vi]);
    const e1 = [P[1][0]-P[0][0],P[1][1]-P[0][1],P[1][2]-P[0][2]], e2 = [P[2][0]-P[0][0],P[2][1]-P[0][1],P[2][2]-P[0][2]];
    let n = [e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]]; const L = Math.hypot(...n)||1; n = n.map((v)=>v/L);
    const czF = ((P[0][2]+P[1][2]+P[2][2])/3 - lo[2]) / hKen;
    if (n[2] > 0.35 && czF > 0.3) { for (const p of P) { roofMinZ = Math.min(roofMinZ, p[2]); if ((p[2]-lo[2])/hKen > 0.88) { rx.push(p[0]); ry.push(p[1]); } } } }
  const eaveFrac = Math.min(0.65, Math.max(0.30, (roofMinZ - lo[2]) / hKen)), ez = -1 + 2 * eaveFrac;
  const fw = hi[0]-lo[0], fd = hi[1]-lo[1];
  const rxF = rx.length ? (Math.max(...rx)-Math.min(...rx)) / fw : 1, ryF = ry.length ? (Math.max(...ry)-Math.min(...ry)) / fd : 1;
  const gable = Math.min(rxF, ryF) < 0.25, ridgeX = rxF >= ryF;   // one ridge axis ~a line => pitched; ridge along the wider one
  const wall = avgFaceColour(E, tris, uv, img, lo, hKen, (n) => Math.abs(n[2]) < 0.35);
  const roof = roofCol || avgFaceColour(E, tris, uv, img, lo, hKen, (n, czF) => n[2] > 0.35 && czF > 0.3);
  // box walls up to the eave
  face([[-1,-1,-1],[1,-1,-1],[1,-1,ez],[-1,-1,ez]], wall);
  face([[1,-1,-1],[1,1,-1],[1,1,ez],[1,-1,ez]], wall);
  face([[1,1,-1],[-1,1,-1],[-1,1,ez],[1,1,ez]], wall);
  face([[-1,1,-1],[-1,-1,-1],[-1,-1,ez],[-1,1,ez]], wall);
  if (!gable) {                                   // flat roof: a roof-coloured cap + top
    face([[-1,-1,ez],[1,-1,ez],[1,-1,1],[-1,-1,1]], roof);
    face([[1,-1,ez],[1,1,ez],[1,1,1],[1,-1,1]], roof);
    face([[1,1,ez],[-1,1,ez],[-1,1,1],[1,1,1]], roof);
    face([[-1,1,ez],[-1,-1,ez],[-1,-1,1],[-1,1,1]], roof);
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
  return { name, hz, verts };
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

// Bake each town mesh at TWO levels of detail: LOD0 (the full Kenney art) and LOD1 (a
// simplified, colour-matched box+roof). They go into one table — LOD0 in [0,N0), LOD1 in
// [N0,2*N0) — so the LOD1 of mesh m is m + MAP_LOD1_OFFSET. The static map references LOD0 ids;
// the host swaps to LOD1 for distant screens.
const meshes0 = [], meshes1 = []; const bases = {};
for (const [g, list, roofs] of GROUPS) { bases[g] = meshes0.length;
  list.forEach(([k, name], i) => { const roofCol = roofs ? ROOF[i % ROOF.length] : null;
    meshes0.push(bake(dir(k), name, imgs[k], roofCol));
    meshes1.push(bakeLOD1(dir(k), name, imgs[k], roofCol, g)); }); }
const meshes = meshes0.concat(meshes1);
const LOD1_OFFSET = meshes0.length;

let voff = [], vcnt = [], hz = [], all = [];
for (const m of meshes) { voff.push(all.length / 12); vcnt.push(m.verts.length); hz.push(m.hz); for (const v of m.verts) all.push(...v); }
const N = meshes.length, NV = all.length / 12;

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
c += `const uint32_t MAP_MESH_VERT_TOTAL = ${NV};\n`;
writeFileSync(join(OUT, "map_mesh_data.c"), c);

let h = `/* GENERATED by tools/bake_map_assets.mjs — the TOWN map mesh table + group indices.\n`;
h += ` * render.c addresses meshes as BASE + offset; the static-map bake places instances. */\n`;
h += `#ifndef TANK_MAP_MESH_DATA_H\n#define TANK_MAP_MESH_DATA_H\n#include <stdint.h>\n\n`;
h += `#define MAP_MESH_COUNT ${N}\n`;
h += `#define MAP_LOD1_OFFSET ${LOD1_OFFSET}   /* the LOD1 (simplified) of town mesh m is m + MAP_LOD1_OFFSET */\n`;
for (const [g, list] of GROUPS) { h += `#define MAP_${g}_BASE ${bases[g]}\n`; if (list.length > 1) h += `#define MAP_${g}_N ${list.length}\n`; }
h += `\nextern const int8_t   MAP_MESH_VERT[];\nextern const uint32_t MAP_MESH_VOFF[MAP_MESH_COUNT];\n`;
h += `extern const uint32_t MAP_MESH_VCNT[MAP_MESH_COUNT];\nextern const int16_t  MAP_MESH_HZ[MAP_MESH_COUNT];\n`;
h += `extern const uint8_t  MAP_ROAD_CANON[MAP_ROAD_N];  /* autotile: per-road canonical connection mask (N=1,E=2,S=4,W=8) */\n`;
h += `extern const uint32_t MAP_MESH_VERT_TOTAL;\n#endif\n`;
writeFileSync(join(OUT, "map_mesh_data.h"), h);

console.log(`baked ${N} town meshes, ${NV} verts (${(NV*12/1024).toFixed(1)} KB) -> ${OUT}/map_mesh_data.{c,h}`);
console.log(`groups: ${GROUPS.map(([g]) => `${g}@${bases[g]}`).join("  ")}`);
console.log(`road canon (measured from art): ${ROAD_CANON.join(", ")}  (square,end,straight,bend,intersection,crossroad)`);
