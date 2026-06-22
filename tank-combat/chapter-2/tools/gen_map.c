/* gen_map.c — HOST-ONLY. Emits src/map_data.c (the initial 4x4 world map) to
 * stdout. Run from gen.sh; the result is committed and compiled into the wasm.
 * The map is fully determined at authoring time, so it is baked as data.
 *
 * Each of the 16 screen-grids is given a DISTINCT interior, both so they are easy
 * to tell apart when scrolling/debugging and so they surface different pathing
 * cases (open, obstacles, dead-ends, rooms, an unreachable island, ...). The art
 * is drawn freely, then the centre cross (row 7 / col 10) and the four centred
 * border gaps are punched open LAST — so every screen stays connected through the
 * cross and its gaps line up across screen borders and the outer wrap, no matter
 * what the art does. An enclosed pocket drawn off the cross therefore stays an
 * island (unreachable), which is deliberate. A connectivity self-check runs after
 * generation and fails the build if any screen's four gaps aren't joined. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/defs.h"

static int m[GRID_H][GRID_W];   /* 1 = wall, working buffer for one screen */

static void setw(int x, int y, int v) { if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H) m[y][x] = v; }
static void box (int x0, int y0, int x1, int y1) { for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++) setw(x,y,1); }
static void ring(int x0, int y0, int x1, int y1) { for (int x=x0;x<=x1;x++){setw(x,y0,1);setw(x,y1,1);} for (int y=y0;y<=y1;y++){setw(x0,y,1);setw(x1,y,1);} }
static void hbar(int y, int x0, int x1) { for (int x=x0;x<=x1;x++) setw(x,y,1); }
static void vbar(int x, int y0, int y1) { for (int y=y0;y<=y1;y++) setw(x,y,1); }

/* distinct interior for each screen (borders already set, cross punched after) */
static void draw_interior(uint32_t s) {
  switch (s) {
    case 0: /* OPEN — baseline, straight-line flow field */ break;
    case 1: /* four corner blocks */
      box(2,2,4,4); box(15,2,17,4); box(2,10,4,12); box(15,10,17,12); break;
    case 2: /* pillar field */
      for (int y=2;y<=12;y+=2) for (int x=2;x<=18;x+=3) setw(x,y,1); break;
    case 3: /* ISLAND: an enclosed pocket (unreachable) + a block */
      ring(13,2,17,5); box(3,10,6,12); break;
    case 4: /* short diagonals */
      for (int k=0;k<6;k++){ setw(2+k,2+k,1); setw(3+k,2+k,1); setw(12+k,8+k,1); setw(13+k,8+k,1);} break;
    case 5: /* horizontal channels */
      hbar(3,2,8); hbar(3,12,18); hbar(11,2,8); hbar(11,12,18); break;
    case 6: /* four rooms with doorways */
      ring(2,2,8,6);  setw(5,6,0);  ring(12,2,18,6);  setw(15,6,0);
      ring(2,9,8,13); setw(5,9,0);  ring(12,9,18,13); setw(15,9,0); break;
    case 7: /* staggered brick maze */
      for (int y=2;y<=12;y+=2){ int off=((y/2)&1)?0:2; for (int x=1+off;x<=17;x+=4) hbar(y,x,x+2);} break;
    case 8: /* inward dead-end teeth */
      for (int x=2;x<=8;x+=2) vbar(x,1,5); for (int x=12;x<=18;x+=2) vbar(x,9,13); break;
    case 9: /* two big rectangles */
      box(2,2,7,6); box(12,9,18,13); break;
    case 10: /* scattered light blocks */
      setw(4,3,1); box(15,3,16,4); setw(5,11,1); box(13,11,14,12); setw(8,4,1); break;
    case 11: /* big frame (the cross makes four doorways through it) */
      ring(5,3,15,11); break;
    case 12: /* vertical stripes */
      for (int x=2;x<=18;x+=2) vbar(x,2,12); break;
    case 13: /* horizontal stripes */
      for (int y=2;y<=12;y+=2) hbar(y,2,18); break;
    case 14: /* nested frames */
      ring(2,2,17,12); ring(5,4,14,10); break;
    default: /* 15: big X */
      for (int k=0;k<=12;k++){ setw(3+k,1+k,1); setw(16-k,1+k,1);} break;
  }
}

static uint32_t g[N_SCREENS][GRID_H];

/* --- connectivity self-check over the assembled world --------------------- */
static int world_wall(int wcx, int wcy) {
  wcx = (wcx % BIG_W + BIG_W) % BIG_W; wcy = (wcy % BIG_H + BIG_H) % BIG_H;
  uint32_t s = (wcy / GRID_H) * SCREENS_X + (wcx / GRID_W);
  return (g[s][wcy % GRID_H] >> (wcx % GRID_W)) & 1u;
}
static void check_connectivity(void) {
  /* every screen's four centred gaps must be mutually reachable; and the whole
   * gap-network must be one component (islands are allowed extra components). */
  static int seen[BIG_W * BIG_H], q[BIG_W * BIG_H];
  for (int i = 0; i < BIG_W * BIG_H; i++) seen[i] = 0;
  int sc = (0) * BIG_W + 10;                       /* screen 0 north gap (10,0) */
  int head = 0, tail = 0; seen[sc] = 1; q[tail++] = sc;
  int reached = 0, total_open = 0;
  while (head < tail) { int c = q[head++]; reached++; int cx = c % BIG_W, cy = c / BIG_W;
    int dx[4]={1,-1,0,0}, dy[4]={0,0,1,-1};
    for (int k=0;k<4;k++){ int nx=(cx+dx[k]+BIG_W)%BIG_W, ny=(cy+dy[k]+BIG_H)%BIG_H, n=ny*BIG_W+nx;
      if (!seen[n] && !world_wall(nx,ny)) { seen[n]=1; q[tail++]=n; } } }
  for (int i=0;i<BIG_W*BIG_H;i++) if (!world_wall(i%BIG_W, i/BIG_W)) total_open++;

  int fail = 0;
  for (uint32_t s=0;s<N_SCREENS;s++){ int sx=s%SCREENS_X, sy=s/SCREENS_X, ox=sx*GRID_W, oy=sy*GRID_H;
    int gaps[4][2]={{ox+10,oy+0},{ox+10,oy+GRID_H-1},{ox+0,oy+7},{ox+GRID_W-1,oy+7}};
    for (int i=0;i<4;i++){ int gc=gaps[i][1]*BIG_W+gaps[i][0];
      if (!seen[gc]) { fprintf(stderr,"FAIL: screen %u gap (%d,%d) not connected\n",s,gaps[i][0],gaps[i][1]); fail=1; } } }
  fprintf(stderr,"connectivity: %d/%d open cells reachable from the gap-network (%d island cells)\n",
          reached, total_open, total_open-reached);
  if (fail) exit(1);
}

int main(void) {
  for (uint32_t s = 0; s < N_SCREENS; s++) {
    for (int y=0;y<GRID_H;y++) for (int x=0;x<GRID_W;x++) m[y][x]=0;   /* open */
    for (int x=0;x<GRID_W;x++){ m[0][x]=1; m[GRID_H-1][x]=1; }         /* N,S border */
    for (int y=0;y<GRID_H;y++){ m[y][0]=1; m[y][GRID_W-1]=1; }         /* W,E border */
    draw_interior(s);
    for (int y=0;y<GRID_H;y++) m[y][GRID_W/2]=0;                       /* punch col 10 (opens N,S gaps) */
    for (int x=0;x<GRID_W;x++) m[GRID_H/2][x]=0;                       /* punch row 7 (opens W,E gaps) */
    for (int y=0;y<GRID_H;y++){ uint32_t w=0; for (int x=0;x<GRID_W;x++) if (m[y][x]) w|=1u<<x; g[s][y]=w; }
  }
  check_connectivity();

  printf("/* GENERATED by tools/gen_map.c (gen.sh) — do not edit by hand.\n");
  printf(" * The initial 4x4 world map, one 20-bit word per screen-row\n");
  printf(" * (GRID_INIT[screen*GRID_H + cy], bit c == column c, 1 = wall).\n");
  printf(" * Each screen has a distinct interior; the centre cross (row 7 / col 10)\n");
  printf(" * and the four centred border gaps are open, lining up across screen\n");
  printf(" * borders and the outer wrap, so the world is one connected toroidal grid. */\n");
  printf("#include \"map.h\"\n\n");
  printf("const uint32_t GRID_INIT[N_SCREENS * GRID_H] = {\n");
  for (uint32_t s = 0; s < N_SCREENS; s++) {
    printf("  /* screen %2u  (sx %u, sy %u) */\n", s, s % SCREENS_X, s / SCREENS_X);
    for (int cy = 0; cy < GRID_H; cy++) {
      printf("  0x%05X,  /* ", g[s][cy]);
      for (int cx = 0; cx < GRID_W; cx++) putchar((g[s][cy] >> cx) & 1u ? '#' : '.');
      printf(" */\n");
    }
  }
  printf("};\n");
  return 0;
}
