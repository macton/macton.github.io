/* gen_map.c — HOST-ONLY. Emits src/map_data.c (the initial 4x4 world map) to
 * stdout. Run from gen.sh; the result is committed and compiled into the wasm.
 *
 * Each screen has (a) solid borders, (b) an always-open RING one cell inside the
 * border, and (c) a distinct interior pattern in the centre. Between neighbouring
 * screens (and across the outer wrap) there are 0, 1, or 2 matching openings —
 * not every border has one, but a randomised spanning tree forces enough that no
 * screen is an island. Every opening sits on the ring, so all of a screen's
 * openings are mutually reachable; the ring + tree make the whole world one
 * connected toroidal grid. An enclosed pocket drawn in a centre stays an island
 * (deliberate). A connectivity self-check fails the build if any screen is
 * unreachable. Deterministic (fixed seed), so the baked map is reproducible. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/defs.h"

static int m[GRID_H][GRID_W];          /* working buffer for one screen (1 = wall) */
static int M[N_SCREENS][GRID_H][GRID_W];

static void setw(int x, int y, int v) { if (x>=0&&x<GRID_W&&y>=0&&y<GRID_H) m[y][x]=v; }
static void box (int x0,int y0,int x1,int y1){ for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)setw(x,y,1); }
static void ring(int x0,int y0,int x1,int y1){ for(int x=x0;x<=x1;x++){setw(x,y0,1);setw(x,y1,1);} for(int y=y0;y<=y1;y++){setw(x0,y,1);setw(x1,y,1);} }
static void hbar(int y,int x0,int x1){ for(int x=x0;x<=x1;x++) setw(x,y,1); }
static void vbar(int x,int y0,int y1){ for(int y=y0;y<=y1;y++) setw(x,y,1); }

/* a distinct centre for each screen (screen 0 included — no blank screens) */
static void draw_interior(uint32_t s) {
  switch (s) {
    case 0:  box(8,5,11,9); break;                                   /* solid central block */
    case 1:  box(3,3,5,5); box(14,3,16,5); box(3,9,5,11); box(14,9,16,11); break; /* corner blocks */
    case 2:  for(int y=3;y<=11;y+=2) for(int x=3;x<=16;x+=3) setw(x,y,1); break;  /* pillars */
    case 3:  ring(12,3,16,6); box(3,9,6,11); break;                  /* ISLAND pocket + block */
    case 4:  for(int k=0;k<6;k++){setw(3+k,3+k,1);setw(4+k,3+k,1);setw(12+k,4+k,1);} break; /* diagonals */
    case 5:  hbar(4,3,8); hbar(4,11,16); hbar(10,3,8); hbar(10,11,16); break;     /* channels */
    case 6:  ring(3,3,8,6); setw(6,6,0); ring(11,8,16,11); setw(13,8,0); break;   /* rooms + doorways */
    case 7:  for(int y=3;y<=11;y+=2){int o=((y/2)&1)?0:2; for(int x=3+o;x<=15;x+=4) hbar(y,x,x+2);} break; /* brick maze */
    case 8:  for(int x=4;x<=8;x+=2) vbar(x,2,6); for(int x=11;x<=15;x+=2) vbar(x,8,12); break; /* dead-end teeth */
    case 9:  box(3,3,8,6); box(11,8,16,11); break;                   /* two rectangles */
    case 10: ring(13,3,15,5); setw(4,4,1); box(8,10,10,11); break;   /* small island + scatter */
    case 11: ring(5,3,14,11); setw(10,3,0); setw(10,11,0); setw(5,7,0); setw(14,7,0); break; /* frame w/ doorways */
    case 12: ring(3,2,16,12); ring(6,5,13,9);
             setw(3,7,0); setw(16,7,0); setw(6,7,0); setw(13,7,0); break; /* nested frames, doorways on row 7 (no islands) */
    case 13: hbar(3,3,12); vbar(12,3,7); hbar(7,7,16); vbar(7,7,11); hbar(11,7,16); break; /* zigzag */
    case 14: for(int y=3;y<=10;y+=3) for(int x=3;x<=15;x+=3) box(x,y,x+1,y+1); break; /* checker blocks */
    default: for(int k=0;k<5;k++){ hbar(3+k,3,16-k); } box(8,9,11,11); break;     /* steps + block */
  }
}

/* --- deterministic PRNG + spanning tree over the 4x4 toroidal screen graph -- */
static uint32_t rs = 0xC0FFEEu;
static uint32_t rng(void){ rs = rs*1664525u + 1013904223u; return rs>>9; }
static int par[N_SCREENS];
static int find(int a){ while(par[a]!=a){par[a]=par[par[a]];a=par[a];} return a; }
static int uni(int a,int b){ a=find(a);b=find(b); if(a==b)return 0; par[a]=b; return 1; }

/* 32 adjacencies: each screen's East and South neighbour (toroidal) */
static int adj_sa[32], adj_sb[32], adj_type[32], adj_open[32];  /* type 0=East,1=South */

int main(void) {
  for (uint32_t s = 0; s < N_SCREENS; s++) {
    for (int y=0;y<GRID_H;y++) for (int x=0;x<GRID_W;x++) m[y][x]=0;
    for (int x=0;x<GRID_W;x++){ m[0][x]=1; m[GRID_H-1][x]=1; }
    for (int y=0;y<GRID_H;y++){ m[y][0]=1; m[y][GRID_W-1]=1; }
    draw_interior(s);
    for (int x=1;x<=GRID_W-2;x++){ m[1][x]=0; m[GRID_H-2][x]=0; }   /* re-open the ring */
    for (int y=1;y<=GRID_H-2;y++){ m[y][1]=0; m[y][GRID_W-2]=0; }
    for (int y=0;y<GRID_H;y++) for (int x=0;x<GRID_W;x++) M[s][y][x]=m[y][x];
  }

  /* enumerate adjacencies */
  int na = 0;
  for (uint32_t s=0;s<N_SCREENS;s++){ int sx=s%SCREENS_X, sy=s/SCREENS_X;
    adj_sa[na]=s; adj_sb[na]=sy*SCREENS_X + (sx+1)%SCREENS_X; adj_type[na]=0; na++;     /* East */
    adj_sa[na]=s; adj_sb[na]=((sy+1)%SCREENS_Y)*SCREENS_X + sx; adj_type[na]=1; na++;   /* South */
  }
  /* randomised spanning tree: shuffle adjacency order, union to connect all 16 */
  int order[32]; for (int i=0;i<32;i++) order[i]=i;
  for (int i=31;i>0;i--){ int j=rng()%(i+1); int t=order[i];order[i]=order[j];order[j]=t; }
  for (int i=0;i<N_SCREENS;i++) par[i]=i;
  int tree[32]={0};
  for (int i=0;i<32;i++){ int e=order[i]; if (uni(adj_sa[e],adj_sb[e])) tree[e]=1; }
  /* 0-2 openings per adjacency; tree edges get >=1 */
  for (int e=0;e<32;e++) adj_open[e] = tree[e] ? (1 + (int)(rng()%2)) : (int)(rng()%3);

  /* apply openings (matched cells on both screens, on the ring) */
  for (int e=0;e<32;e++){ int sa=adj_sa[e], sb=adj_sb[e];
    int span = (adj_type[e]==0) ? GRID_H : GRID_W;   /* rows for East, cols for South */
    int p0 = (adj_type[e]==0) ? 1 : 1, p1 = span-2;  /* valid opening positions (on the ring) */
    int used = -99;
    for (int k=0;k<adj_open[e];k++){ int pos; do { pos = p0 + (int)(rng()%(p1-p0+1)); } while (pos==used); used=pos;
      if (adj_type[e]==0){ M[sa][pos][GRID_W-1]=0; M[sb][pos][0]=0; }      /* East/West crossing */
      else               { M[sa][GRID_H-1][pos]=0; M[sb][0][pos]=0; }      /* South/North crossing */
    }
  }

  /* connectivity self-check: every screen must have a reachable cell */
  static int seen[BIG_W*BIG_H], q[BIG_W*BIG_H];
  for (int i=0;i<BIG_W*BIG_H;i++) seen[i]=0;
  #define WALL(wx,wy) (M[((wy)/GRID_H)*SCREENS_X+((wx)/GRID_W)][(wy)%GRID_H][(wx)%GRID_W])
  int src = 7*BIG_W + 1;                       /* world (1,7): screen 0 ring, always open */
  int head=0,tail=0; seen[src]=1; q[tail++]=src; int reached=0, total=0;
  while (head<tail){ int c=q[head++]; reached++; int cx=c%BIG_W, cy=c/BIG_W; int dx[4]={1,-1,0,0},dy[4]={0,0,1,-1};
    for (int k=0;k<4;k++){ int nx=(cx+dx[k]+BIG_W)%BIG_W, ny=(cy+dy[k]+BIG_H)%BIG_H, n=ny*BIG_W+nx;
      if(!seen[n] && !WALL(nx,ny)){ seen[n]=1; q[tail++]=n; } } }
  for (int i=0;i<BIG_W*BIG_H;i++) if(!WALL(i%BIG_W,i/BIG_W)) total++;
  int fail=0;
  for (uint32_t s=0;s<N_SCREENS;s++){ int sx=s%SCREENS_X, sy=s/SCREENS_X, any=0;
    for (int y=0;y<GRID_H&&!any;y++) for (int x=0;x<GRID_W&&!any;x++) if(seen[(sy*GRID_H+y)*BIG_W+(sx*GRID_W+x)]) any=1;
    if(!any){ fprintf(stderr,"FAIL: screen %u is an island (unreachable)\n",s); fail=1; } }
  fprintf(stderr,"map: %d/%d open cells reachable; %d island cells\n", reached, total, total-reached);
  if (fail) return 1;

  printf("/* GENERATED by tools/gen_map.c (gen.sh) — do not edit by hand.\n");
  printf(" * 4x4 world, one 20-bit word per screen-row (bit c == column c, 1 = wall).\n");
  printf(" * Solid borders + an open ring + a distinct centre per screen; 0-2 matching\n");
  printf(" * openings per screen border (a spanning tree guarantees no island screen). */\n");
  printf("#include \"map.h\"\n\n");
  printf("const uint32_t GRID_INIT[N_SCREENS * GRID_H] = {\n");
  for (uint32_t s = 0; s < N_SCREENS; s++) {
    printf("  /* screen %2u  (sx %u, sy %u) */\n", s, s % SCREENS_X, s / SCREENS_X);
    for (int cy = 0; cy < GRID_H; cy++) {
      uint32_t w = 0; for (int cx=0;cx<GRID_W;cx++) if (M[s][cy][cx]) w |= 1u<<cx;
      printf("  0x%05X,  /* ", w);
      for (int cx = 0; cx < GRID_W; cx++) putchar(M[s][cy][cx] ? '#' : '.');
      printf(" */\n");
    }
  }
  printf("};\n");
  return 0;
}
