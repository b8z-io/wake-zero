// The scenes (see scenes.c) and the final composite: crossfade, vignette, grain and the debug corner.
#pragma once
#include "analysis.h"
#include "gfx.h"

typedef struct {
  const char *title;
  void (*init)(void);
  void (*step)(float dt, float t, const Analyzer *an);   // an is NULL with the camera off
  void (*draw)(const Target *out);
} Scene;

extern Scene SCENES[];
extern int SCREEN_W, SCREEN_H;
// Ink runs its fluid on the CPU (src/ink.c).
void ink_init(void);
void ink_step(float dt, float t, const Analyzer *an);
void ink_draw(const Target *out);
extern const int NSCENES;
void scenes_init(int w, int h);
void composite_init(void);
void composite_draw(const Target *a, const Target *b, float mix, int debug, const Target *out);
