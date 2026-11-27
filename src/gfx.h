// Tiny OpenGL ES 2.0 toolkit, the counterpart of js/gl.js: an off-screen EGL context, full-screen passes, render
// targets and ping-pong pairs, and the globals every shader shares (palette, camera fields, time).
#pragma once
#include <GLES2/gl2.h>

typedef struct { GLuint tex, fb; int w, h; } Target;
typedef struct { Target a, b; int w, h; } PingPong;   // read = a, write = b
typedef struct { GLuint p; } Prog;

typedef struct {
  float time, hue, aspect, bg;
  float camScale[2], camRes[2], flowRes[2];
  float pa[3], pb[3], pc[3], pd[3];
  GLuint cam, flow;
} Globals;

extern Globals G;

int gfx_init(void);                     // 0 on success
const char *gfx_renderer(void);
Prog gfx_program(const char *name, const char *fs_body);
Target gfx_target(int w, int h, GLenum filter);
PingPong gfx_pingpong(int w, int h, GLenum filter);
void pp_swap(PingPong *pp);
void gfx_free(Target *t);
void gfx_free_pp(PingPong *pp);
GLuint gfx_texture(int w, int h, GLenum filter);

// One full-screen pass: use(), set uniforms, draw().
void gfx_use(Prog pr);                  // binds the program and sets the globals
void u1f(Prog pr, const char *n, float v);
void u2f(Prog pr, const char *n, float x, float y);
void u3f(Prog pr, const char *n, float x, float y, float z);
void utex(Prog pr, const char *n, GLuint tex);   // samplers get units in order after the globals'
void gfx_draw(const Target *t);         // NULL is not allowed: everything renders off-screen

void gfx_read(const Target *t, unsigned char *rgba);   // rows bottom first, as GL gives them
