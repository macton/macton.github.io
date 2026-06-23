/* ascii_dump.c — a GPU-free look at the scene, for developing the geometry.
 * Runs the real sim and prints the camera-relative view as text: terrain, body,
 * leg segments, joints, and feet. Not shipped; a way to see the walker without a
 * browser. Usage: ascii_dump [ticks] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/sim.h"
#include "../src/gait.h"
#include "../src/terrain.h"

#define GW 108
#define GH 30
#define CAM_Y (TERRAIN_MID - SUB / 2)
#define VW (16 * SUB)
#define VH (8 * SUB)

static char grid[GH][GW];

static void plot(int32_t wx, int32_t wy, int32_t camx, char ch) {
  int32_t rx = wx - camx, ry = wy - CAM_Y;
  int col = (int)(((int64_t)(rx + VW / 2) * GW) / VW);
  int row = (int)(((int64_t)(ry + VH / 2) * GH) / VH);
  if (col >= 0 && col < GW && row >= 0 && row < GH) grid[row][col] = ch;
}

static void seg(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t camx, char ch) {
  for (int s = 0; s <= 10; s++) {
    int32_t x = ax + (bx - ax) * s / 10, y = ay + (by - ay) * s / 10;
    plot(x, y, camx, ch);
  }
}

static void frame(const World* w) {
  for (int r = 0; r < GH; r++) { for (int c = 0; c < GW; c++) grid[r][c] = ' '; }
  int32_t camx = w->body_x;
  for (int32_t x = camx - VW / 2; x <= camx + VW / 2; x += 24) plot(x, terrain_y(x), camx, '.');
  for (int i = 0; i < N_LEGS; i++) {
    char sc = (i < N_LEGS / 2) ? 'o' : ':';
    const int32_t* JX = &w->joint_x[i * N_JOINT];
    const int32_t* JY = &w->joint_y[i * N_JOINT];
    for (int s = 0; s < N_SEG; s++) seg(JX[s], JY[s], JX[s + 1], JY[s + 1], camx, sc);
    for (int j = 0; j <= N_SEG; j++) plot(JX[j], JY[j], camx, '+');
    plot(JX[N_SEG], JY[N_SEG], camx, w->leg_clamped[i] ? 'X' : '*');
  }
  plot(w->body_x, w->body_y, camx, '@');
  plot(w->body_x - 84, w->body_y + 8, camx, '@');
  plot(w->body_x + 58, w->body_y, camx, '@');
  for (int r = 0; r < GH; r++) { grid[r][GW - 1] = 0; printf("%s\n", grid[r]); }
}

int main(int argc, char** argv) {
  int ticks = argc > 1 ? atoi(argv[1]) : 0;
  World w; sim_init(&w);
  for (int t = 0; t < ticks; t++) sim_tick(&w);
  printf("frame %u  body=(%d,%d)  curl_max=%u\n", w.frame, w.body_x, w.body_y, w.curl_max);
  frame(&w);
  return 0;
}
