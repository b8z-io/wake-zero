// Wake Zero: Wake for the M5Stack CardputerZero, the browser version's app.js as a native program. Renders
// off-screen with OpenGL ES 2.0 on the VideoCore IV, reads each frame back and writes it to the LCD's framebuffer.
//
//   wake [--camera rpicam|fake|off] [--mirror] [--flip|--no-flip] [--fb /dev/fb_lcd] [--rotate 0|90|180|270]
//        [--swap] [--kbd DEVICE] [--fps 30] [--scene N] [--stream URL [--stream-scale N]]
//        [--record] [--tour SECONDS] [--out DIR --frames N [--every K]]
//
// --tour 10 shows every scene once for 10 s each, snapshots each one (PNG) and quits; with --record it also
// records the whole tour as one MP4. Made for docs and posts.
//
// Keys: space / enter / right (or /) next scene, left (or ,) previous, 1-6 pick one, P palette, A auto,
// D fields + frame rate, M mirror, F flip the camera 180, S snapshot (PNG), V record (MP4), L stream on/off
// (to the last --stream address), C camera on/off, Q or Esc quit.
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "analysis.h"
#include "camera.h"
#include "capture.h"
#include "font.h"
#include "gfx.h"
#include "scenes.h"

#define W 320
#define H 170
#define AUTO_S 90.0f
#define FADE_S 2.5f
#define VERSION "0.5.0"

typedef struct { const char *name; float a[3], b[3], c[3], d[3]; } Palette;
static const Palette PALETTES[] = {
  { "Aurora", { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { 1, 1, 1 }, { 0.0f, 0.33f, 0.67f } },
  { "Ember", { 0.55f, 0.35f, 0.25f }, { 0.5f, 0.35f, 0.25f }, { 1, 1, 1 }, { 0.0f, 0.08f, 0.15f } },
  { "Tide", { 0.25f, 0.45f, 0.55f }, { 0.3f, 0.4f, 0.45f }, { 1, 1, 1 }, { 0.55f, 0.6f, 0.68f } },
  { "Orchid", { 0.55f, 0.4f, 0.6f }, { 0.45f, 0.35f, 0.4f }, { 1, 1, 1 }, { 0.82f, 0.95f, 0.08f } },
  { "Acid", { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { 2, 1, 0 }, { 0.5f, 0.2f, 0.25f } },
};
#define NPAL ((int)(sizeof PALETTES / sizeof *PALETTES))

// flip: the CardputerZero's camera module is mounted upside down, so the picture is turned by default.
static struct { int scene, palette, autocycle, mirror, flip, debug; } S = { 0, 0, 1, 0, 1, 0 };
static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ------------------------------------------------------------------------------------------------ settings
static char conf_path[512];

// The last stream address and scale, so L can stream again without the command line.
static void stream_conf(char *path, size_t n) {
  const char *home = getenv("HOME");
  snprintf(path, n, "%s/.config/wake-stream", home ? home : "/tmp");
}
static int load_stream(char *url, size_t n, int *scale) {
  char p[512];
  stream_conf(p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f) return 0;
  char fmt[32];
  snprintf(fmt, sizeof fmt, "%%%zus %%d", n - 1);
  const int got = fscanf(f, fmt, url, scale);
  fclose(f);
  if (got < 2) *scale = 1;
  return got >= 1;
}
static void save_stream(const char *url, int scale) {
  char p[512], dir[512];
  const char *home = getenv("HOME");
  snprintf(dir, sizeof dir, "%s/.config", home ? home : "/tmp");
  mkdir(dir, 0755);
  stream_conf(p, sizeof p);
  FILE *f = fopen(p, "w");
  if (!f) return;
  fprintf(f, "%s %d\n", url, scale);
  fclose(f);
}
static int no_save;
static void load_settings(void) {
  const char *home = getenv("HOME");
  snprintf(conf_path, sizeof conf_path, "%s/.config/wake-cardputer", home ? home : "/tmp");
  FILE *f = fopen(conf_path, "r");
  if (!f) return;
  const int got = fscanf(f, "%d %d %d %d %d", &S.scene, &S.palette, &S.autocycle, &S.mirror, &S.flip);
  if (got < 4) S.scene = S.palette = 0;
  if (got < 5) S.flip = 1;   // settings from before flip existed
  fclose(f);
  S.scene = ((S.scene % NSCENES) + NSCENES) % NSCENES;
  S.palette = ((S.palette % NPAL) + NPAL) % NPAL;
}
static void save_settings(void) {
  if (no_save) return;
  const char *home = getenv("HOME");
  char dir[512];
  snprintf(dir, sizeof dir, "%s/.config", home ? home : "/tmp");
  mkdir(dir, 0755);
  FILE *f = fopen(conf_path, "w");
  if (!f) return;
  fprintf(f, "%d %d %d %d %d\n", S.scene, S.palette, S.autocycle, S.mirror, S.flip);
  fclose(f);
}

// ------------------------------------------------------------------------------------------------ display
typedef struct {
  int fd, xres, yres, bpp, line, rotate, swap;
  struct fb_bitfield r, g, b;
  unsigned char *map, *back;
  size_t size;
} Display;

static int display_open(Display *d, const char *path, int rotate, int swap) {
  memset(d, 0, sizeof *d);
  d->fd = open(path, O_RDWR);
  if (d->fd < 0) { fprintf(stderr, "wake: can't open %s: %s\n", path, strerror(errno)); return -1; }
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo fx;
  if (ioctl(d->fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(d->fd, FBIOGET_FSCREENINFO, &fx) < 0) {
    fprintf(stderr, "wake: %s isn't a framebuffer\n", path);
    return -1;
  }
  d->xres = v.xres; d->yres = v.yres; d->bpp = v.bits_per_pixel; d->line = fx.line_length;
  d->r = v.red; d->g = v.green; d->b = v.blue;
  d->size = (size_t)fx.line_length * v.yres;
  // A portrait panel (170 x 320) needs the picture turned; --rotate overrides the guess.
  d->rotate = rotate >= 0 ? rotate : (d->xres < d->yres ? 90 : 0);
  d->swap = swap;
  d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
  if (d->map == MAP_FAILED) d->map = NULL;   // fall back to write()
  d->back = calloc(1, d->size);
  fprintf(stderr, "wake: %s is %dx%d, %d bpp (rgb %d/%d/%d at %d/%d/%d), rotate %d%s\n", path, d->xres, d->yres, d->bpp,
          d->r.length, d->g.length, d->b.length, d->r.offset, d->g.offset, d->b.offset, d->rotate, d->swap ? ", swapped bytes" : "");
  return 0;
}

static inline unsigned pack_px(const Display *d, const unsigned char *p) {
  return ((unsigned)(p[0] >> (8 - d->r.length)) << d->r.offset) | ((unsigned)(p[1] >> (8 - d->g.length)) << d->g.offset) |
         ((unsigned)(p[2] >> (8 - d->b.length)) << d->b.offset);
}

// rgba: W x H, rows bottom first.
static void display_show(Display *d, const unsigned char *rgba) {
  const int bytes = d->bpp / 8;
  for (int y = 0; y < H; y++) {
    const unsigned char *src = rgba + 4 * (H - 1 - y) * W;
    for (int x = 0; x < W; x++, src += 4) {
      int fx, fy;
      switch (d->rotate) {
        case 90: fx = H - 1 - y; fy = x; break;
        case 180: fx = W - 1 - x; fy = H - 1 - y; break;
        case 270: fx = y; fy = W - 1 - x; break;
        default: fx = x; fy = y;
      }
      if (fx >= d->xres || fy >= d->yres) continue;
      unsigned char *o = d->back + fy * d->line + fx * bytes;
      const unsigned v = pack_px(d, src);
      if (bytes == 2) {
        if (d->swap) { o[0] = v >> 8; o[1] = v; } else { o[0] = v; o[1] = v >> 8; }
      } else if (bytes == 4) {
        o[0] = v; o[1] = v >> 8; o[2] = v >> 16; o[3] = 0xff;
      } else if (bytes == 3) {
        o[0] = v; o[1] = v >> 8; o[2] = v >> 16;
      }
    }
  }
  if (d->map) memcpy(d->map, d->back, d->size);
  else { lseek(d->fd, 0, SEEK_SET); if (write(d->fd, d->back, d->size) < 0) {} }
}

static void display_clear(Display *d) {
  memset(d->back, 0, d->size);
  if (d->map) memcpy(d->map, d->back, d->size);
}

// ------------------------------------------------------------------------------------------------ keyboard
static int kbd_open(const char *want) {
  const char *env = getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE");
  const char *tries[] = { want, env, "/dev/input/by-path/platform-3f804000.i2c-event" };
  for (int i = 0; i < 3; i++) {
    if (!tries[i]) continue;
    int fd = open(tries[i], O_RDONLY | O_NONBLOCK);
    if (fd >= 0) return fd;
  }
  for (int i = 0; i < 16; i++) {   // any device with letter keys
    char p[32];
    snprintf(p, sizeof p, "/dev/input/event%d", i);
    int fd = open(p, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    unsigned char bits[KEY_MAX / 8 + 1] = { 0 };
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 && (bits[KEY_A / 8] & (1 << (KEY_A % 8)))) return fd;
    close(fd);
  }
  return -1;
}

// ------------------------------------------------------------------------------------------------ output
static void write_ppm(const char *dir, int n, const unsigned char *rgba) {
  char p[600];
  snprintf(p, sizeof p, "%s/frame_%05d.ppm", dir, n);
  FILE *f = fopen(p, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = H - 1; y >= 0; y--)
    for (int x = 0; x < W; x++) fwrite(rgba + 4 * (y * W + x), 1, 3, f);
  fclose(f);
}

// ------------------------------------------------------------------------------------------------ main
static void usage(void) {
  fprintf(stderr, "usage: wake [--camera rpicam|fake|off] [--mirror] [--flip|--no-flip] [--fb PATH] [--rotate 0|90|180|270]\n"
                  "            [--swap] [--kbd DEVICE] [--fps N] [--scene N] [--stream URL [--stream-scale N]]\n"
                  "            [--record] [--tour SECONDS] [--out DIR --frames N [--every K]]\n"
                  "  --tour 10 --record               every scene for 10 s: one MP4 plus a PNG of each, then quit\n"
                  "  --stream udp://<computer>:5000  live H.264 (MPEG-TS) for VLC, OBS, TouchDesigner, ...\n");
}

int main(int argc, char **argv) {
  const char *cam_mode = "rpicam", *fb_path = "/dev/fb_lcd", *kbd_path = NULL, *out_dir = NULL;
  const char *stream_url = NULL;
  float tour = 0;
  int record_arg = 0;
  int rotate = -1, swap = 0, fps = 30, frames_max = 0, every = 1, scene_arg = -1, mirror_arg = -1, flip_arg = -1, stream_scale = 1;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--camera") && v) cam_mode = argv[++i];
    else if (!strcmp(a, "--mirror")) mirror_arg = 1;
    else if (!strcmp(a, "--flip")) flip_arg = 1;
    else if (!strcmp(a, "--no-flip")) flip_arg = 0;
    else if (!strcmp(a, "--stream") && v) stream_url = argv[++i];
    else if (!strcmp(a, "--stream-scale") && v) stream_scale = atoi(argv[++i]);
    else if (!strcmp(a, "--tour") && v) tour = atof(argv[++i]);
    else if (!strcmp(a, "--record")) record_arg = 1;
    else if (!strcmp(a, "--fb") && v) fb_path = argv[++i];
    else if (!strcmp(a, "--rotate") && v) rotate = atoi(argv[++i]);
    else if (!strcmp(a, "--swap")) swap = 1;
    else if (!strcmp(a, "--kbd") && v) kbd_path = argv[++i];
    else if (!strcmp(a, "--fps") && v) fps = atoi(argv[++i]);
    else if (!strcmp(a, "--scene") && v) scene_arg = atoi(argv[++i]);
    else if (!strcmp(a, "--out") && v) out_dir = argv[++i];
    else if (!strcmp(a, "--frames") && v) frames_max = atoi(argv[++i]);
    else if (!strcmp(a, "--every") && v) every = atoi(argv[++i]);
    else { usage(); return a[0] == '-' && a[1] == 'h' ? 0 : 2; }
  }
  if (fps < 5) fps = 5;
  if (every < 1) every = 1;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);
  signal(SIGPIPE, SIG_IGN);
  srand((unsigned)time(NULL));
  load_settings();
  if (scene_arg >= 0) S.scene = scene_arg % NSCENES;
  if (mirror_arg >= 0) S.mirror = mirror_arg;
  if (flip_arg >= 0) S.flip = flip_arg;
  if (tour > 0) {   // every scene once, from the first, without touching the saved settings
    if (tour < 5) tour = 5;
    S.scene = 0; S.autocycle = 1; no_save = 1;
  }
  const float auto_s = tour > 0 ? tour : AUTO_S;

  Display disp;
  const int headless = out_dir != NULL;
  if (!headless && display_open(&disp, fb_path, rotate, swap) < 0) return 1;
  if (gfx_init() < 0) return 1;
  fprintf(stderr, "wake-zero %s: OpenGL ES on %s\n", VERSION, gfx_renderer());

  // Camera fields as textures: rgba = level (high byte), motion, trail, level (low byte); flow at half size.
  G.cam = gfx_texture(CAM_W, CAM_H, GL_LINEAR);
  G.flow = gfx_texture(CAM_W / 2, CAM_H / 2, GL_LINEAR);
  G.camRes[0] = CAM_W; G.camRes[1] = CAM_H; G.flowRes[0] = CAM_W / 2; G.flowRes[1] = CAM_H / 2;
  G.aspect = (float)W / H;
  G.bg = 0.3f;
  const float ca = CAM_ASPECT, scr = G.aspect;   // crop the camera to fill the screen ("cover")
  G.camScale[0] = scr > ca ? 1 : scr / ca;
  G.camScale[1] = scr > ca ? ca / scr : 1;
  float *gray = calloc(CAM_W * CAM_H, sizeof(float));

  // Clear the fields to "nothing there".
  void upload_fields(const Analyzer *an);
  upload_fields(NULL);

  scenes_init(W, H);
  composite_init();
  Target outs[2] = { gfx_target(W, H, GL_LINEAR), gfx_target(W, H, GL_LINEAR) };
  Target final = gfx_target(W, H, GL_NEAREST);
  unsigned char *rgba = malloc(W * H * 4);
  Encoder rec = { 0 }, live = { 0 };
  int snap = 0;
  char path[400];
  // L toggles the stream: to --stream's address, or else the last one used.
  char live_url[400] = "";
  int live_scale = stream_scale;
  if (stream_url) { snprintf(live_url, sizeof live_url, "%s", stream_url); save_stream(live_url, live_scale); }
  else load_stream(live_url, sizeof live_url, &live_scale);
  if (stream_url) {
    if (enc_stream(&live, W, H, fps, stream_url, stream_scale)) fprintf(stderr, "wake: streaming to %s\n", stream_url);
    else fprintf(stderr, "wake: couldn't start ffmpeg for the stream\n");
  }
  if (record_arg) {
    if (enc_record(&rec, W, H, fps, path, sizeof path)) fprintf(stderr, "wake: recording to %s\n", path);
    else fprintf(stderr, "wake: couldn't start ffmpeg to record\n");
  }
  int tour_snapped = -1;

  Analyzer *an = an_new(CAM_W, CAM_H);
  Camera *cam = cam_open(cam_mode);
  int have_fields = 0;
  int kbd = headless ? -1 : kbd_open(kbd_path);
  if (!headless && kbd < 0) fprintf(stderr, "wake: no keyboard found; it will auto-cycle\n");

  int cur = S.scene, prev = -1, frame = 0;
  float fade = 1, sceneT = 0;
  double flash_until = 0;
  char flash[48];
  snprintf(flash, sizeof flash, "%s", SCENES[cur].title);
  const double t0 = now_s();
  double last = t0, last_cam = t0, perf_since = t0;
  flash_until = 2.5;
  int perf_n = 0;
  float perf_fps = 0, perf_ms = 0, perf_work = 0;

  while (running) {
    const double wall = now_s();
    const float t = headless ? frame / 30.0f : (float)(wall - t0);
    float dt = headless ? 1.0f / 30 : (float)(wall - last);
    dt = fminf(1.0f / 20, fmaxf(1.0f / 240, dt));
    last = wall;

    // Keys
    int go = INT_MIN;   // scene to change to
    struct input_event ev;
    while (kbd >= 0 && read(kbd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
      if (ev.type != EV_KEY || ev.value != 1) continue;
      switch (ev.code) {
        case KEY_SPACE: case KEY_ENTER: case KEY_RIGHT: case KEY_SLASH: case KEY_DOWN: case KEY_DOT: go = cur + 1; break;
        case KEY_LEFT: case KEY_COMMA: case KEY_UP: case KEY_SEMICOLON: go = cur - 1; break;
        case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5: case KEY_6: case KEY_7: case KEY_8: case KEY_9:
          if (ev.code - KEY_1 < NSCENES) go = ev.code - KEY_1;
          break;
        case KEY_P:
          S.palette = (S.palette + 1) % NPAL; save_settings();
          snprintf(flash, sizeof flash, "%s", PALETTES[S.palette].name); flash_until = t + 2.5; break;
        case KEY_A:
          S.autocycle = !S.autocycle; save_settings();
          snprintf(flash, sizeof flash, "auto %s", S.autocycle ? "on" : "off"); flash_until = t + 2.5; break;
        case KEY_D: S.debug = !S.debug; break;
        case KEY_F:
          S.flip = !S.flip; save_settings();
          snprintf(flash, sizeof flash, "flip %s", S.flip ? "on" : "off"); flash_until = t + 2.5; break;
        case KEY_S: snap = 1; break;
        case KEY_L:
          if (enc_on(&live)) {
            enc_stop(&live);
            fprintf(stderr, "wake: stream stopped\n");
            snprintf(flash, sizeof flash, "stream off");
          } else if (!live_url[0]) {
            snprintf(flash, sizeof flash, "no stream address");   // run once with --stream udp://<computer>:5000
            fprintf(stderr, "wake: no stream address yet: run once with --stream udp://<computer>:5000\n");
          } else if (enc_stream(&live, W, H, fps, live_url, live_scale)) {
            fprintf(stderr, "wake: streaming to %s\n", live_url);
            snprintf(flash, sizeof flash, "streaming");
          } else snprintf(flash, sizeof flash, "can't stream");
          flash_until = t + 2; break;
        case KEY_V:
          if (enc_on(&rec)) {
            enc_stop(&rec);
            fprintf(stderr, "wake: saved %s\n", rec.what);
            snprintf(flash, sizeof flash, "saved");
          } else if (enc_record(&rec, W, H, fps, path, sizeof path)) {
            fprintf(stderr, "wake: recording to %s\n", path);
            snprintf(flash, sizeof flash, "recording");
          } else snprintf(flash, sizeof flash, "can't record");
          flash_until = t + 2; break;
        case KEY_M:
          S.mirror = !S.mirror; save_settings();
          snprintf(flash, sizeof flash, "mirror %s", S.mirror ? "on" : "off"); flash_until = t + 2.5; break;
        case KEY_C:
          if (cam_live(cam)) { cam_close(cam); cam = cam_open("off"); have_fields = 0; upload_fields(NULL); }
          else { cam_close(cam); cam = cam_open(cam_mode); an_free(an); an = an_new(CAM_W, CAM_H); }
          snprintf(flash, sizeof flash, "%s", cam_status(cam)); flash_until = t + 2.5; break;
        case KEY_Q: case KEY_ESC: running = 0; break;
      }
    }
    if (S.autocycle && sceneT > auto_s && fade >= 1) {
      if (tour > 0 && cur == NSCENES - 1) running = 0;   // the tour is over
      else go = cur + 1;
    }
    if (tour > 0 && tour_snapped != cur && sceneT > auto_s * 0.7f) { snap = 1; tour_snapped = cur; }
    if (go != INT_MIN) {
      go = ((go % NSCENES) + NSCENES) % NSCENES;
      if (go != cur) {
        prev = cur; cur = go; fade = 0; sceneT = 0;
        S.scene = cur; save_settings();
        snprintf(flash, sizeof flash, "%s", SCENES[cur].title); flash_until = t + 2.5;
      }
    }

    // Camera
    const double cam_now = headless ? t : wall;
    if (cam_poll(cam, gray, (S.mirror ? CAM_MIRROR : 0) | (S.flip ? CAM_FLIP : 0), dt)) {
      an_update(an, gray, (float)(cam_now - last_cam));
      last_cam = cam_now;
      G.bg += (an_background_level(an) - G.bg) * 0.1f;
      upload_fields(an);
      have_fields = 1;
    }

    // Render
    const Palette *pal = &PALETTES[S.palette];
    G.time = fmodf(t, 3600); G.hue = fmodf(t * 0.006f, 1);
    memcpy(G.pa, pal->a, sizeof G.pa); memcpy(G.pb, pal->b, sizeof G.pb);
    memcpy(G.pc, pal->c, sizeof G.pc); memcpy(G.pd, pal->d, sizeof G.pd);
    sceneT += dt;
    const Analyzer *fields = have_fields ? an : NULL;
    SCENES[cur].step(dt, t, fields);
    SCENES[cur].draw(&outs[0]);
    if (fade < 1 && prev >= 0) {
      fade = fminf(1, fade + dt / FADE_S);
      SCENES[prev].step(dt, t, fields);
      SCENES[prev].draw(&outs[1]);
    }
    const float m = fade * fade * (3 - 2 * fade);
    composite_draw(&outs[0], &outs[1], prev >= 0 ? m : 1, S.debug, &final);
    gfx_read(&final, rgba);

    // Out to files and the network: the clean picture, before any labels go on.
    if (snap) {
      snap = 0;
      char tag[32];
      snprintf(tag, sizeof tag, "%d-%s", cur + 1, SCENES[cur].title);
      for (char *c = tag; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
      if (capture_snapshot(rgba, W, H, tag, path, sizeof path)) { fprintf(stderr, "wake: snapshot %s\n", path); snprintf(flash, sizeof flash, "snapshot"); }
      else snprintf(flash, sizeof flash, "can't save");
      flash_until = t + 1.5;
    }
    if (enc_on(&rec) && !enc_frame(&rec, rgba, W, H)) { snprintf(flash, sizeof flash, "recording stopped"); flash_until = t + 2.5; }
    if (enc_on(&live) && !enc_frame(&live, rgba, W, H)) { snprintf(flash, sizeof flash, "stream stopped"); flash_until = t + 2.5; }

    // Labels
    if (t < flash_until) {
      const int sc = 2;
      font_draw(rgba, W, H, (W - font_width(flash, sc)) / 2, H - 7 * sc - 10, flash, sc, 235, 235, 235);
    }
    if (enc_on(&live)) font_draw(rgba, W, H, W - 12 - (enc_on(&rec) ? 10 : 0) - font_width("LIVE", 1), 5, "LIVE", 1, 235, 40, 40);
    if (enc_on(&rec)) {   // a red dot, top right, while recording
      for (int y = 4; y < 12; y++)
        for (int x = W - 12; x < W - 4; x++) {
          const int dx = 2 * x - (2 * W - 17), dy = 2 * y - 15;
          if (dx * dx + dy * dy > 64) continue;
          unsigned char *d = rgba + 4 * ((H - 1 - y) * W + x);
          d[0] = 230; d[1] = 30; d[2] = 30;
        }
    }
    if (S.debug) {
      char line[96];
      snprintf(line, sizeof line, "%s %s %.0f FPS %.0fMS", SCENES[cur].title, PALETTES[S.palette].name, perf_fps, perf_ms);
      font_draw(rgba, W, H, 4, 4, line, 1, 235, 235, 235);
      snprintf(line, sizeof line, "%s%s%s%s BG %.2f %s", cam_status(cam), S.mirror ? " MIRROR" : "", S.flip ? " FLIP" : "",
               enc_on(&live) ? " LIVE" : "", G.bg, VERSION);
      font_draw(rgba, W, H, 4, 14, line, 1, 200, 200, 200);
    }

    if (headless) {
      if (frame % every == 0) write_ppm(out_dir, frame / every, rgba);
      if (frames_max && frame + 1 >= frames_max) running = 0;
    } else {
      display_show(&disp, rgba);
    }
    frame++;

    // Frame rate: report every 5 s, and sleep off what's left of the frame.
    const double done = now_s();
    perf_work += (float)(done - wall);
    perf_n++;
    if (done - perf_since >= 1) {
      perf_fps = perf_n / (float)(done - perf_since);
      perf_ms = perf_work * 1000 / perf_n;
      if (frame % (fps * 5) < perf_n) fprintf(stderr, "wake: %s %.1f fps, %.1f ms of work a frame, %s\n", SCENES[cur].title, perf_fps, perf_ms, cam_status(cam));
      perf_n = 0; perf_work = 0; perf_since = done;
    }
    if (!headless) {
      const double left = 1.0 / fps - (done - wall);
      if (left > 0) { struct timespec ts = { 0, (long)(left * 1e9) }; nanosleep(&ts, NULL); }
    }
  }

  enc_stop(&rec);
  enc_stop(&live);
  if (!headless) display_clear(&disp);
  cam_close(cam);
  save_settings();
  free(gray); free(rgba);
  return 0;
}

// The analysis fields into the camera textures (or "nothing there" for NULL).
void upload_fields(const Analyzer *an) {
  static unsigned char cam[CAM_W * CAM_H * 4], flow[(CAM_W / 2) * (CAM_H / 2) * 4];
  const int n = CAM_W * CAM_H, fn = (CAM_W / 2) * (CAM_H / 2);
  for (int i = 0; i < n; i++) {
    float L = an ? an->level[i] : 0, mo = an ? an->motion[i] : 0, tr = an ? an->trail[i] : 0;
    L = fminf(fmaxf(L, 0), 1) * 255;
    const float hi = floorf(L);
    cam[4 * i] = (unsigned char)hi;
    cam[4 * i + 1] = (unsigned char)lroundf(fminf(fmaxf(mo, 0), 1) * 255);
    cam[4 * i + 2] = (unsigned char)lroundf(fminf(fmaxf(tr, 0), 1) * 255);
    cam[4 * i + 3] = (unsigned char)lroundf((L - hi) * 255);
  }
  for (int i = 0; i < fn; i++)
    for (int k = 0; k < 2; k++) {
      const float v = an ? an->flow[2 * i + k] : 0;
      flow[4 * i + k] = (unsigned char)lroundf(fminf(fmaxf(0.5f + v / (2 * 1.5f), 0), 1) * 255);
    }
  for (int i = 0; i < fn; i++) { flow[4 * i + 2] = 0; flow[4 * i + 3] = 255; }
  glBindTexture(GL_TEXTURE_2D, G.cam);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CAM_W, CAM_H, GL_RGBA, GL_UNSIGNED_BYTE, cam);
  glBindTexture(GL_TEXTURE_2D, G.flow);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CAM_W / 2, CAM_H / 2, GL_RGBA, GL_UNSIGNED_BYTE, flow);
}
