// Camera input: small greyscale frames for the analysis, never kept or shown.
//   rpicam  the Cardputer's IMX219 through rpicam-vid (or libcamera-vid), 256x144 YUV420 on a pipe; only the
//           luma is used, averaged down to 128x72
//   fake    a synthetic room with someone walking about and waving (for testing without a camera)
//   off     no camera: the scenes run on their own
#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define CW 256
#define CH 144
#define FRAME (CW * CH * 3 / 2)

struct Camera {
  int mode;            // 0 off, 1 rpicam, 2 fake
  pid_t pid;
  int fd;
  unsigned char *buf;
  size_t have;
  int frames;
  float fake_t;
  char status[96];
};

static int on_path(const char *exe) {
  const char *path = getenv("PATH");
  char dirs[1024], full[1200];
  snprintf(dirs, sizeof dirs, "%s", path ? path : "/usr/bin:/bin");
  for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":")) {
    snprintf(full, sizeof full, "%s/%s", d, exe);
    if (access(full, X_OK) == 0) return 1;
  }
  return 0;
}

static int start_rpicam(Camera *c) {
  const char *exe = on_path("rpicam-vid") ? "rpicam-vid" : on_path("libcamera-vid") ? "libcamera-vid" : NULL;
  if (!exe) { snprintf(c->status, sizeof c->status, "no rpicam-vid (apt install rpicam-apps-lite)"); return 0; }
  int p[2];
  if (pipe(p) < 0) return 0;
  pid_t pid = fork();
  if (pid < 0) { close(p[0]); close(p[1]); return 0; }
  if (pid == 0) {
    // Die with Wake, however it ends: the launcher kills apps outright when Esc is held, and an orphaned
    // rpicam-vid would keep the camera busy for the next one.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) _exit(1);
    dup2(p[1], 1);
    int log = open("/tmp/wake-camera.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);   // rpicam-vid's own messages
    if (log >= 0) dup2(log, 2);
    close(p[0]); close(p[1]);
    execlp(exe, exe, "-t", "0", "-n", "--codec", "yuv420", "--width", "256", "--height", "144",
           "--framerate", "30", "--flush", "-o", "-", (char *)NULL);
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  c->pid = pid; c->fd = p[0];
  c->buf = malloc(FRAME);
  snprintf(c->status, sizeof c->status, "camera starting");
  return 1;
}

Camera *cam_open(const char *mode) {
  Camera *c = calloc(1, sizeof *c);
  c->fd = -1;
  if (!strcmp(mode, "fake")) { c->mode = 2; snprintf(c->status, sizeof c->status, "fake camera"); }
  else if (!strcmp(mode, "off")) { snprintf(c->status, sizeof c->status, "camera off"); }
  else if (start_rpicam(c)) c->mode = 1;
  return c;
}

const char *cam_status(const Camera *c) { return c->status; }
int cam_live(const Camera *c) { return c->mode != 0; }

void cam_close(Camera *c) {
  if (!c) return;
  if (c->pid > 0) { kill(c->pid, SIGTERM); waitpid(c->pid, NULL, 0); }
  if (c->fd >= 0) close(c->fd);
  free(c->buf);
  free(c);
}

// Where camera pixel (x, y) lands in the analysis frame.
static inline int dest(int x, int y, int orient) {
  const int fx = ((orient & CAM_MIRROR) != 0) != ((orient & CAM_FLIP) != 0), fy = (orient & CAM_FLIP) != 0;
  return (fy ? CAM_H - 1 - y : y) * CAM_W + (fx ? CAM_W - 1 - x : x);
}

static void fake_frame(Camera *c, float *g, int orient) {
  const float t = c->fake_t;
  c->fake_t += 1.0f / 30;
  const float cx = 30 + 60 * (0.5f - 0.5f * cosf(t * 0.6f)), cy = 40;
  const float ax = cx + 18 + 10 * sinf(t * 5), ay = 24 + 6 * cosf(t * 5);
  for (int y = 0; y < CAM_H; y++)
    for (int x = 0; x < CAM_W; x++) {
      const float room = 0.35f + 0.15f * x / CAM_W + 0.05f * sinf(y / 9.0f);
      const float bx = (x - cx) / 14, by = (y - cy) / 22, rx = (x - ax) / 5, ry = (y - ay) / 5;
      float v = room + 0.45f * expf(-(bx * bx + by * by)) + 0.4f * expf(-(rx * rx + ry * ry)) + 0.03f * sinf(t * 9);
      v += ((rand() & 255) / 255.0f - 0.5f) * 0.02f;
      g[dest(x, y, orient)] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
}

// Reads whatever has arrived. Returns 1 if a new frame was written to g (CAM_W x CAM_H, 0..1).
int cam_poll(Camera *c, float *g, int orient, float dt) {
  if (c->mode == 2) {
    static float acc;
    acc += dt;
    if (acc < 1.0f / 30) return 0;
    acc = 0;
    fake_frame(c, g, orient);
    return 1;
  }
  if (c->mode != 1) return 0;
  int got = 0;
  for (;;) {
    ssize_t r = read(c->fd, c->buf + c->have, FRAME - c->have);
    if (r > 0) {
      c->have += r;
      if (c->have == FRAME) { got = 1; c->have = 0; c->frames++; }
      if (got) {
        // Keep reading in case a newer frame is already waiting; the newest complete one wins.
        const unsigned char *Y = c->buf;
        for (int y = 0; y < CAM_H; y++)
          for (int x = 0; x < CAM_W; x++) {
            const unsigned char *p = Y + 2 * y * CW + 2 * x;
            const float v = (p[0] + p[1] + p[CW] + p[CW + 1]) * (1.0f / (4 * 255));
            g[dest(x, y, orient)] = v;
          }
        snprintf(c->status, sizeof c->status, "camera live");
      }
      continue;
    }
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {   // rpicam-vid has gone
      snprintf(c->status, sizeof c->status, "camera stopped: see /tmp/wake-camera.log");
      close(c->fd); c->fd = -1;
      if (c->pid > 0) { waitpid(c->pid, NULL, WNOHANG); c->pid = 0; }
      c->mode = 0;
    }
    break;
  }
  return got;
}
