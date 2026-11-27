// Snapshots, recordings and streams. Frames go to ffmpeg (on the CardputerZero image already) on a pipe as raw
// RGBA, bottom row first, so every command flips them the right way up.
#include "capture.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void dir_for(char *out, size_t n, const char *sub) {
  const char *home = getenv("HOME");
  char base[256];
  snprintf(base, sizeof base, "%s/%s", home ? home : "/tmp", sub);   // e.g. ~/Videos
  mkdir(base, 0755);
  snprintf(out, n, "%s/wake", base);
  mkdir(out, 0755);
}

static void stamp(char *out, size_t n) {
  time_t t = time(NULL);
  strftime(out, n, "%Y%m%d-%H%M%S", localtime(&t));
}

int capture_snapshot(const unsigned char *rgba, int w, int h, const char *tag, char *path_out, size_t n) {
  char dir[300], ts[32], ppm[400];
  dir_for(dir, sizeof dir, "Pictures");
  stamp(ts, sizeof ts);
  snprintf(ppm, sizeof ppm, "/tmp/wake-%s.ppm", ts);
  FILE *f = fopen(ppm, "wb");
  if (!f) return 0;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--)
    for (int x = 0; x < w; x++) fwrite(rgba + 4 * (y * w + x), 1, 3, f);
  fclose(f);
  // Scaled up 4x with hard pixel edges, so it reads as the screen it came from. In the background, so the art
  // doesn't stall while it converts.
  if (tag) snprintf(path_out, n, "%s/wake-%s-%s.png", dir, ts, tag);
  else snprintf(path_out, n, "%s/wake-%s.png", dir, ts);
  char cmd[1200];
  snprintf(cmd, sizeof cmd, "(ffmpeg -hide_banner -loglevel error -y -i '%s' -vf scale=iw*4:ih*4:flags=neighbor '%s'; rm -f '%s') >/dev/null 2>&1 &",
           ppm, path_out, ppm);
  return system(cmd) == 0;
}

static int start(Encoder *e, int w, int h, int fps, const char *output_args) {
  char cmd[1500];
  snprintf(cmd, sizeof cmd,
           "exec ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - %s",
           w, h, fps, output_args);
  e->pipe = popen(cmd, "w");
  return e->pipe != NULL;
}

// Fragmented MP4 (a fragment a second), so a recording cut off by a kill, a closed SSH session or a flat
// battery still plays up to the moment it stopped: a plain MP4 only gets its index at the very end.
int enc_record(Encoder *e, int w, int h, int fps, char *path_out, size_t n) {
  char dir[300], ts[32], args[800];
  dir_for(dir, sizeof dir, "Videos");
  stamp(ts, sizeof ts);
  snprintf(path_out, n, "%s/wake-%s.mp4", dir, ts);
  snprintf(args, sizeof args,
           "-vf vflip -c:v libx264 -preset veryfast -crf 16 -g %d -pix_fmt yuv420p "
           "-movflags +frag_keyframe+empty_moov+default_base_moof '%s' >/dev/null 2>&1",
           fps, path_out);
  snprintf(e->what, sizeof e->what, "%s", path_out);
  return start(e, w, h, fps, args);
}

// url: anything ffmpeg can send to, as MPEG-TS: udp://<your computer>:5000 (VLC, OBS, TouchDesigner),
// srt://<host>:<port>, ... scale > 1 enlarges it with hard pixel edges before encoding (more CPU).
int enc_stream(Encoder *e, int w, int h, int fps, const char *url, int scale) {
  char vf[120], target[400], args[1000];
  if (scale > 1) snprintf(vf, sizeof vf, "vflip,scale=iw*%d:ih*%d:flags=neighbor", scale, scale);
  else snprintf(vf, sizeof vf, "vflip");
  // Over UDP, send whole MPEG-TS packets per datagram unless the URL already says otherwise.
  if (!strncmp(url, "udp://", 6) && !strstr(url, "pkt_size"))
    snprintf(target, sizeof target, "%s%spkt_size=1316", url, strchr(url, '?') ? "&" : "?");
  else snprintf(target, sizeof target, "%s", url);
  snprintf(args, sizeof args,
           "-vf %s -c:v libx264 -preset veryfast -tune zerolatency -g %d -b:v 3M -maxrate 3M -bufsize 1M "
           "-pix_fmt yuv420p -f mpegts '%s' >/dev/null 2>&1",
           vf, fps, target);
  snprintf(e->what, sizeof e->what, "%s", url);
  return start(e, w, h, fps, args);
}

int enc_frame(Encoder *e, const unsigned char *rgba, int w, int h) {
  if (!e->pipe) return 0;
  if (fwrite(rgba, 1, (size_t)w * h * 4, e->pipe) != (size_t)w * h * 4) { enc_stop(e); return 0; }
  return 1;
}

void enc_stop(Encoder *e) {
  if (e->pipe) pclose(e->pipe);
  e->pipe = NULL;
}
