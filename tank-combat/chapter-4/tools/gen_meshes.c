/* gen_meshes.c — host-bake the chapter-4 low-poly meshes into src/mesh_data.c.
 *
 * These shapes are fully determined at author time (no runtime input), so they are
 * BAKED on the host and committed, exactly like the escape table — the runtime
 * loads packed int8 vertex buffers, never a mesh parser. The shapes are original,
 * procedural low-poly geometry (CC0); ASSETS.md records where externally-sourced
 * art would drop into the same kind->mesh table instead.
 *
 * Each mesh is a NON-INDEXED triangle list (flat shading wants a per-face normal),
 * authored in a unit box [-1,1]; the instance's half-extents scale it to subcells.
 * Output vertex: int8 px,py,pz,0, nx,ny,nz,0 (snorm8x4 pos + snorm8x4 normal). */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, z; } V;
static V v(double x, double y, double z) { V r = { x, y, z }; return r; }
static V sub(V a, V b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static V cross(V a, V b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V norm(V a) { double m = sqrt(a.x * a.x + a.y * a.y + a.z * a.z); if (m < 1e-9) m = 1; return v(a.x / m, a.y / m, a.z / m); }

#define VSTRIDE 12          /* pos snorm8x4 + normal snorm8x4 + colour unorm8x4 */
static signed char OUT[900000];   /* room for the baked OBJ tank (a few thousand tris) + the small shapes */
static int NV = 0;
static int M_OFF[16], M_CNT[16], NM = 0, cur_off = 0;

static signed char qn(double f) { double s = f * 127.0; if (s > 127) s = 127; if (s < -127) s = -127; return (signed char)(s >= 0 ? s + 0.5 : s - 0.5); }
/* the procedural meshes are tintable: vertex colour defaults to WHITE, so
 * meshColour*instanceTint in the shader == the instance tint (the chapter-3 behaviour).
 * The Kenney map meshes, baked separately, carry their real per-vertex palette colour.
 * A mesh that wants its OWN baked colours (the tree's brown trunk + green canopy) sets
 * CUR before emitting and the instance tint is left white — exactly the town-mesh path. */
static unsigned char CUR[3] = { 255, 255, 255 };
static void colour(unsigned char r, unsigned char g, unsigned char b) { CUR[0] = r; CUR[1] = g; CUR[2] = b; }
static void emitv(V p, V n) {
  int o = NV * VSTRIDE;
  OUT[o] = qn(p.x); OUT[o+1] = qn(p.y); OUT[o+2] = qn(p.z); OUT[o+3] = 0;
  OUT[o+4] = qn(n.x); OUT[o+5] = qn(n.y); OUT[o+6] = qn(n.z); OUT[o+7] = 0;
  OUT[o+8] = (signed char)CUR[0]; OUT[o+9] = (signed char)CUR[1]; OUT[o+10] = (signed char)CUR[2]; OUT[o+11] = (signed char)255;
  NV++;
}
/* one flat triangle, its normal forced OUTWARD (these meshes are convex around the
 * origin, so the outward normal is the one pointing away from it). */
static void tri(V a, V b, V c) {
  V n = norm(cross(sub(b, a), sub(c, a)));
  V cen = v((a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3);
  if (n.x * cen.x + n.y * cen.y + n.z * cen.z < 0) n = v(-n.x, -n.y, -n.z);
  emitv(a, n); emitv(b, n); emitv(c, n);
}
static void quad(V a, V b, V c, V d) { tri(a, b, c); tri(a, c, d); }

static void mesh_begin(void) { cur_off = NV; }
static void mesh_end(void) { M_OFF[NM] = cur_off; M_CNT[NM] = NV - cur_off; NM++; }

/* a tapered box: bottom square (±bx,±by) at z=-1, top square (±tx,±ty) at z=+1.
 * tx==bx,ty==by gives a cube; tx<bx tapers it (hull / turret / pylon). */
static void taperbox(double bx, double by, double tx, double ty) {
  V b0 = v(-bx,-by,-1), b1 = v(bx,-by,-1), b2 = v(bx,by,-1), b3 = v(-bx,by,-1);
  V t0 = v(-tx,-ty, 1), t1 = v(tx,-ty, 1), t2 = v(tx,ty, 1), t3 = v(-tx,ty, 1);
  quad(b0, b3, b2, b1);    /* bottom */
  quad(t0, t1, t2, t3);    /* top */
  quad(b0, b1, t1, t0);    /* -y side */
  quad(b1, b2, t2, t1);    /* +x side */
  quad(b2, b3, t3, t2);    /* +y side */
  quad(b3, b0, t0, t3);    /* -x side */
}
/* a square pyramid: base (±1,±1) at z=-1, apex at (0,0,1). */
static void pyramid(void) {
  V b0 = v(-1,-1,-1), b1 = v(1,-1,-1), b2 = v(1,1,-1), b3 = v(-1,1,-1), ap = v(0,0,1);
  quad(b0, b3, b2, b1);    /* base */
  tri(b0, b1, ap); tri(b1, b2, ap); tri(b2, b3, ap); tri(b3, b0, ap);
}
/* a stylised low-poly tree: a TALL THIN brown trunk + a faceted green canopy, authored in
 * the unit box [-1,1] and carrying its OWN baked colours (the instance tint is white). It is
 * static scenery the host scatters on grass-cell CORNERS — the trunk is deliberately thin so
 * it reads as decoration tucked at a cell boundary, with no reason to expect a tank to hit it
 * (it never enters the sim — render only, no collision). The geometry is star-convex about
 * the origin (the origin sits inside the canopy), so tri()'s outward-normal flip is correct;
 * the trunk emits no top cap (it is hidden inside the canopy base, and a +z cap there would
 * mis-flip). */
static void tree(void) {
  const double tw = 0.13, zb = -1.0, zt = -0.30;          /* trunk: thin square prism */
  V b0 = v(-tw,-tw,zb), b1 = v(tw,-tw,zb), b2 = v(tw,tw,zb), b3 = v(-tw,tw,zb);
  V u0 = v(-tw,-tw,zt), u1 = v(tw,-tw,zt), u2 = v(tw,tw,zt), u3 = v(-tw,tw,zt);
  colour(110, 70, 40);                                    /* bark brown */
  quad(b0, b1, u1, u0); quad(b1, b2, u2, u1);             /* -y, +x sides */
  quad(b2, b3, u3, u2); quad(b3, b0, u0, u3);             /* +y, -x sides */
  quad(b0, b3, b2, b1);                                   /* bottom cap (no top: under canopy) */

  const double cw = 0.72, zr = -0.15, ztop = 1.0, zbot = -0.55;   /* canopy: square bipyramid */
  V c0 = v(-cw,-cw,zr), c1 = v(cw,-cw,zr), c2 = v(cw,cw,zr), c3 = v(-cw,cw,zr);
  V top = v(0,0,ztop), bot = v(0,0,zbot);
  colour(60, 120, 55);                                    /* foliage green */
  tri(c0,c1,top); tri(c1,c2,top); tri(c2,c3,top); tri(c3,c0,top);   /* upper cone */
  tri(c1,c0,bot); tri(c2,c1,bot); tri(c3,c2,bot); tri(c0,c3,bot);   /* lower cone */
  colour(255, 255, 255);                                  /* reset: later meshes stay tintable */
}

/* ---- OBJ loader (build-time only, no runtime parser ships) -------------------------
 * Reads a Wavefront OBJ (v / vn / o / usemtl / f with v//vn indices; tris..ngons, fan-
 * triangulated) and bakes it into the same flat-shaded 12-byte vertex format as the shapes
 * above. Used for the CC0 Quaternius tank: its object groups split cleanly into the HULL
 * (Tank_body + the two TrackMesh) and the TURRET (Tank_Turret + Tank_Gun), so each baked
 * part feeds one renderer kind. Both parts share ONE normalisation frame (computed over the
 * whole tank) so they stay registered while the renderer spins them independently. Per-
 * material grey keeps the tracks dark while the identity tint colours the body. */
#define OBJ_MAXV 80000
#define OBJ_MAXF 40000
static V   obj_pos[OBJ_MAXV]; static int obj_npos;
static V   obj_nrm[OBJ_MAXV]; static int obj_nnrm;
typedef struct { int v[8], n[8], nv, grey, part; } OFace;   /* part: 0 = hull, 1 = turret */
static OFace obj_face[OBJ_MAXF]; static int obj_nface;

/* material name -> a grey level that MULTIPLIES the instance's identity tint: tracks/wheels
 * stay dark, the body takes the tint, highlights read brighter. Unknown -> mid. */
static int mat_grey(const char* m) {
  if (strstr(m, "Wheel"))  return 70;
  if (strstr(m, "Dark"))   return 150;
  if (strstr(m, "Light"))  return 255;
  if (strstr(m, "Detail")) return 225;
  return 200;   /* Main */
}
/* Blender Y-up -> engine Z-up with the gun (Blender -x) pointing engine +x (facing 0). This
 * map is a proper rotation (det +1), so winding + normals carry over without a flip. */
static V to_engine(V b) { return v(-b.x, b.z, b.y); }

static int obj_load(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  obj_npos = obj_nnrm = obj_nface = 0;
  int part = 0, grey = 200;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == 'v' && line[1] == ' ') {
      V p; if (sscanf(line + 2, "%lf %lf %lf", &p.x, &p.y, &p.z) == 3 && obj_npos < OBJ_MAXV) obj_pos[obj_npos++] = to_engine(p);
    } else if (line[0] == 'v' && line[1] == 'n') {
      V p; if (sscanf(line + 3, "%lf %lf %lf", &p.x, &p.y, &p.z) == 3 && obj_nnrm < OBJ_MAXV) obj_nrm[obj_nnrm++] = to_engine(p);
    } else if (line[0] == 'o' && line[1] == ' ') {
      part = strstr(line, "Gun") ? 2 : strstr(line, "Turret") ? 1 : 0;   /* 0 hull · 1 turret body · 2 gun */
    } else if (!strncmp(line, "usemtl", 6)) {
      grey = mat_grey(line + 6);
    } else if (line[0] == 'f' && line[1] == ' ') {
      OFace fc; fc.nv = 0; fc.grey = grey; fc.part = part;
      for (char* tok = strtok(line + 2, " \t\r\n"); tok && fc.nv < 8; tok = strtok(NULL, " \t\r\n")) {
        int vi = 0, ni = 0;
        if (sscanf(tok, "%d//%d", &vi, &ni) < 1) continue;
        if (ni == 0) sscanf(tok, "%d/%*d/%d", &vi, &ni);          /* v/vt/vn fallback */
        fc.v[fc.nv] = vi - 1; fc.n[fc.nv] = ni - 1; fc.nv++;
      }
      if (fc.nv >= 3 && obj_nface < OBJ_MAXF) obj_face[obj_nface++] = fc;
    }
  }
  fclose(f);
  return obj_nface > 0;
}
/* bbox over the vertices used by faces whose part is in [pmin,pmax] (pmin<0 = every face). */
static void obj_bbox(int pmin, int pmax, double mn[3], double mx[3]) {
  for (int k = 0; k < 3; k++) { mn[k] = 1e30; mx[k] = -1e30; }
  for (int i = 0; i < obj_nface; i++) {
    OFace* fc = &obj_face[i];
    if (pmin >= 0 && (fc->part < pmin || fc->part > pmax)) continue;
    for (int j = 0; j < fc->nv; j++) {
      V p = obj_pos[fc->v[j]]; double c[3] = { p.x, p.y, p.z };
      for (int k = 0; k < 3; k++) { if (c[k] < mn[k]) mn[k] = c[k]; if (c[k] > mx[k]) mx[k] = c[k]; }
    }
  }
}
/* Bake faces with part in [pmin,pmax] (pmin<0 = all), normalised into the unit box about a
 * shared frame: XY centred on (px,py) — the ROTATION PIVOT — uniform scale `s` (aspect
 * preserved), the model bottom `mnz` anchored to z = -1 so the renderer's box rests it on
 * the floor, and an optional vertical lift `vex` (>1 raises a low model so it reads from a
 * top-down camera). A tank bakes its hull and its turret about the SAME pivot (the turret
 * ring) and scale, so the turret spins on its seat instead of swinging off. `grey` >= 0
 * forces a uniform vertex grey (255 = white, so an instance tint shows through, as the
 * mite's mode colour does); grey < 0 uses each face's per-material grey. Flat normal = the
 * OBJ face normal. */
static double g_emit_yaw = 0;   /* radians, applied about the vertical (z) axis at bake time */
static V yaw_xy(V p) { double c = cos(g_emit_yaw), s = sin(g_emit_yaw); return v(p.x * c - p.y * s, p.x * s + p.y * c, p.z); }

static void obj_emit(int pmin, int pmax, int grey, double px, double py, double s, double mnz, double vex) {
  if (s < 1e-9) s = 1;
  for (int i = 0; i < obj_nface; i++) {
    OFace* fc = &obj_face[i];
    if (pmin >= 0 && (fc->part < pmin || fc->part > pmax)) continue;
    int gv = grey >= 0 ? grey : fc->grey;
    colour((unsigned char)gv, (unsigned char)gv, (unsigned char)gv);
    V nn = v(0, 0, 0);                                  /* flat normal: average the face's vertex normals */
    for (int k = 0; k < fc->nv; k++) if (fc->n[k] >= 0 && fc->n[k] < obj_nnrm) { V g = obj_nrm[fc->n[k]]; nn.x += g.x; nn.y += g.y; nn.z += g.z; }
    nn = norm(yaw_xy(nn));
    V P[8];
    for (int k = 0; k < fc->nv; k++) {
      V p = obj_pos[fc->v[k]];
      double zz = (p.z - mnz) / s * vex - 1.0; if (zz > 1.0) zz = 1.0;
      P[k] = yaw_xy(v((p.x - px) / s, (p.y - py) / s, zz));
    }
    for (int t = 1; t + 1 < fc->nv; t++) { emitv(P[0], nn); emitv(P[t], nn); emitv(P[t + 1], nn); }   /* fan */
  }
  colour(255, 255, 255);                               /* reset for any later mesh */
}
/* the uniform scale that fits everything about pivot (px,py) bottom mnz into [-1,1] (with the
 * vertical lift applied to the height term), so no axis clips. */
static double fit_scale(double px, double py, const double mn[3], const double mx[3], double vex) {
  double sx = mx[0] - px; if (px - mn[0] > sx) sx = px - mn[0];
  double sy = mx[1] - py; if (py - mn[1] > sy) sy = py - mn[1];
  double sz = (mx[2] - mn[2]) * vex / 2.0;
  double s = sx; if (sy > s) s = sy; if (sz > s) s = sz;
  return s;
}

#define G_MITE_YAW (-1.5707963)   /* z-yaw (radians) so the crab's front leads along engine +x */
#define TANK_VEX 1.5               /* vertical lift: the real hull is low, so raise it to read from the top-down camera */
#define MITE_VEX 1.15              /* a gentler lift for the crab */

/* a downward marker: a square top that converges to an apex BELOW, so when the renderer parks it
 * above a selected tank the point hovers down at it. Authored apex-down in the unit box; star-convex
 * about the origin, so tri()'s outward-normal flip is correct. */
static void arrow(void) {
  V ap = v(0, 0, -1);                                                  /* apex (points down) */
  V t0 = v(-1,-1,1), t1 = v(1,-1,1), t2 = v(1,1,1), t3 = v(-1,1,1);    /* the wide top */
  quad(t0, t1, t2, t3);                                                /* top cap */
  tri(t1, t0, ap); tri(t2, t1, ap); tri(t3, t2, ap); tri(t0, t3, ap); /* four faces down to the apex */
}

int main(int argc, char** argv) {
  mesh_begin(); taperbox(1, 1, 1, 1);            mesh_end();   /* M_CUBE    */
  mesh_begin(); taperbox(1, 1, 0.40, 0.40);      mesh_end();   /* M_PYLON   (tall tapered tower) */
  mesh_begin(); pyramid();                       mesh_end();   /* M_PYRAMID (a mite spike) */
  mesh_begin(); taperbox(1, 1, 0.82, 0.66);      mesh_end();   /* M_HULL    (a tapered tank body) */
  mesh_begin(); taperbox(1, 1, 0.55, 0.55);      mesh_end();   /* M_TURRET  (a tapered dome) */
  mesh_begin(); tree();                          mesh_end();   /* M_TREE    (thin trunk + canopy) */
  mesh_begin(); arrow();                         mesh_end();   /* M_ARROW   (selected-tank down-marker) */

  /* M_TANK_HULL / M_TANK_TURRET — baked from the CC0 Quaternius tank OBJ when present
   * (ASSETS.md records the gdown re-bake), else a procedural fallback so the build never
   * needs the download. Both parts share ONE frame whose XY origin is the TURRET RING
   * (so the turret spins on its seat, not the tank centre), with a vertical lift so the
   * low hull reads as 3-D from the game's top-down camera. */
  const char* adir = argc > 1 ? argv[1] : "tools/quaternius";
  char tpath[768]; snprintf(tpath, sizeof tpath, "%s/Tank.obj", adir);
  if (obj_load(tpath)) {
    double mn[3], mx[3]; obj_bbox(-1, 0, mn, mx);                /* whole-tank bbox (z anchor + extents) */
    double tn[3], tx[3]; obj_bbox(1, 1, tn, tx);                 /* the turret BODY (part 1) → the ring pivot */
    double px = (tn[0] + tx[0]) / 2, py = (tn[1] + tx[1]) / 2;   /* turret-ring centre = the shared pivot */
    double vex = TANK_VEX, s = fit_scale(px, py, mn, mx, vex);
    mesh_begin(); obj_emit(0, 0, -1, px, py, s, mn[2], vex); mesh_end();   /* M_TANK_HULL   (body + tracks) */
    mesh_begin(); obj_emit(1, 2, -1, px, py, s, mn[2], vex); mesh_end();   /* M_TANK_TURRET (turret + gun)  */
    fprintf(stderr, "gen_meshes: baked tank from %s\n", tpath);
  } else {
    mesh_begin(); taperbox(1, 1, 0.82, 0.66);    mesh_end();   /* M_TANK_HULL   fallback */
    mesh_begin(); taperbox(1, 1, 0.55, 0.55);    mesh_end();   /* M_TANK_TURRET fallback */
    fprintf(stderr, "gen_meshes: %s not found — procedural tank fallback\n", tpath);
  }

  /* M_MITE — a low-poly CC0 Cute-Monsters crab as the swarm creature (LOD: the renderer
   * only binds it close up, where few are in view; the cheap M_PYRAMID draws the whole swarm
   * when zoomed out). Baked WHITE so the mite's mode tint (idle/hunt/home) colours it. */
  char mpath[768]; snprintf(mpath, sizeof mpath, "%s/Crab.obj", adir);
  if (obj_load(mpath)) {
    double mn[3], mx[3]; obj_bbox(-1, 0, mn, mx);
    double px = (mn[0] + mx[0]) / 2, py = (mn[1] + mx[1]) / 2;
    double vex = MITE_VEX, s = fit_scale(px, py, mn, mx, vex);
    g_emit_yaw = G_MITE_YAW;                                    /* face the crab's front toward engine +x (heading 0) */
    mesh_begin(); obj_emit(-1, 0, 255, px, py, s, mn[2], vex); mesh_end();   /* M_MITE (all faces, white) */
    g_emit_yaw = 0;
    fprintf(stderr, "gen_meshes: baked mite from %s\n", mpath);
  } else {
    mesh_begin(); pyramid();                     mesh_end();    /* M_MITE fallback = the spike */
    fprintf(stderr, "gen_meshes: %s not found — procedural mite fallback\n", mpath);
  }

  printf("/* GENERATED by tools/gen_meshes.c — do not edit. The chapter-4 low-poly\n");
  printf(" * meshes, host-baked (no runtime parser). See src/mesh_data.h / ASSETS.md. */\n");
  printf("#include \"render.h\"   /* for the K_* opaque-kind order */\n");
  printf("#include \"mesh_data.h\"\n\n");

  printf("const int8_t MESH_VERT[] = {\n");
  for (int i = 0; i < NV; i++) {
    int o = i * VSTRIDE;
    printf("  %d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d,\n",
           OUT[o], OUT[o+1], OUT[o+2], OUT[o+3], OUT[o+4], OUT[o+5], OUT[o+6], OUT[o+7],
           (unsigned char)OUT[o+8], (unsigned char)OUT[o+9], (unsigned char)OUT[o+10], (unsigned char)OUT[o+11]);
  }
  printf("};\n\n");

  printf("const uint32_t MESH_VOFF[M_PROC_COUNT] = {");
  for (int m = 0; m < NM; m++) printf(" %d%s", M_OFF[m], m + 1 < NM ? "," : " ");
  printf("};\n");
  printf("const uint32_t MESH_VCNT[M_PROC_COUNT] = {");
  for (int m = 0; m < NM; m++) printf(" %d%s", M_CNT[m], m + 1 < NM ? "," : " ");
  printf("};\n");
  printf("const uint32_t MESH_VERT_TOTAL = %d;\n\n", NV);

  /* the kind->mesh binding for the DYNAMIC things, keyed by the real enum so it can't
   * drift from render.h. The terrain (floors/walls/nests) is no longer a per-frame
   * kind — it is STATIC TOWN map data, placed by src/staticmap.c and drawn from the
   * uploaded-once static buffer with the town meshes. What remains here is the swarm,
   * the tanks, and the overlays; the placeholder pass (page toggle) binds M_CUBE for
   * every kind instead. */
  printf("const uint8_t MESH_FOR_KIND[K_OPAQUE_COUNT] = {\n");
  printf("  [K_RING]=M_ARROW, [K_MITE]=M_PYRAMID, [K_HULL]=M_TANK_HULL, [K_TURRET]=M_TANK_TURRET,\n");
  printf("  [K_BARREL]=M_CUBE, [K_DEST]=M_CUBE };\n");   /* the gun is part of the turret mesh now: K_BARREL emits no instances */
  return 0;
}
