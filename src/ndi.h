// NDI® output: the picture as a source anyone on the network can pick up by name (OBS, NDI Studio Monitor,
// TouchDesigner, vMix...), found automatically, no addresses. NDI® is a registered trademark of Vizrt NDI AB.
//
// The NDI library isn't part of this program: package/install-ndi.sh fetches it from NDI onto the device (using
// it means accepting the NDI SDK licence). It's loaded when NDI is switched on, so everything else works without.
#pragma once
#include <stddef.h>

typedef struct Ndi Ndi;

// name: the source's name. NULL on failure, with the reason in err.
Ndi *ndi_open(const char *name, char *err, size_t n);
// rgba: w x h, rows bottom first. Sent scaled up by scale with hard pixel edges, at fps.
void ndi_send(Ndi *s, const unsigned char *rgba, int w, int h, int scale, int fps);
int ndi_connections(Ndi *s);   // receivers watching now
void ndi_close(Ndi *s);
