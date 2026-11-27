// OpenGL ES 2.0 through EGL with no window: GBM on the GPU's render node (the Cardputer's VideoCore IV), or
// Mesa's surfaceless platform elsewhere (a desktop, CI). Everything draws into textures; main.c reads the final
// frame back and sends it to the LCD.
#include "gfx.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Globals G;
static GLuint vbo;
static int unit_next;

// Shared by every fragment shader (the GLSL ES 1.00 version of COMMON in js/gl.js). Camera fields: r level (with
// 8 more bits of it in a), g motion, b trail. Flow: rg, 0.5 = still.
static const char *COMMON =
  "#extension GL_OES_standard_derivatives : enable\n"
  "precision highp float;\n"
  "#ifndef GL_OES_standard_derivatives\n"   // VideoCore IV has it; if a driver didn't, lines get a fixed width
  "float fwidth(float x) { return 0.08; }\n"
  "#endif\n"
  "uniform vec2 uCamScale, uCamRes, uFlowRes;\n"
  "uniform float uTime, uHue, uAspect, uBg;\n"
  "uniform vec3 uPa, uPb, uPc, uPd;\n"
  "uniform sampler2D uCam, uFlow;\n"
  "varying vec2 vUv;\n"
  "#define FLOW_MAX 1.5\n"
  "float presenceOf(float L) { return smoothstep(0.12, 0.4, abs(L - uBg)); }\n"
  "vec2 camUV(vec2 uv) { vec2 c = (uv - 0.5) * uCamScale + 0.5; return vec2(c.x, 1.0 - c.y); }\n"
  "vec4 decodeCam(vec4 c) { c.r += c.a * (1.0 / 255.0); c.a = 1.0; return c; }\n"
  "vec4 camAt(vec2 c) { return decodeCam(texture2D(uCam, c)); }\n"
  "vec2 flowAt(vec2 c) { return (texture2D(uFlow, c).xy * 2.0 - 1.0) * FLOW_MAX; }\n"
  "vec2 camFlow(vec2 c) { return flowAt(c) * vec2(1.0, -1.0) / uCamScale; }\n"
  "vec3 palette(float t) { return max(uPa + uPb * cos(6.2831853 * (uPc * t + uPd)), 0.0); }\n"
  "float hash(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
  // Corner values for value noise. GLSL ES 1.00 has no integers to hash, so this hashes the cell's whole-number
  // coordinates as floats: neighbouring cells pass the same exact values for a shared corner, so they agree.
  "float chash(vec2 i) { vec3 p = fract(vec3(i.xyx) * 0.1031); p += dot(p, p.yzx + 33.33); return fract((p.x + p.y) * p.z); }\n"
  "float vnoise(vec2 p) {\n"
  "  vec2 i = floor(p), f = p - i;\n"
  "  vec2 u = f * f * (3.0 - 2.0 * f);\n"
  "  return mix(mix(chash(i), chash(i + vec2(1.0, 0.0)), u.x), mix(chash(i + vec2(0.0, 1.0)), chash(i + vec2(1.0, 1.0)), u.x), u.y);\n"
  "}\n"
  "float fbm(vec2 p) {\n"
  "  float s = 0.0, a = 0.5;\n"
  "  for (int i = 0; i < 4; i++) { s += a * vnoise(p); p = p * 2.03 + vec2(17.1, 9.2); a *= 0.5; }\n"
  "  return s / 0.9375;\n"
  "}\n"
  "vec4 cubicW(float v) {\n"
  "  vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;\n"
  "  vec4 s = n * n * n;\n"
  "  float x = s.x, y = s.y - 4.0 * s.x, z = s.z - 4.0 * s.y + 6.0 * s.x;\n"
  "  return vec4(x, y, z, 6.0 - x - y - z) * (1.0 / 6.0);\n"
  "}\n"
  // Smooth (cubic B-spline) sampling of the camera fields in four bilinear taps.
  "vec4 camBicubic(vec2 uv) {\n"
  "  vec2 res = uCamRes, inv = 1.0 / res;\n"
  "  uv = uv * res - 0.5;\n"
  "  vec2 f = fract(uv);\n"
  "  uv -= f;\n"
  "  vec4 xc = cubicW(f.x), yc = cubicW(f.y);\n"
  "  vec4 c = uv.xxyy + vec2(-0.5, 1.5).xyxy;\n"
  "  vec4 s = vec4(xc.xz + xc.yw, yc.xz + yc.yw);\n"
  "  vec4 off = (c + vec4(xc.yw, yc.yw) / s) * inv.xxyy;\n"
  "  vec4 s0 = texture2D(uCam, off.xz), s1 = texture2D(uCam, off.yz), s2 = texture2D(uCam, off.xw), s3 = texture2D(uCam, off.yw);\n"
  "  float sx = s.x / (s.x + s.y), sy = s.z / (s.z + s.w);\n"
  "  return decodeCam(mix(mix(s3, s2, sx), mix(s1, s0, sx), sy));\n"
  "}\n"
  // 8-bit render targets only on this GPU: dithered rounding, so feedback that fades a little each frame fades
  // all the way out instead of sticking at a low value.
  "vec3 dither8(vec3 c) { return c + (hash(gl_FragCoord.xy + fract(uTime * 7.13) * 91.7) - 0.5) * (1.0 / 255.0); }\n";

static const char *VS =
  "attribute vec2 aPos;\n"
  "varying vec2 vUv;\n"
  "void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static EGLDisplay dpy;
static EGLContext ctx;

static int try_display(EGLDisplay d) {
  EGLint maj, min;
  if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) return 0;
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
  EGLConfig cfg = NULL;
  EGLint n = 0;
  if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) cfg = NULL;   // EGL_KHR_no_config_context allows none
  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (c == EGL_NO_CONTEXT) { eglTerminate(d); return 0; }
  if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { eglDestroyContext(d, c); eglTerminate(d); return 0; }
  dpy = d; ctx = c;
  return 1;
}

int gfx_init(void) {
  PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
    (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (!getPlatformDisplay) { fprintf(stderr, "wake: EGL has no eglGetPlatformDisplayEXT\n"); return -1; }
  int ok = 0;
  const char *node = getenv("WAKE_DRI");
  int fd = open(node ? node : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (fd >= 0) {
    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm) ok = try_display(getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL));
    if (!ok) close(fd);
  }
  if (!ok) ok = try_display(getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
  if (!ok) { fprintf(stderr, "wake: no EGL display with OpenGL ES 2 (error 0x%x)\n", eglGetError()); return -1; }

  static const float tri[] = { -1, -1, 3, -1, -1, 3 };
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  return 0;
}

const char *gfx_renderer(void) { return (const char *)glGetString(GL_RENDERER); }

static void print_numbered(const char *src) {
  int line = 1;
  fprintf(stderr, "%4d ", line);
  for (const char *p = src; *p; p++) {
    fputc(*p, stderr);
    if (*p == '\n' && p[1]) fprintf(stderr, "%4d ", ++line);
  }
  fputc('\n', stderr);
}

static GLuint compile(GLenum type, const char *src, const char *name) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "wake: shader %s failed to compile:\n%s\n", name, log);
    print_numbered(src);
    exit(1);
  }
  return s;
}

Prog gfx_program(const char *name, const char *fs_body) {
  size_t n = strlen(COMMON) + strlen(fs_body) + 2;
  char *fs = malloc(n);
  snprintf(fs, n, "%s\n%s", COMMON, fs_body);
  GLuint p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, VS, name));
  glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs, name));
  glBindAttribLocation(p, 0, "aPos");
  glLinkProgram(p);
  free(fs);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(p, sizeof log, NULL, log);
    fprintf(stderr, "wake: program %s failed to link:\n%s\n", name, log);
    exit(1);
  }
  return (Prog){ p };
}

GLuint gfx_texture(int w, int h, GLenum filter) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

Target gfx_target(int w, int h, GLenum filter) {
  Target t = { gfx_texture(w, h, filter), 0, w, h };
  glGenFramebuffers(1, &t.fb);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "wake: %dx%d render target is incomplete\n", w, h);
    exit(1);
  }
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  return t;
}

PingPong gfx_pingpong(int w, int h, GLenum filter) {
  return (PingPong){ gfx_target(w, h, filter), gfx_target(w, h, filter), w, h };
}

void pp_swap(PingPong *pp) { Target t = pp->a; pp->a = pp->b; pp->b = t; }

void gfx_free(Target *t) {
  if (t->tex) glDeleteTextures(1, &t->tex);
  if (t->fb) glDeleteFramebuffers(1, &t->fb);
  t->tex = t->fb = 0;
}

void gfx_free_pp(PingPong *pp) { gfx_free(&pp->a); gfx_free(&pp->b); }

static GLint loc(Prog pr, const char *n) { return glGetUniformLocation(pr.p, n); }
void u1f(Prog pr, const char *n, float v) { GLint l = loc(pr, n); if (l >= 0) glUniform1f(l, v); }
void u2f(Prog pr, const char *n, float x, float y) { GLint l = loc(pr, n); if (l >= 0) glUniform2f(l, x, y); }
void u3f(Prog pr, const char *n, float x, float y, float z) { GLint l = loc(pr, n); if (l >= 0) glUniform3f(l, x, y, z); }

void utex(Prog pr, const char *n, GLuint tex) {
  GLint l = loc(pr, n);
  if (l < 0) return;
  glActiveTexture(GL_TEXTURE0 + unit_next);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform1i(l, unit_next++);
}

void gfx_use(Prog pr) {
  glUseProgram(pr.p);
  unit_next = 0;
  u1f(pr, "uTime", G.time); u1f(pr, "uHue", G.hue); u1f(pr, "uAspect", G.aspect); u1f(pr, "uBg", G.bg);
  u2f(pr, "uCamScale", G.camScale[0], G.camScale[1]);
  u2f(pr, "uCamRes", G.camRes[0], G.camRes[1]);
  u2f(pr, "uFlowRes", G.flowRes[0], G.flowRes[1]);
  u3f(pr, "uPa", G.pa[0], G.pa[1], G.pa[2]); u3f(pr, "uPb", G.pb[0], G.pb[1], G.pb[2]);
  u3f(pr, "uPc", G.pc[0], G.pc[1], G.pc[2]); u3f(pr, "uPd", G.pd[0], G.pd[1], G.pd[2]);
  utex(pr, "uCam", G.cam);
  utex(pr, "uFlow", G.flow);
}

void gfx_draw(const Target *t) {
  glBindFramebuffer(GL_FRAMEBUFFER, t->fb);
  glViewport(0, 0, t->w, t->h);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

void gfx_read(const Target *t, unsigned char *rgba) {
  glBindFramebuffer(GL_FRAMEBUFFER, t->fb);
  glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}
