// NDI® sender, through the library's own dynamic-load table (NDIlib_v6_load), so nothing links to it at build time.
// Frames go out asynchronously: NDI compresses on its own thread, while we fill the other of two buffers.
#include "ndi.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_NDI
#include <Processing.NDI.Lib.h>

struct Ndi {
  void *dl;
  const NDIlib_v6 *lib;
  NDIlib_send_instance_t send;
  unsigned char *buf[2];
  size_t cap;
  int which;
};

static void *load(char *err, size_t n) {
  const char *env = getenv("WAKE_NDI_LIB");
  const char *paths[] = { env, "/usr/share/APPLaunch/lib/ndi/libndi.so.6", "libndi.so.6" };
  char why[200] = "";
  for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
    if (!paths[i]) continue;
    void *h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    // Keep the first real reason (the file is there but won't load) over "no such file" for the fallbacks.
    const char *e = dlerror();
    if (e && (!why[0] || strstr(why, "No such file"))) snprintf(why, sizeof why, "%s", e);
  }
  if (!why[0] || strstr(why, "No such file")) snprintf(err, n, "NDI not installed (sudo ./install-ndi.sh)");
  else snprintf(err, n, "NDI won't load: %s", why);
  return NULL;
}

Ndi *ndi_open(const char *name, char *err, size_t n) {
  void *dl = load(err, n);
  if (!dl) return NULL;
  const NDIlib_v6 *(*v6)(void) = (const NDIlib_v6 *(*)(void))dlsym(dl, "NDIlib_v6_load");
  const NDIlib_v6 *lib = v6 ? v6() : NULL;
  if (!lib) { snprintf(err, n, "NDI library too old"); dlclose(dl); return NULL; }
  if (!lib->initialize()) { snprintf(err, n, "NDI won't start on this CPU"); dlclose(dl); return NULL; }
  NDIlib_send_create_t c;
  memset(&c, 0, sizeof c);
  c.p_ndi_name = name;
  c.clock_video = false;   // our own loop keeps the time
  NDIlib_send_instance_t send = lib->send_create(&c);
  if (!send) { snprintf(err, n, "NDI sender failed"); lib->destroy(); dlclose(dl); return NULL; }
  Ndi *s = calloc(1, sizeof *s);
  s->dl = dl; s->lib = lib; s->send = send;
  return s;
}

void ndi_send(Ndi *s, const unsigned char *rgba, int w, int h, int scale, int fps) {
  if (!s) return;
  if (scale < 1) scale = 1;
  const int ow = w * scale, oh = h * scale;
  const size_t need = (size_t)ow * oh * 4;
  if (s->cap < need) {
    s->lib->send_send_video_async_v2(s->send, NULL);   // let NDI finish with the old buffers first
    for (int i = 0; i < 2; i++) { free(s->buf[i]); s->buf[i] = malloc(need); }
    s->cap = need;
  }
  unsigned char *out = s->buf[s->which];
  s->which ^= 1;
  for (int y = 0; y < oh; y++) {   // RGBA bottom-up -> BGRX top-down, each pixel scale x scale
    const unsigned char *src = rgba + 4 * (size_t)(h - 1 - y / scale) * w;
    unsigned char *dst = out + 4 * (size_t)y * ow;
    for (int x = 0; x < ow; x++) {
      const unsigned char *p = src + 4 * (x / scale);
      dst[4 * x] = p[2]; dst[4 * x + 1] = p[1]; dst[4 * x + 2] = p[0]; dst[4 * x + 3] = 255;
    }
  }
  NDIlib_video_frame_v2_t f;
  memset(&f, 0, sizeof f);
  f.xres = ow; f.yres = oh;
  f.FourCC = NDIlib_FourCC_video_type_BGRX;
  f.frame_rate_N = fps * 1000; f.frame_rate_D = 1000;
  f.picture_aspect_ratio = (float)ow / oh;
  f.frame_format_type = NDIlib_frame_format_type_progressive;
  f.timecode = NDIlib_send_timecode_synthesize;
  f.p_data = out;
  f.line_stride_in_bytes = ow * 4;
  s->lib->send_send_video_async_v2(s->send, &f);
}

int ndi_connections(Ndi *s) { return s ? s->lib->send_get_no_connections(s->send, 0) : 0; }

void ndi_close(Ndi *s) {
  if (!s) return;
  s->lib->send_send_video_async_v2(s->send, NULL);   // wait until NDI is done with our buffers
  s->lib->send_destroy(s->send);
  s->lib->destroy();
  dlclose(s->dl);
  free(s->buf[0]); free(s->buf[1]);
  free(s);
}

#else   // built without the NDI headers: say so when asked
Ndi *ndi_open(const char *name, char *err, size_t n) { (void)name; snprintf(err, n, "built without NDI"); return NULL; }
void ndi_send(Ndi *s, const unsigned char *rgba, int w, int h, int scale, int fps) { (void)s; (void)rgba; (void)w; (void)h; (void)scale; (void)fps; }
int ndi_connections(Ndi *s) { (void)s; return 0; }
void ndi_close(Ndi *s) { (void)s; }
#endif
