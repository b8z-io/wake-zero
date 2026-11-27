// Getting frames out: PNG snapshots, MP4 recordings and a live network stream, all through ffmpeg.
#pragma once
#include <stdio.h>

typedef struct { FILE *pipe; char what[300]; } Encoder;

// rgba: w x h, rows bottom first (as glReadPixels gives them).
int capture_snapshot(const unsigned char *rgba, int w, int h, char *path_out, size_t n);
int enc_record(Encoder *e, int w, int h, int fps, char *path_out, size_t n);
int enc_stream(Encoder *e, int w, int h, int fps, const char *url, int scale);
int enc_frame(Encoder *e, const unsigned char *rgba, int w, int h);   // 0 and stops if ffmpeg has gone
void enc_stop(Encoder *e);
static inline int enc_on(const Encoder *e) { return e->pipe != NULL; }
