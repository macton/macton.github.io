// bake_map_assets.mjs — OFFLINE builder (run by hand against a locally-downloaded
// Kenney Tower Defense Kit, CC0). Parses the chosen OBJ models, samples the shared
// colormap palette per face, normalises geometry into the engine's unit box, and bakes:
//   (1) compact per-vertex-COLOURED meshes  -> src/map_mesh_data.c   (committed, tiny)
//   (2) the STATIC map instance buffer, screen-bucketed for frustum culling
//                                            -> src/map_static.bin    (committed)
// The downloaded source art is NOT committed (multi-MB); only these baked outputs are.
// Nothing here runs at game time — the runtime fetches the baked buffers and uploads them.
import { readFileSync, writeFileSync } from "node:fs";
import { inflateSync } from "node:zlib";
import { join } from "node:path";

// ---- minimal PNG decode (truecolour/truecolour+alpha, 8-bit; enough for Kenney's palette)
function decodePNG(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error("not a PNG");
  let p = 8, w = 0, h = 0, bitDepth = 0, colorType = 0; const idat = []; let plte = null;
  while (p < buf.length) {
    const len = buf.readUInt32BE(p), type = buf.toString("ascii", p + 4, p + 8); const d = p + 8;
    if (type === "IHDR") { w = buf.readUInt32BE(d); h = buf.readUInt32BE(d + 4); bitDepth = buf[d + 8]; colorType = buf[d + 9]; }
    else if (type === "PLTE") { plte = []; for (let i = 0; i < len; i += 3) plte.push([buf[d + i], buf[d + i + 1], buf[d + i + 2]]); }
    else if (type === "IDAT") idat.push(buf.subarray(d, d + len));
    else if (type === "IEND") break;
    p = d + len + 4;
  }
  if (bitDepth !== 8) throw new Error("only 8-bit PNG supported, got " + bitDepth);
  const ch = colorType === 6 ? 4 : colorType === 2 ? 3 : colorType === 3 ? 1 : (() => { throw new Error("colorType " + colorType); })();
  const raw = inflateSync(Buffer.concat(idat));
  const stride = w * ch, out = Buffer.alloc(h * stride);
  const pae = (a, b, c) => { const pp = a + b - c, da = Math.abs(pp - a), db = Math.abs(pp - b), dc = Math.abs(pp - c); return da <= db && da <= dc ? a : db <= dc ? b : c; };
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], rs = y * (stride + 1) + 1, os = y * stride;
    for (let x = 0; x < stride; x++) {
      const cur = raw[rs + x], a = x >= ch ? out[os + x - ch] : 0, b = y > 0 ? out[os - stride + x] : 0, c = x >= ch && y > 0 ? out[os - stride + x - ch] : 0;
      let v = cur;
      if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1; else if (f === 4) v += pae(a, b, c);
      out[os + x] = v & 255;
    }
  }
  return { w, h, ch, data: out, plte };
}
function sampleUV(img, u, v) {                                  // nearest sample; the palette is flat swatches
  const x = Math.min(img.w - 1, Math.max(0, Math.round(u * (img.w - 1))));
  const y = Math.min(img.h - 1, Math.max(0, Math.round((1 - v) * (img.h - 1))));  // OBJ v is bottom-up
  const o = (y * img.w + x) * img.ch;
  if (img.plte) return img.plte[img.data[o]];                  // indexed colour
  return [img.data[o], img.data[o + 1], img.data[o + 2]];
}

// ---- OBJ parse: positions (xyz, ignoring trailing vertex-colour), UVs, faces (triangulated)
function parseOBJ(text) {
  const pos = [], uv = [], tris = []; const seen = new Set();
  for (const line of text.split("\n")) {
    const t = line.trim(); if (!t || t[0] === "#") continue;
    const a = t.split(/\s+/);
    if (a[0] === "v") pos.push([+a[1], +a[2], +a[3]]);
    else if (a[0] === "vt") uv.push([+a[1], +a[2]]);
    else if (a[0] === "f") {
      const vs = a.slice(1).map((s) => { const [vi, ti] = s.split("/"); return [+vi - 1, ti ? +ti - 1 : -1]; });
      for (let i = 1; i + 1 < vs.length; i++) {                // fan-triangulate; dedupe Kenney's doubled emit
        const key = [vs[0], vs[i], vs[i + 1]].map((x) => x[0]).join(",");
        if (seen.has(key)) continue; seen.add(key);
        tris.push([vs[0], vs[i], vs[i + 1]]);
      }
    }
  }
  return { pos, uv, tris };
}

// Bake one model into the engine's [-1,1] unit box. Kenney is Y-up (footprint x/z,
// height +y, base at y=0); the engine is Z-up (footprint x/y, height +z). Rotate -90deg
// about X: (x,y,z) -> (x,-z,y) (proper rotation, winding preserved), then scale by 2 so
// one Kenney unit == one cell ([-1,1] spans 2). The model keeps its real proportions and
// sits with its base on z=0. Colour comes from the palette per face-vertex, except a
// `tintable` mesh (the nest) is forced white so the 63 per-nest instance tints show.
const SCALE = 2;
function qn(f) { let s = Math.round(f * 127); return s > 127 ? 127 : s < -127 ? -127 : s; }   // snorm8
function bakeMesh(OBJ, img, name, { tintable = false } = {}) {
  const { pos, uv, tris } = parseOBJ(readFileSync(join(OBJ, name + ".obj"), "utf8"));
  const verts = [];
  for (const tri of tris) {
    // face normal from the rotated geometry (flat shading); palette colour from vt
    const P = tri.map(([vi]) => { const p = pos[vi]; return [p[0] * SCALE, -p[2] * SCALE, p[1] * SCALE]; });
    const e1 = [P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]];
    const e2 = [P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]];
    let n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    const L = Math.hypot(...n) || 1; n = n.map((v) => v / L);
    for (let i = 0; i < 3; i++) {
      const ti = tri[i][1];
      const col = tintable || ti < 0 ? [255, 255, 255] : sampleUV(img, uv[ti][0], uv[ti][1]);
      verts.push([qn(P[i][0]), qn(P[i][1]), qn(P[i][2]), 0, qn(n[0]), qn(n[1]), qn(n[2]), 0, col[0], col[1], col[2], 255]);
    }
  }
  // bbox sanity (engine space): footprint should fit [-1,1], height >=0
  let lo = [9, 9, 9], hi = [-9, -9, -9];
  for (const v of verts) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], v[k] / 127); hi[k] = Math.max(hi[k], v[k] / 127); }
  console.log(`  ${name}: ${verts.length} verts  bbox[-1,1]=[${lo.map(n=>n.toFixed(2))}]..[${hi.map(n=>n.toFixed(2))}]`);
  return verts;
}

const DIR = process.argv[2] || ".";
const OUTC = process.argv[3] || "src/map_mesh_data.c";
const OBJ = join(DIR, "OBJ format");
const img = decodePNG(readFileSync(join(OBJ, "Textures", "colormap.png")));
console.log(`colormap ${img.w}x${img.h} (indexed=${!!img.plte}); baking map meshes -> ${OUTC}`);

// M_MAP_FLOOR, M_MAP_WALL, M_MAP_NEST  (must match the enum order in mesh_data.h)
const MESHES = [
  ["M_MAP_FLOOR", "tile", {}],
  ["M_MAP_WALL", "tower-square-bottom-a", {}],
  ["M_MAP_NEST", "detail-crystal", { tintable: true }],   // neutral: the 63 nest tints colour it
];
const all = []; const voff = [], vcnt = [];
for (const [, name, opt] of MESHES) { const v = bakeMesh(OBJ, img, name, opt); voff.push(all.length); vcnt.push(v.length); all.push(...v); }

let out = `/* GENERATED by tools/bake_map_assets.mjs from the Kenney Tower Defense Kit (CC0).\n`;
out += ` * The KENNEY map meshes (floor/wall/nest), normalised into the engine unit box with\n`;
out += ` * per-vertex palette colour. Source art is NOT committed; this baked buffer is. */\n`;
out += `#include "render.h"\n#include "mesh_data.h"\n\n`;
out += `const int8_t MAP_MESH_VERT[] = {\n`;
for (const v of all) out += `  ${v.slice(0, 8).join(",")}, ${v.slice(8).join(",")},\n`;
out += `};\n`;
out += `const uint32_t MAP_MESH_VOFF[M_MAP_COUNT] = { ${voff.join(", ")} };\n`;
out += `const uint32_t MAP_MESH_VCNT[M_MAP_COUNT] = { ${vcnt.join(", ")} };\n`;
out += `const uint32_t MAP_MESH_VERT_TOTAL = ${all.length};\n`;
writeFileSync(OUTC, out);
console.log(`wrote ${OUTC}: ${all.length} verts (${(all.length * 12 / 1024).toFixed(1)} KB), meshes [${MESHES.map(m=>m[0]).join(", ")}]`);
