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

typedef struct { double x, y, z; } V;
static V v(double x, double y, double z) { V r = { x, y, z }; return r; }
static V sub(V a, V b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static V cross(V a, V b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V norm(V a) { double m = sqrt(a.x * a.x + a.y * a.y + a.z * a.z); if (m < 1e-9) m = 1; return v(a.x / m, a.y / m, a.z / m); }

#define VSTRIDE 12          /* pos snorm8x4 + normal snorm8x4 + colour unorm8x4 */
static signed char OUT[60000];
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

int main(void) {
  mesh_begin(); taperbox(1, 1, 1, 1);            mesh_end();   /* M_CUBE    */
  mesh_begin(); taperbox(1, 1, 0.40, 0.40);      mesh_end();   /* M_PYLON   (tall tapered tower) */
  mesh_begin(); pyramid();                       mesh_end();   /* M_PYRAMID (a mite spike) */
  mesh_begin(); taperbox(1, 1, 0.82, 0.66);      mesh_end();   /* M_HULL    (a tapered tank body) */
  mesh_begin(); taperbox(1, 1, 0.55, 0.55);      mesh_end();   /* M_TURRET  (a tapered dome) */
  mesh_begin(); tree();                          mesh_end();   /* M_TREE    (thin trunk + canopy) */

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
  printf("  [K_RING]=M_CUBE, [K_MITE]=M_PYRAMID, [K_HULL]=M_HULL, [K_TURRET]=M_TURRET,\n");
  printf("  [K_BARREL]=M_CUBE, [K_DEST]=M_CUBE };\n");
  return 0;
}
