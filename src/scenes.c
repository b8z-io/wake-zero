// The scenes, ported from js/scenes/*.js to OpenGL ES 2.0 (GLSL ES 1.00) for the Cardputer's 320x170 screen.
// The shaders follow the browser versions closely; the differences are noted where they are made: no float
// render targets on VideoCore IV, and a much smaller screen (so fewer, larger features).
#include "scenes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int W, H;
int SCREEN_W, SCREEN_H;
static float frand(void) { return (float)rand() / RAND_MAX; }

// ---------------------------------------------------------------------------------------------------- Strata
static const char *STRATA_FS =
  "uniform float uBands, uSpeed;\n"
  "void main() {\n"
  "  vec2 q = vUv * vec2(uAspect, 1.0);\n"
  "  vec2 warp = vec2(vnoise(q * 2.5 + uTime * 0.05), vnoise(q * 2.5 + 7.3 - uTime * 0.04)) - 0.5;\n"
  "  vec2 c = camUV(vUv + warp * 0.04);\n"
  "  vec4 cam = camBicubic(c);\n"
  "  float lvl = cam.r, heat = clamp(cam.b * 1.3, 0.0, 1.0);\n"
  "  float n = fbm(q * 1.1 + vec2(uTime * 0.02, -uTime * 0.015));\n"
  "  float f = lvl * 0.9 + n * 0.5 + cam.b * 0.15;\n"
  "  float x = f * uBands - uTime * uSpeed;\n"
  "  float fr = fract(x), w = fwidth(x);\n"
  "  float d = min(fr, 1.0 - fr);\n"
  "  float line = 1.0 - smoothstep(0.4 * w, 1.6 * w, d);\n"
  "  float k = floor(x);\n"
  "  vec3 base = palette(uHue + k * 0.05);\n"
  "  vec3 hot = palette(uHue + 0.45 + cam.b * 0.25);\n"
  "  vec3 col = base * (0.06 + 0.14 * fr);\n"
  "  col += line * mix(base * 0.85 + 0.08, hot * 1.3 + 0.25, heat) * (0.55 + heat);\n"
  "  col += hot * heat * 0.16 * (0.5 + 0.5 * fr);\n"
  "  gl_FragColor = vec4(col, 1.0);\n"
  "}\n";

static Prog strata;
static void strata_init(void) { strata = gfx_program("strata", STRATA_FS); }
static void strata_step(float dt, float t, const Analyzer *an) { (void)dt; (void)t; (void)an; }
static void strata_draw(const Target *out) {
  gfx_use(strata);
  u1f(strata, "uBands", 12);   // browser: 16; fewer, so the lines stay apart at 170 px
  u1f(strata, "uSpeed", 0.12f);
  gfx_draw(out);
}

// ---------------------------------------------------------------------------------------------------- Lattice
static const char *LATTICE_FS =
  "uniform float uCells; uniform vec2 uRes;\n"
  "void main() {\n"
  "  vec2 p = vUv * vec2(uAspect, 1.0) * uCells;\n"
  "  vec2 s = vec2(1.0, 1.7320508);\n"
  "  vec4 hc = floor(vec4(p, p - vec2(0.5, 1.0)) / s.xyxy) + 0.5;\n"
  "  vec4 h = vec4(p - hc.xy * s, p - (hc.zw + 0.5) * s);\n"
  "  vec4 cell = dot(h.xy, h.xy) < dot(h.zw, h.zw) ? vec4(h.xy, hc.xy) : vec4(h.zw, hc.zw + 0.5);\n"
  "  vec2 centre = cell.zw * s;\n"
  "  vec2 cuv = centre / uCells / vec2(uAspect, 1.0);\n"
  "  vec2 c = camUV(cuv);\n"
  "  vec4 cam = camAt(c);\n"
  "  vec2 f = camFlow(c) * vec2(uAspect, 1.0) * uCells;\n"
  "  vec2 off = f * 0.08;\n"
  "  float ol = length(off);\n"
  "  if (ol > 0.45) off *= 0.45 / ol;\n"
  "  float lvl = smoothstep(0.08, 0.95, cam.r), trail = cam.b;\n"
  "  vec2 cc = centre / uCells;\n"
  "  float n = fbm(cc * 1.7 + vec2(uTime * 0.05, -uTime * 0.04));\n"
  "  float wave = 0.5 + 0.5 * sin(length(cc - vec2(uAspect * 0.5, 0.5)) * 14.0 - uTime * 1.1 + n * 6.0);\n"
  "  float r = 0.05 + 0.16 * n * wave + 0.06 * lvl + 0.34 * trail;\n"
  "  float d = length(cell.xy - off);\n"
  "  float aa = uCells / uRes.y * 1.5;\n"
  "  float dotA = 1.0 - smoothstep(r - aa, r + aa, d);\n"
  "  float ring = (1.0 - smoothstep(0.0, aa * 1.5, abs(d - r - 0.07))) * smoothstep(0.1, 0.6, trail);\n"
  "  vec3 col = palette(uHue + lvl * 0.45 + n * 0.3 + trail * 0.3 + cell.w * 0.003);\n"
  "  vec3 c2 = col * dotA * (0.22 + 0.35 * wave * n + 1.1 * trail) + palette(uHue + 0.5) * ring * 0.7;\n"
  "  gl_FragColor = vec4(c2 + vec3(0.008, 0.008, 0.014), 1.0);\n"
  "}\n";

static Prog lattice;
static void lattice_init(void) { lattice = gfx_program("lattice", LATTICE_FS); }
static void lattice_step(float dt, float t, const Analyzer *an) { (void)dt; (void)t; (void)an; }
static void lattice_draw(const Target *out) {
  gfx_use(lattice);
  u1f(lattice, "uCells", 13);   // browser: 30 across a 1080 px screen; 13 keeps each dot over 10 px here
  u2f(lattice, "uRes", W, H);
  gfx_draw(out);
}

// ---------------------------------------------------------------------------------------------------- Echo
// The feedback buffer holds colour at half scale, so light can build up to 2 before it clips in 8 bits.
static const char *ECHO_FEED =
  "uniform sampler2D uSrc; uniform vec2 uCentre; uniform float uZoom, uTurn, uKeep, uAdd, uShift;\n"
  "void main() {\n"
  "  vec2 a = vec2(uAspect, 1.0);\n"
  "  vec2 d = (vUv - uCentre) * a;\n"
  "  float cs = cos(uTurn), sn = sin(uTurn);\n"
  "  vec2 s = uCentre + mat2(cs, sn, -sn, cs) * d * uZoom / a;\n"
  "  vec3 c = vec3(0.0);\n"
  "  if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0) c = texture2D(uSrc, s).rgb * 2.0 * uKeep;\n"
  "  c = mix(c, c.gbr, uShift);\n"
  "  vec4 cam = camAt(camUV(vUv));\n"
  "  float ang = atan(d.y, d.x) / 6.2831853;\n"
  "  c += palette(uHue + ang + uTime * 0.03) * cam.g * uAdd;\n"
  "  vec2 lp = (vUv - uCentre - 0.28 * vec2(cos(uTime * 0.4), sin(uTime * 0.53))) * a;\n"
  "  c += palette(uHue + 0.5) * exp(-dot(lp, lp) / 0.0004) * uAdd * 0.08;\n"
  "  gl_FragColor = vec4(dither8(c * 0.5), 1.0);\n"
  "}\n";
static const char *ECHO_SHOW =
  "uniform sampler2D uSrc;\n"
  "void main() { vec3 c = texture2D(uSrc, vUv).rgb * 2.0; gl_FragColor = vec4(1.0 - exp(-c * 1.6) + vec3(0.004, 0.004, 0.008), 1.0); }\n";

static Prog echo_feed, echo_show;
static PingPong echo_f;
static float echo_centre[2] = { 0.5f, 0.5f }, echo_spin = 0.15f;
static void echo_init(void) {
  echo_feed = gfx_program("echo feed", ECHO_FEED);
  echo_show = gfx_program("echo show", ECHO_SHOW);
  echo_f = gfx_pingpong(W, H, GL_LINEAR);
}
static void echo_step(float dt, float t, const Analyzer *an) {
  (void)t;
  float fx = 0, m = 0, tx = 0.5f, ty = 0.5f;
  if (an) {
    for (int k = 0; k < AN_COLS * AN_ROWS; k++) { fx += an->zFlowX[k] * an->zMotion[k]; m += an->zMotion[k]; }
    if (m > 0.05f) { fx /= m; tx = 0.5f + (an->cx - 0.5f) * 0.5f; ty = 0.5f - (an->cy - 0.5f) * 0.5f; }
  }
  const float k = 1 - expf(-dt * 0.8f);
  echo_centre[0] += (tx - echo_centre[0]) * k;
  echo_centre[1] += (ty - echo_centre[1]) * k;
  const float target = 0.15f + fmaxf(-1.2f, fminf(1.2f, fx * 3));
  echo_spin += (target - echo_spin) * (1 - expf(-dt * 1.5f));
  gfx_use(echo_feed);
  utex(echo_feed, "uSrc", echo_f.a.tex);
  u2f(echo_feed, "uCentre", echo_centre[0], echo_centre[1]);
  u1f(echo_feed, "uZoom", expf(dt * 0.9f));
  u1f(echo_feed, "uTurn", echo_spin * dt);
  u1f(echo_feed, "uKeep", expf(-dt * 0.55f));
  u1f(echo_feed, "uAdd", dt * 9);
  u1f(echo_feed, "uShift", fminf(1, dt * 0.6f));
  gfx_draw(&echo_f.b);
  pp_swap(&echo_f);
}
static void echo_draw(const Target *out) {
  gfx_use(echo_show);
  utex(echo_show, "uSrc", echo_f.a.tex);
  gfx_draw(out);
}

// ---------------------------------------------------------------------------------------------------- Ripple
// Heights need more than 8 bits, so each one is packed into two channels as 16-bit fixed point (range +-4):
// rg = height now, ba = height one step ago. Linear filtering stays correct because unpacking is linear.
static const char *RIPPLE_PACK =
  "vec2 pack16(float h) { float x = clamp(h * 0.125 + 0.5, 0.0, 1.0) * 255.0; float hi = floor(x); return vec2(hi / 255.0, x - hi); }\n"
  "float unpack16(vec2 e) { return ((e.x * 255.0 + e.y) / 255.0 - 0.5) * 8.0; }\n"
  // Exact at texel centres (where the simulation samples). Texture filtering rounds to 8 bits, which would
  // wreck the packed low byte between texels, so in between H() blends four centres itself, after unpacking.
  "float Hc(vec2 p) { return unpack16(texture2D(uS, p).rg); }\n"
  "float H(vec2 p) {\n"
  "  vec2 x = p / uTexel - 0.5, i = floor(x), f = x - i;\n"
  "  vec2 c = (i + 0.5) * uTexel;\n"
  "  return mix(mix(Hc(c), Hc(c + vec2(uTexel.x, 0.0)), f.x), mix(Hc(c + vec2(0.0, uTexel.y)), Hc(c + uTexel), f.x), f.y);\n"
  "}\n";
static const char *RIPPLE_SIM =
  "uniform sampler2D uS; uniform vec2 uTexel; uniform float uDamp, uPush; uniform vec3 uRain;\n"
  "%s"
  "void main() {\n"
  "  vec4 e = texture2D(uS, vUv);\n"
  "  float now = unpack16(e.rg), before = unpack16(e.ba);\n"
  "  float L = Hc(vUv - vec2(uTexel.x, 0.0)), R = Hc(vUv + vec2(uTexel.x, 0.0));\n"
  "  float T = Hc(vUv + vec2(0.0, uTexel.y)), B = Hc(vUv - vec2(0.0, uTexel.y));\n"
  "  float h = ((L + R + T + B) * 0.5 - before) * uDamp;\n"
  // A little viscosity: pulls each point towards its neighbours, which damps the grid-scale (checkerboard)
  // waves hardest. The browser's 400-row grid hides them; at 85 rows, scaled up 2x, they show.
  "  h = mix(h, (L + R + T + B) * 0.25, 0.025);\n"
  "  h -= camAt(camUV(vUv)).g * uPush;\n"
  "  vec2 q = (vUv - uRain.xy) * vec2(uAspect, 1.0);\n"
  "  h -= uRain.z * exp(-dot(q, q) / 0.0004);\n"
  "  gl_FragColor = vec4(pack16(h), pack16(now));\n"
  "}\n";
static const char *RIPPLE_SHOW =
  "uniform sampler2D uS; uniform vec2 uTexel;\n"
  "%s"
  "void main() {\n"
  "  float h = H(vUv);\n"
  "  float l = H(vUv - vec2(uTexel.x, 0.0)), r = H(vUv + vec2(uTexel.x, 0.0));\n"
  "  float b = H(vUv - vec2(0.0, uTexel.y)), t = H(vUv + vec2(0.0, uTexel.y));\n"
  "  vec3 n = normalize(vec3(l - r, b - t, 0.1));\n"
  "  vec2 fq = vUv * vec2(uAspect, 1.0);\n"
  "  n.xy += (vec2(vnoise(fq * 25.0 + uTime * vec2(0.6, 0.4)), vnoise(fq * 25.0 + 7.3 - uTime * vec2(0.5, 0.7))) - 0.5) * 0.05;\n"
  "  n = normalize(n);\n"
  "  float caus = clamp((l + r + b + t - 4.0 * h) * -13.0, 0.0, 3.0);\n"
  "  vec2 uv = vUv + n.xy * 0.05;\n"
  "  vec2 q = uv * vec2(uAspect, 1.0);\n"
  "  float f = fbm(q * 2.2 + vec2(uTime * 0.02, -uTime * 0.017));\n"
  "  float lvl = smoothstep(0.2, 1.0, camBicubic(camUV(uv)).r);\n"
  "  vec3 floor_ = palette(uHue + 0.55 + f * 0.3) * (0.1 + 0.16 * f) + palette(uHue + 0.2 + lvl * 0.2) * lvl * 0.1;\n"
  "  float spec = pow(max(dot(n, normalize(vec3(-0.45, 0.55, 0.7))), 0.0), 80.0);\n"
  "  vec3 col = floor_ * (0.8 + caus) + palette(uHue + 0.1) * caus * 0.25 + palette(uHue + 0.35) * spec * 1.2;\n"
  "  gl_FragColor = vec4(col + vec3(0.004, 0.006, 0.012), 1.0);\n"
  "}\n";

static Prog ripple_sim, ripple_show;
static PingPong ripple_s;
static float ripple_next_rain, ripple_texel[2];
static char *fmt_with(const char *fmt, const char *arg) {
  size_t n = strlen(fmt) + strlen(arg) + 1;
  char *s = malloc(n);
  snprintf(s, n, fmt, arg);
  return s;
}
static void ripple_init(void) {
  char *a = fmt_with(RIPPLE_SIM, RIPPLE_PACK), *b = fmt_with(RIPPLE_SHOW, RIPPLE_PACK);
  ripple_sim = gfx_program("ripple sim", a);
  ripple_show = gfx_program("ripple show", b);
  free(a); free(b);
  // Half the screen's resolution. The browser runs 400 rows at 120 steps a second; with 85 rows, 30 steps a
  // second moves waves across the screen at about the same speed, and the damping and push are scaled to match.
  const int sh = H / 2, sw = W / 2;
  ripple_s = gfx_pingpong(sw, sh, GL_LINEAR);
  ripple_texel[0] = 1.0f / sw; ripple_texel[1] = 1.0f / sh;
  // Flat water is h = 0, which packs to (127/255, 0.5) in each pair: clear both buffers to that.
  for (int i = 0; i < 2; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, i ? ripple_s.b.fb : ripple_s.a.fb);
    glClearColor(127.0f / 255, 0.5f, 127.0f / 255, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glClearColor(0, 0, 0, 1);
}
static void ripple_step(float dt, float t, const Analyzer *an) {
  (void)an;
  int n = (int)lroundf(dt * 30);
  n = n < 1 ? 1 : n > 2 ? 2 : n;
  for (int i = 0; i < n; i++) {
    float rain[3] = { 0, 0, 0 };
    if (t > ripple_next_rain && i == 0) {
      const int big = frand() < 0.2f;   // mostly small drops, now and then a big one
      rain[0] = 0.05f + 0.9f * frand(); rain[1] = 0.05f + 0.9f * frand();
      rain[2] = big ? 0.5f + frand() * 0.7f : 0.12f + frand() * 0.2f;
      ripple_next_rain = t + (big ? 0.3f : 0.05f) + frand() * (big ? 0.8f : 0.25f);
    }
    gfx_use(ripple_sim);
    utex(ripple_sim, "uS", ripple_s.a.tex);
    u2f(ripple_sim, "uTexel", ripple_texel[0], ripple_texel[1]);
    u1f(ripple_sim, "uDamp", 0.982f);   // browser: 0.994 per step at 120 steps/s; less here, as the viscosity damps too
    u1f(ripple_sim, "uPush", 0.06f);    // 0.015 x 4
    u3f(ripple_sim, "uRain", rain[0], rain[1], rain[2]);
    gfx_draw(&ripple_s.b);
    pp_swap(&ripple_s);
  }
}
static void ripple_draw(const Target *out) {
  gfx_use(ripple_show);
  utex(ripple_show, "uS", ripple_s.a.tex);
  u2f(ripple_show, "uTexel", ripple_texel[0], ripple_texel[1]);
  gfx_draw(out);
}

// ---------------------------------------------------------------------------------------------------- Thermal
static const char *THERMAL_HEAT =
  "uniform sampler2D uSrc; uniform float uCool; uniform vec2 uPx;\n"
  "void main() {\n"
  "  float lvl = 0.0, mo = 0.0;\n"
  "  for (int i = 0; i < 9; i++) {\n"
  "    float a = float(i) * 0.6981;\n"
  "    vec2 off = i == 0 ? vec2(0.0) : vec2(cos(a), sin(a)) * vec2(0.045 / uAspect, 0.045);\n"
  "    vec4 c = camAt(camUV(vUv + off));\n"
  "    lvl += c.r; mo += c.g;\n"
  "  }\n"
  "  lvl /= 9.0; mo /= 9.0;\n"
  "  float warm = smoothstep(0.3, 0.95, lvl), hot = smoothstep(0.02, 0.3, mo);\n"
  "  vec2 b = vUv - vec2(0.0, uPx.y * 0.6);\n"
  "  float prev = texture2D(uSrc, b).r * 0.8 + (texture2D(uSrc, b + vec2(uPx.x, 0.0)).r + texture2D(uSrc, b - vec2(uPx.x, 0.0)).r\n"
  "    + texture2D(uSrc, b + vec2(0.0, uPx.y)).r + texture2D(uSrc, b - vec2(0.0, uPx.y)).r) * 0.05;\n"
  "  gl_FragColor = vec4(dither8(vec3(max(hot, prev * uCool), warm, 0.0)), 1.0);\n"
  "}\n";
static const char *THERMAL_SHOW =
  "uniform sampler2D uSrc; uniform vec2 uRes;\n"
  "vec3 iron(float t) {\n"
  "  t = clamp(t, 0.0, 1.0);\n"
  "  vec3 c = mix(vec3(0.01, 0.0, 0.06), vec3(0.18, 0.02, 0.42), smoothstep(0.0, 0.22, t));\n"
  "  c = mix(c, vec3(0.62, 0.05, 0.42), smoothstep(0.18, 0.42, t));\n"
  "  c = mix(c, vec3(0.95, 0.32, 0.05), smoothstep(0.38, 0.62, t));\n"
  "  c = mix(c, vec3(1.0, 0.78, 0.12), smoothstep(0.58, 0.82, t));\n"
  "  return mix(c, vec3(1.0, 0.98, 0.85), smoothstep(0.8, 1.0, t));\n"
  "}\n"
  "void main() {\n"
  "  vec2 q = vUv * vec2(uAspect, 1.0);\n"
  "  vec4 s = texture2D(uSrc, vUv);\n"
  "  float h = s.g * 0.55 + s.r * 0.6;\n"
  "  float room = 0.05 + 0.07 * fbm(q * 1.8 + vec2(uTime * 0.01, 2.0));\n"
  "  float t = max(h, room) + (hash(floor(vUv * uRes * 0.5) + fract(uTime * 9.1) * 71.0) - 0.5) * 0.025;\n"
  "  vec3 col = iron(t);\n"
  "  float sx = 1.0 - 0.032 * min(1.0, 1.0 / uAspect + 0.4), sy0 = 0.18, sy1 = 0.82;\n"
  "  if (vUv.x > sx && vUv.x < sx + 0.012 / uAspect * min(uAspect, 1.6) && vUv.y > sy0 && vUv.y < sy1) {\n"
  "    float st = (vUv.y - sy0) / (sy1 - sy0);\n"
  "    col = iron(st) * 0.95;\n"
  "    if (abs(fract(st * 4.0 + 0.5) - 0.5) < 0.02) col = vec3(0.85);\n"
  "  }\n"
  "  vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
  "  float box = step(max(abs(p.x), abs(p.y)), 0.045) * step(0.038, max(abs(p.x), abs(p.y)));\n"
  "  box *= step(0.016, min(abs(p.x), abs(p.y)));\n"
  "  col = mix(col, vec3(0.9), box * 0.7);\n"
  "  gl_FragColor = vec4(col, 1.0);\n"
  "}\n";

static Prog thermal_heat, thermal_show;
static PingPong thermal_f;
static void thermal_init(void) {
  thermal_heat = gfx_program("thermal heat", THERMAL_HEAT);
  thermal_show = gfx_program("thermal show", THERMAL_SHOW);
  thermal_f = gfx_pingpong(W / 2, H / 2, GL_LINEAR);
}
static void thermal_step(float dt, float t, const Analyzer *an) {
  (void)t; (void)an;
  gfx_use(thermal_heat);
  utex(thermal_heat, "uSrc", thermal_f.a.tex);
  u1f(thermal_heat, "uCool", expf(-dt * 0.75f));
  u2f(thermal_heat, "uPx", 1.0f / thermal_f.w, 1.0f / thermal_f.h);
  gfx_draw(&thermal_f.b);
  pp_swap(&thermal_f);
}
static void thermal_draw(const Target *out) {
  gfx_use(thermal_show);
  utex(thermal_show, "uSrc", thermal_f.a.tex);
  u2f(thermal_show, "uRes", W, H);
  gfx_draw(out);
}

// ---------------------------------------------------------------------------------------------------- list
Scene SCENES[] = {   // the browser's order, for the scenes ported so far
  { "Ink", ink_init, ink_step, ink_draw },
  { "Strata", strata_init, strata_step, strata_draw },
  { "Lattice", lattice_init, lattice_step, lattice_draw },
  { "Ripple", ripple_init, ripple_step, ripple_draw },
  { "Echo", echo_init, echo_step, echo_draw },
  { "Thermal", thermal_init, thermal_step, thermal_draw },
};
const int NSCENES = sizeof SCENES / sizeof *SCENES;

void scenes_init(int w, int h) {
  W = w; H = h; SCREEN_W = w; SCREEN_H = h;
  for (int i = 0; i < NSCENES; i++) SCENES[i].init();
}

// ---------------------------------------------------------------------------------------------------- composite
static const char *COMPOSITE_FS =
  "uniform sampler2D uA, uB; uniform float uMix, uGrain, uDebug; uniform vec2 uRes;\n"
  "void main() {\n"
  "  vec3 c = mix(texture2D(uB, vUv).rgb, texture2D(uA, vUv).rgb, uMix);\n"
  "  vec2 q = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
  "  c *= 1.0 - 0.35 * smoothstep(0.35, 1.1, length(q));\n"
  "  c += (hash(vUv * uRes + fract(uTime * 7.13) * 91.7) - 0.5) * uGrain;\n"
  "  if (uDebug > 0.5) {\n"   // analysis fields, bottom right: red movement, green level, blue presence, white flow
  "    vec2 r = (vUv - vec2(0.64, 0.03)) / vec2(0.34, 0.34 * uAspect / (uCamScale.x / uCamScale.y));\n"
  "    if (r.x > 0.0 && r.x < 1.0 && r.y > 0.0 && r.y < 1.0) {\n"
  "      vec2 cc = vec2(r.x, 1.0 - r.y);\n"
  "      vec4 cam = camAt(cc);\n"
  "      vec2 f = flowAt(cc);\n"
  "      c = vec3(cam.g, cam.r * 0.5, presenceOf(cam.r)) + vec3(clamp(length(f) * 0.5, 0.0, 1.0)) * 0.5;\n"
  "    }\n"
  "  }\n"
  "  gl_FragColor = vec4(c, 1.0);\n"
  "}\n";
static Prog composite;
void composite_init(void) { composite = gfx_program("composite", COMPOSITE_FS); }
void composite_draw(const Target *a, const Target *b, float mix, int debug, const Target *out) {
  gfx_use(composite);
  utex(composite, "uA", a->tex);
  utex(composite, "uB", b->tex);
  u1f(composite, "uMix", mix);
  u1f(composite, "uGrain", 0.02f);
  u1f(composite, "uDebug", debug ? 1 : 0);
  u2f(composite, "uRes", W, H);
  gfx_draw(out);
}
