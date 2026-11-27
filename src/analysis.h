// C port of Wake's js/analysis.js and js/presence.js (copies in reference/). Same fields, same maths.
// Pure code with no I/O, so tests/parity checks it against the JavaScript.
#pragma once

#define AN_COLS 6
#define AN_ROWS 3

typedef struct {
  int w, h, fw, fh;
  float *gray, *cur, *prev, *tmp, *tmp2;
  float *level, *motion, *trail, *fresh, *gx, *gy;
  float *hc, *hp, *sxx, *syy, *sxy, *sxt, *syt, *flow;
  int *hot, hotN;
  unsigned hist[128];
  float lo, hi, noise, energy, cx, cy;
  int frames;
  float zMotion[AN_COLS * AN_ROWS], zLevel[AN_COLS * AN_ROWS], zFlowX[AN_COLS * AN_ROWS], zFlowY[AN_COLS * AN_ROWS];
} Analyzer;

Analyzer *an_new(int w, int h);
void an_free(Analyzer *a);
// gray: w*h values 0..1 (already mirrored if wanted), dt: seconds since the previous frame.
void an_update(Analyzer *a, const float *gray, float dt);
float an_background_level(const Analyzer *a);
