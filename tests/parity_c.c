// Runs src/analysis.c over the frames file, quantising the input to bytes like the browser's canvas does,
// and prints the fields of the last frame as JSON.
#include "../src/analysis.h"
#include <stdio.h>
#include <stdlib.h>
static void arr(const char *k, const float *a, int n, int comma) {
  printf("\"%s\":[", k);
  for (int i = 0; i < n; i++) printf(i ? ",%.9g" : "%.9g", a[i]);
  printf("]%s", comma ? "," : "");
}
int main(int argc, char **argv) {
  const int W = 128, H = 72, n = W * H;
  FILE *f = fopen(argv[1], "rb");
  float *fr = malloc(n * sizeof(float)), *g = malloc(n * sizeof(float));
  Analyzer *an = an_new(W, H);
  while (fread(fr, sizeof(float), n, f) == (size_t)n) {
    for (int i = 0; i < n; i++) {   // as the JS: clamped byte, then grey from the three equal channels
      float v = fr[i] * 255; int b = v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f);
      g[i] = (0.299f * b + 0.587f * b + 0.114f * b) / 255;
    }
    an_update(an, g, 1.0f / 30);
  }
  printf("{");
  arr("level", an->level, n, 1); arr("motion", an->motion, n, 1); arr("trail", an->trail, n, 1);
  arr("flow", an->flow, an->fw * an->fh * 2, 1);
  printf("\"energy\":%.9g,\"cx\":%.9g,\"cy\":%.9g,", an->energy, an->cx, an->cy);
  arr("zMotion", an->zMotion, AN_COLS * AN_ROWS, 1);
  printf("\"bg\":%.9g}\n", an_background_level(an));
  return 0;
}
