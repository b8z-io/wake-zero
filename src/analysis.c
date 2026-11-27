// C port of Wake's js/analysis.js (and backgroundLevel from js/presence.js; copies in reference/). Kept
// line-for-line close to the JavaScript so the two can be compared; tests/parity.sh checks they agree.
#include "analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline int mini(int a, int b) { return a < b ? a : b; }
static inline int maxi(int a, int b) { return a > b ? a : b; }
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void box_blur(const float *src, float *dst, float *tmp, int w, int h, int r) {
  const float k = 1.0f / (2 * r + 1);
  for (int y = 0; y < h; y++) {
    const int o = y * w;
    float s = 0;
    for (int i = -r; i <= r; i++) s += src[o + mini(maxi(i, 0), w - 1)];
    for (int x = 0; x < w; x++) {
      tmp[o + x] = s * k;
      s += src[o + mini(x + r + 1, w - 1)] - src[o + maxi(x - r, 0)];
    }
  }
  for (int x = 0; x < w; x++) {
    float s = 0;
    for (int i = -r; i <= r; i++) s += tmp[mini(maxi(i, 0), h - 1) * w + x];
    for (int y = 0; y < h; y++) {
      dst[y * w + x] = s * k;
      s += tmp[mini(y + r + 1, h - 1) * w + x] - tmp[maxi(y - r, 0) * w + x];
    }
  }
}

static float percentile(const float *arr, int n, float frac, float lo, float hi, unsigned *hist) {
  const int bins = 128;
  memset(hist, 0, bins * sizeof *hist);
  const float s = bins / (hi - lo);
  for (int i = 0; i < n; i++) {
    float v = (arr[i] - lo) * s;
    int b = v < 0 ? 0 : (int)v;   // JS: (v | 0), then clamped
    hist[mini(bins - 1, maxi(0, b))]++;
  }
  const float target = frac * n;
  unsigned acc = 0;
  for (int b = 0; b < bins; b++) {
    acc += hist[b];
    if (acc >= target) return lo + (b + 0.5f) / s;
  }
  return hi;
}

Analyzer *an_new(int w, int h) {
  Analyzer *a = calloc(1, sizeof *a);
  a->w = w; a->h = h; a->fw = w >> 1; a->fh = h >> 1;
  const int n = w * h, fn = a->fw * a->fh;
  float **full[] = { &a->gray, &a->cur, &a->prev, &a->tmp, &a->tmp2, &a->level, &a->motion, &a->trail, &a->fresh, &a->gx, &a->gy };
  for (size_t i = 0; i < sizeof full / sizeof *full; i++) *full[i] = calloc(n, sizeof(float));
  float **half[] = { &a->hc, &a->hp, &a->sxx, &a->syy, &a->sxy, &a->sxt, &a->syt };
  for (size_t i = 0; i < sizeof half / sizeof *half; i++) *half[i] = calloc(fn, sizeof(float));
  a->flow = calloc(fn * 2, sizeof(float));
  a->hot = calloc(n, sizeof(int));
  a->lo = 0; a->hi = 1; a->noise = 0.01f; a->cx = 0.5f; a->cy = 0.5f;
  return a;
}

void an_free(Analyzer *a) {
  if (!a) return;
  float *all[] = { a->gray, a->cur, a->prev, a->tmp, a->tmp2, a->level, a->motion, a->trail, a->fresh, a->gx, a->gy,
                   a->hc, a->hp, a->sxx, a->syy, a->sxy, a->sxt, a->syt, a->flow };
  for (size_t i = 0; i < sizeof all / sizeof *all; i++) free(all[i]);
  free(a->hot);
  free(a);
}

static void compute_flow(Analyzer *a, float md, float span, float dt, int first) {
  const int w = a->w, fw = a->fw, fh = a->fh;
  float *hc = a->hc, *hp = a->hp, *tmp = a->tmp;
  const float *cur = a->cur, *prev = a->prev;
  const float inv = 1 / (4 * span);
  for (int y = 0; y < fh; y++) {
    for (int x = 0; x < fw; x++) {
      const int i0 = 2 * y * w + 2 * x, i1 = i0 + w;
      hc[y * fw + x] = (cur[i0] + cur[i0 + 1] + cur[i1] + cur[i1 + 1]) * inv;
      hp[y * fw + x] = (prev[i0] + prev[i0 + 1] + prev[i1] + prev[i1 + 1] - 4 * md) * inv;
    }
  }
  box_blur(hc, hc, tmp, fw, fh, 1);
  box_blur(hp, hp, tmp, fw, fh, 1);
  float *sxx = a->sxx, *syy = a->syy, *sxy = a->sxy, *sxt = a->sxt, *syt = a->syt;
  for (int y = 0; y < fh; y++) {
    for (int x = 0; x < fw; x++) {
      const int i = y * fw + x;
      const int xl = y * fw + maxi(x - 1, 0), xr = y * fw + mini(x + 1, fw - 1);
      const int yu = maxi(y - 1, 0) * fw + x, yd = mini(y + 1, fh - 1) * fw + x;
      const float ix = (hc[xr] - hc[xl] + hp[xr] - hp[xl]) * 0.25f;
      const float iy = (hc[yd] - hc[yu] + hp[yd] - hp[yu]) * 0.25f;
      const float it = hc[i] - hp[i];
      sxx[i] = ix * ix; syy[i] = iy * iy; sxy[i] = ix * iy; sxt[i] = ix * it; syt[i] = iy * it;
    }
  }
  float *ss[] = { sxx, syy, sxy, sxt, syt };
  for (int k = 0; k < 5; k++) box_blur(ss[k], ss[k], tmp, fw, fh, 2);
  const float kf = first ? 0 : 0.45f, MAXV = 3;
  for (int y = 0; y < fh; y++) {
    for (int x = 0; x < fw; x++) {
      const int i = y * fw + x;
      const float A = sxx[i], B = sxy[i], C = syy[i];
      const float det = A * C - B * B, tr = A + C;
      const float lmin = (tr - sqrtf(fmaxf(tr * tr - 4 * det, 0))) / 2;
      float vx = 0, vy = 0;
      const int j = 2 * y * w + 2 * x;
      const float m = fmaxf(fmaxf(a->fresh[j], a->fresh[j + 1]), fmaxf(a->fresh[j + w], a->fresh[j + w + 1]));
      if (lmin > 2e-6f && m > 0.02f) {
        vx = (-C * sxt[i] + B * syt[i]) / det;
        vy = (B * sxt[i] - A * syt[i]) / det;
        const float mag = hypotf(vx, vy);
        if (mag > MAXV) { vx *= MAXV / mag; vy *= MAXV / mag; }
        vx *= fminf(1, m * 2); vy *= fminf(1, m * 2);
      }
      vx = vx / fw / dt; vy = vy / fh / dt;
      a->flow[2 * i] = a->flow[2 * i] * kf + vx * (1 - kf);
      a->flow[2 * i + 1] = a->flow[2 * i + 1] * kf + vy * (1 - kf);
    }
  }
}

static void compute_zones(Analyzer *a) {
  const int w = a->w, h = a->h, fw = a->fw, fh = a->fh, cols = AN_COLS, rows = AN_ROWS, z = cols * rows;
  memset(a->zMotion, 0, sizeof a->zMotion); memset(a->zLevel, 0, sizeof a->zLevel);
  memset(a->zFlowX, 0, sizeof a->zFlowX); memset(a->zFlowY, 0, sizeof a->zFlowY);
  for (int y = 0, i = 0; y < h; y++) {
    const int zr = mini(rows - 1, (y * rows) / h) * cols;
    for (int x = 0; x < w; x++, i++) {
      const int k = zr + mini(cols - 1, (x * cols) / w);
      a->zMotion[k] += a->motion[i]; a->zLevel[k] += a->level[i];
    }
  }
  for (int y = 0, i = 0; y < fh; y++) {
    const int zr = mini(rows - 1, (y * rows) / fh) * cols;
    for (int x = 0; x < fw; x++, i++) {
      const int k = zr + mini(cols - 1, (x * cols) / fw);
      a->zFlowX[k] += a->flow[2 * i]; a->zFlowY[k] += a->flow[2 * i + 1];
    }
  }
  const float ka = (float)z / (w * h), kb = (float)z / (fw * fh);
  for (int k = 0; k < z; k++) { a->zMotion[k] *= ka; a->zLevel[k] *= ka; a->zFlowX[k] *= kb; a->zFlowY[k] *= kb; }
}

void an_update(Analyzer *a, const float *gray_in, float dt) {
  const int w = a->w, h = a->h, n = w * h;
  float *cur = a->cur, *prev = a->prev, *tmp = a->tmp;
  dt = fminf(fmaxf(dt, 1.0f / 120), 0.25f);
  memcpy(a->gray, gray_in, n * sizeof(float));
  box_blur(a->gray, cur, tmp, w, h, 1);

  // Auto levels, so a dim room still gives a full-range field.
  const float lo = percentile(cur, n, 0.02f, 0, 1, a->hist), hi = percentile(cur, n, 0.98f, 0, 1, a->hist);
  const int first = a->frames++ == 0;
  const float ka = first ? 1 : 1 - expf(-dt * 2);
  a->lo += (lo - a->lo) * ka;
  a->hi += (hi - a->hi) * ka;
  const float span = fmaxf(a->hi - a->lo, 0.08f);

  if (first) memcpy(prev, cur, n * sizeof(float));

  // Motion: frame difference with the global change removed, over a noise floor.
  float md = 0;
  for (int i = 0; i < n; i++) md += cur[i] - prev[i];
  md /= n;
  float *d = a->tmp2;
  for (int i = 0; i < n; i++) d[i] = fabsf(cur[i] - prev[i] - md) / span;
  const float med = percentile(d, n, 0.5f, 0, 0.25f, a->hist);
  a->noise += (med - a->noise) * (first ? 1 : 0.05f);
  const float thr = fmaxf(0.03f, a->noise * 3);
  float *fresh = a->fresh;
  for (int i = 0; i < n; i++) fresh[i] = clamp01((d[i] - thr) / 0.12f);
  box_blur(fresh, fresh, tmp, w, h, 1);
  const float fastK = powf(0.55f, dt * 30), slowK = powf(0.93f, dt * 30);
  float e = 0, sx = 0, sy = 0;
  int hotN = 0;
  for (int y = 0, i = 0; y < h; y++) {
    for (int x = 0; x < w; x++, i++) {
      const float m = fresh[i];
      a->motion[i] = fmaxf(m, a->motion[i] * fastK);
      a->trail[i] = fmaxf(m, a->trail[i] * slowK);
      if (m > 0.05f) { e += m; sx += m * x; sy += m * y; }
      if (m > 0.35f) a->hot[hotN++] = i;
    }
  }
  a->hotN = hotN;
  a->energy = e / n;
  if (e > 0.5f) {
    const float kc = 1 - expf(-dt * 6);
    a->cx += (sx / e / (w - 1) - a->cx) * kc;
    a->cy += (sy / e / (h - 1) - a->cy) * kc;
  }

  // Level: normalised, blurred hard enough to lose all detail, and smoothed over time.
  float *t2 = a->tmp2;
  for (int i = 0; i < n; i++) t2[i] = clamp01((cur[i] - a->lo) / span);
  box_blur(t2, t2, tmp, w, h, 2);
  box_blur(t2, t2, tmp, w, h, 2);
  const float kl = first ? 1 : 1 - expf(-dt * 8);
  float *L = a->level;
  for (int i = 0; i < n; i++) L[i] += (t2[i] - L[i]) * kl;
  for (int y = 0, i = 0; y < h; y++) {
    for (int x = 0; x < w; x++, i++) {
      a->gx[i] = (L[y * w + mini(x + 1, w - 1)] - L[y * w + maxi(x - 1, 0)]) * 0.5f;
      a->gy[i] = (L[mini(y + 1, h - 1) * w + x] - L[maxi(y - 1, 0) * w + x]) * 0.5f;
    }
  }

  compute_flow(a, md, span, dt, first);
  memcpy(prev, cur, n * sizeof(float));
  compute_zones(a);
}

static int cmpf(const void *x, const void *y) {
  const float a = *(const float *)x, b = *(const float *)y;
  return (a > b) - (a < b);
}

// The background's level, from the frame's left and right edges and top corners (see js/presence.js).
float an_background_level(const Analyzer *a) {
  const int w = a->w, h = a->h;
  static float vals[8192];
  int n = 0;
  const int side = maxi(2, (int)lroundf(w * 0.1f)), corner = (int)lroundf(w * 0.2f);
  for (int y = 0; y < h * 0.85f; y += 2)
    for (int x = 0; x < side; x += 2) {
      if (n + 2 > 8192) break;
      vals[n++] = a->level[y * w + x]; vals[n++] = a->level[y * w + w - 1 - x];
    }
  for (int x = 0; x < corner; x += 2) {
    if (n + 4 > 8192) break;
    vals[n++] = a->level[x]; vals[n++] = a->level[w - 1 - x]; vals[n++] = a->level[w + x]; vals[n++] = a->level[2 * w - 1 - x];
  }
  qsort(vals, n, sizeof(float), cmpf);
  return vals[n >> 1];
}
