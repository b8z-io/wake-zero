// Ink: a fluid simulation (stable fluids, with vorticity). Movement in front of the camera drags the fluid along
// with it and pours in coloured ink, hue set by the direction of the movement. The same steps as Wake's ink.js,
// but the browser runs them on the GPU in half-float textures, which VideoCore IV can't render to; here they run
// on the CPU in floats (128x68 for the velocity, the full 320x170 for the ink) and the GPU only shades the result.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "scenes.h"

#define SW 128
#define SH 68
#define TAU 6.2831853f

static int DW, DH;
static float *vx, *vy, *tx, *ty, *curl, *dv, *pr, *pr2;   // velocity in sim cells per second; row 0 at the bottom
static float *dye, *dye2;                                  // ink, rgb, at screen size
static unsigned char *up;
static float tonemap[2048];
static GLuint tex;
static Prog show;

static const char *SHOW =
  "uniform sampler2D uDye; uniform vec2 uTexel;\n"
  "float lum(vec2 p) { return dot(texture2D(uDye, p).rgb, vec3(0.333)); }\n"
  "void main() {\n"
  "  vec3 c = texture2D(uDye, vUv).rgb;\n"
  "  float dx = lum(vUv + vec2(uTexel.x, 0.0)) - lum(vUv - vec2(uTexel.x, 0.0));\n"
  "  float dy = lum(vUv + vec2(0.0, uTexel.y)) - lum(vUv - vec2(0.0, uTexel.y));\n"
  "  vec3 n = normalize(vec3(-dx, -dy, 0.05));\n"   // the ink arrives tone-mapped, so its slopes are gentler
  "  float shade = 0.72 + 0.4 * clamp(dot(n, normalize(vec3(-0.4, 0.6, 0.7))), 0.0, 1.0);\n"
  "  gl_FragColor = vec4(c * shade + vec3(0.006, 0.007, 0.012), 1.0);\n"
  "}\n";

// ---------------------------------------------------------------------------------------------------- helpers
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float fractf(float v) { return v - floorf(v); }
static inline float smooth01(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

static float chash(float x, float y) {   // the shaders' chash()
  float p0 = fractf(x * 0.1031f), p1 = fractf(y * 0.1031f), p2 = fractf(x * 0.1031f);
  const float d = p0 * (p1 + 33.33f) + p1 * (p2 + 33.33f) + p2 * (p0 + 33.33f);
  p0 += d; p1 += d; p2 += d;
  return fractf((p0 + p1) * p2);
}
static float vnoise(float x, float y) {
  const float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
  const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
  const float a = chash(ix, iy), b = chash(ix + 1, iy), c = chash(ix, iy + 1), d = chash(ix + 1, iy + 1);
  return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}
static float fbm(float x, float y) {
  float s = 0, a = 0.5f;
  for (int i = 0; i < 4; i++) { s += a * vnoise(x, y); x = x * 2.03f + 17.1f; y = y * 2.03f + 9.2f; a *= 0.5f; }
  return s / 0.9375f;
}
static void palette(float t, float *rgb) {
  for (int k = 0; k < 3; k++) rgb[k] = fmaxf(G.pa[k] + G.pb[k] * cosf(TAU * (G.pc[k] * t + G.pd[k])), 0);
}

// Bilinear sample of a w x h field at (x, y) in cell coordinates (cell centres at whole numbers), clamped.
static inline float bilerp(const float *f, int w, int h, float x, float y) {
  x = clampf(x, 0, w - 1.001f); y = clampf(y, 0, h - 1.001f);
  const int ix = (int)x, iy = (int)y;
  const float fx = x - ix, fy = y - iy;
  const float *r0 = f + iy * w + ix, *r1 = r0 + w;
  return (r0[0] + (r0[1] - r0[0]) * fx) * (1 - fy) + (r1[0] + (r1[1] - r1[0]) * fx) * fy;
}

// Camera fields at a screen uv (y up), as camUV() and camFlow() in the shaders. Fields are row 0 at the top.
typedef struct { float motion, level, fx, fy, sfx, sfy; } CamAt;   // f: image units/s (y down); sf: screen uv/s
static CamAt cam_at(const Analyzer *an, float u, float v) {
  CamAt r = { 0 };
  if (!an) return r;
  const float cx = (u - 0.5f) * G.camScale[0] + 0.5f, cyTop = 1 - ((v - 0.5f) * G.camScale[1] + 0.5f);
  r.motion = bilerp(an->motion, an->w, an->h, cx * an->w - 0.5f, cyTop * an->h - 0.5f);
  r.level = bilerp(an->level, an->w, an->h, cx * an->w - 0.5f, cyTop * an->h - 0.5f);
  // flow is interleaved x,y at half size: sample each component
  const float fx = cx * an->fw - 0.5f, fy = cyTop * an->fh - 0.5f;
  const float X = clampf(fx, 0, an->fw - 1.001f), Y = clampf(fy, 0, an->fh - 1.001f);
  const int ix = (int)X, iy = (int)Y;
  const float ax = X - ix, ay = Y - iy;
  for (int k = 0; k < 2; k++) {
    const float *f = an->flow + k;
    const int w = an->fw, i0 = (iy * w + ix) * 2, i1 = i0 + w * 2;
    const float val = (f[i0] + (f[i0 + 2] - f[i0]) * ax) * (1 - ay) + (f[i1] + (f[i1 + 2] - f[i1]) * ax) * ay;
    if (k == 0) r.fx = val; else r.fy = val;
  }
  r.sfx = r.fx / G.camScale[0];
  r.sfy = -r.fy / G.camScale[1];
  return r;
}

// ---------------------------------------------------------------------------------------------------- simulation
#define AT(f, x, y) f[(y) * SW + (x)]
static inline int cl(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

void ink_init(void) {
  DW = SCREEN_W; DH = SCREEN_H;
  float **sim[] = { &vx, &vy, &tx, &ty, &curl, &dv, &pr, &pr2 };
  for (size_t i = 0; i < sizeof sim / sizeof *sim; i++) *sim[i] = calloc(SW * SH, sizeof(float));
  dye = calloc(DW * DH * 3, sizeof(float));
  dye2 = calloc(DW * DH * 3, sizeof(float));
  up = calloc(DW * DH, 4);
  for (int i = 0; i < 2048; i++) tonemap[i] = 1 - expf(-(i / 256.0f) * 1.4f);   // browser: uExposure 1.4
  tex = gfx_texture(DW, DH, GL_LINEAR);
  show = gfx_program("ink show", SHOW);
}

void ink_step(float dt, float t, const Analyzer *an) {
  const float aspect = (float)DW / DH;

  // The ambient swirl is slow, smooth noise: work it out on a coarse grid and interpolate.
  enum { AW = 33, AH = 18 };
  static float amb[AW * AH];
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < AH; y++)
    for (int x = 0; x < AW; x++) {
      const float u = (float)x / (AW - 1), v = (float)y / (AH - 1);
      amb[y * AW + x] = fbm(u * aspect * 2.5f + t * 0.05f, v * 2.5f + t * 0.05f) * 12.566f;
    }

  // Force: movement drags the fluid towards (a multiple of) its own velocity; slow noise keeps it alive.
  const float gain = 1.6f, ambient = 18;
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      const float u = (x + 0.5f) / SW, v = (y + 0.5f) / SH;
      float *px = &AT(vx, x, y), *py = &AT(vy, x, y);
      if (an) {
        const CamAt c = cam_at(an, u, v);
        const float w = c.motion * smooth01(0, 0.04f, hypotf(c.sfx, c.sfy));
        if (w > 0) {
          const float k = clampf(w * dt * 14, 0, 1);
          *px += (c.sfx * SW * gain - *px) * k;
          *py += (c.sfy * SH * gain - *py) * k;
        }
      }
      const float a = bilerp(amb, AW, AH, u * (AW - 1), v * (AH - 1));
      *px += cosf(a) * ambient * dt;
      *py += sinf(a) * ambient * dt;
    }

  // Vorticity confinement: put back the swirl the coarse grid smooths away.
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++)
      AT(curl, x, y) = 0.5f * (AT(vy, cl(x + 1, SW - 1), y) - AT(vy, cl(x - 1, SW - 1), y)
                               - AT(vx, x, cl(y + 1, SH - 1)) + AT(vx, x, cl(y - 1, SH - 1)));
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      const float L = AT(curl, cl(x - 1, SW - 1), y), R = AT(curl, cl(x + 1, SW - 1), y);
      const float T = AT(curl, x, cl(y + 1, SH - 1)), B = AT(curl, x, cl(y - 1, SH - 1)), C = AT(curl, x, y);
      float fx = 0.5f * (fabsf(T) - fabsf(B)), fy = 0.5f * (fabsf(R) - fabsf(L));
      const float len = hypotf(fx, fy) + 1e-4f;
      fx = fx / len * 20 * C; fy = -(fy / len * 20 * C);
      AT(vx, x, y) = clampf(AT(vx, x, y) + fx * dt, -1500, 1500);
      AT(vy, x, y) = clampf(AT(vy, x, y) + fy * dt, -1500, 1500);
    }

  // Projection: divergence, pressure (Jacobi, warm-started), subtract the gradient.
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      const float Cx = AT(vx, x, y), Cy = AT(vy, x, y);
      const float L = x > 0 ? AT(vx, x - 1, y) : -Cx, R = x < SW - 1 ? AT(vx, x + 1, y) : -Cx;
      const float T = y < SH - 1 ? AT(vy, x, y + 1) : -Cy, B = y > 0 ? AT(vy, x, y - 1) : -Cy;
      AT(dv, x, y) = 0.5f * (R - L + T - B);
    }
  for (int i = 0; i < SW * SH; i++) pr[i] *= 0.8f;
  for (int it = 0; it < 20; it++) {
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < SH; y++)
      for (int x = 0; x < SW; x++)
        AT(pr2, x, y) = (AT(pr, cl(x - 1, SW - 1), y) + AT(pr, cl(x + 1, SW - 1), y) + AT(pr, x, cl(y + 1, SH - 1))
                         + AT(pr, x, cl(y - 1, SH - 1)) - AT(dv, x, y)) * 0.25f;
    float *s = pr; pr = pr2; pr2 = s;
  }
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      AT(vx, x, y) -= AT(pr, cl(x + 1, SW - 1), y) - AT(pr, cl(x - 1, SW - 1), y);
      AT(vy, x, y) -= AT(pr, x, cl(y + 1, SH - 1)) - AT(pr, x, cl(y - 1, SH - 1));
    }

  // Advect the velocity through itself.
  const float kv = 1 / (1 + 0.12f * dt);
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      const float sx = x - dt * AT(vx, x, y), sy = y - dt * AT(vy, x, y);
      AT(tx, x, y) = bilerp(vx, SW, SH, sx, sy) * kv;
      AT(ty, x, y) = bilerp(vy, SW, SH, sx, sy) * kv;
    }
  { float *s = vx; vx = tx; tx = s; s = vy; vy = ty; ty = s; }

  // Ink: poured in where things move, coloured by the direction of the movement.
  const float amount = 7 * dt;
  if (an) {
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < DH; y++)
      for (int x = 0; x < DW; x++) {
        const CamAt c = cam_at(an, (x + 0.5f) / DW, (y + 0.5f) / DH);
        if (c.motion < 0.002f) continue;
        float rgb[3];
        palette(G.hue + atan2f(-c.fy, c.fx) / TAU * 0.4f + c.level * 0.25f, rgb);
        float *d = dye + 3 * (y * DW + x);
        for (int k = 0; k < 3; k++) d[k] += rgb[k] * c.motion * amount;
      }
  }
  // three faint wandering sources, so there is always something drifting
  for (int s = 0; s < 3; s++) {
    const float k = s;
    const float px = 0.5f + 0.38f * sinf(t * (0.07f + 0.02f * k) + k * 2.1f), py = 0.5f + 0.36f * cosf(t * (0.05f + 0.017f * k) + k * 1.3f);
    float rgb[3];
    palette(G.hue + k * 0.33f + t * 0.01f, rgb);
    const int r = (int)(0.11f * DH) + 1, cx = (int)(px * DW), cy = (int)(py * DH);
    for (int y = cy - r; y <= cy + r; y++) {
      if (y < 0 || y >= DH) continue;
      for (int x = cx - r; x <= cx + r; x++) {
        if (x < 0 || x >= DW) continue;
        const float qx = ((x + 0.5f) / DW - px) * aspect, qy = (y + 0.5f) / DH - py;
        const float g = expf(-(qx * qx + qy * qy) / 0.0012f) * 0.7f * dt;
        float *d = dye + 3 * (y * DW + x);
        for (int c = 0; c < 3; c++) d[c] += rgb[c] * g;
      }
    }
  }

  // Advect the ink along the velocity (sampled at each ink pixel), fading slowly.
  const float kd = 1 / (1 + 0.55f * dt), toSimX = (float)SW / DW, toSimY = (float)SH / DH;
  #pragma omp parallel for schedule(static)
  for (int y = 0; y < DH; y++)
    for (int x = 0; x < DW; x++) {
      const float gx = (x + 0.5f) * toSimX - 0.5f, gy = (y + 0.5f) * toSimY - 0.5f;
      const float sx = x - dt * bilerp(vx, SW, SH, gx, gy) / toSimX, sy = y - dt * bilerp(vy, SW, SH, gx, gy) / toSimY;
      const float X = clampf(sx, 0, DW - 1.001f), Y = clampf(sy, 0, DH - 1.001f);
      const int ix = (int)X, iy = (int)Y;
      const float fx = X - ix, fy = Y - iy;
      const float *a = dye + 3 * (iy * DW + ix), *b = a + 3 * DW;
      float *o = dye2 + 3 * (y * DW + x);
      for (int c = 0; c < 3; c++)
        o[c] = ((a[c] + (a[c + 3] - a[c]) * fx) * (1 - fy) + (b[c] + (b[c + 3] - b[c]) * fx) * fy) * kd;
    }
  { float *s = dye; dye = dye2; dye2 = s; }
}

void ink_draw(const Target *out) {
  // Tone-mapped (the browser's 1 - exp(-c * 1.4)) and dithered into 8 bits, rows bottom first like the GL texture.
  static unsigned seed = 1;
  for (int i = 0, n = DW * DH; i < n; i++)
    for (int c = 0; c < 3; c++) {
      seed = seed * 1664525u + 1013904223u;
      const float v = dye[3 * i + c];
      const int idx = v <= 0 ? 0 : v >= 7.99f ? 2047 : (int)(v * 256);
      const float o = tonemap[idx] * 255 + ((seed >> 8) & 255) / 256.0f;
      up[4 * i + c] = (unsigned char)(o > 255 ? 255 : o);
    }
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DW, DH, GL_RGBA, GL_UNSIGNED_BYTE, up);
  gfx_use(show);
  utex(show, "uDye", tex);
  u2f(show, "uTexel", 1.0f / DW, 1.0f / DH);
  gfx_draw(out);
}
