#pragma once
#define CAM_W 128
#define CAM_H 72
#define CAM_ASPECT (16.0f / 9.0f)

typedef struct Camera Camera;
Camera *cam_open(const char *mode);   // "rpicam", "fake" or "off"
// orient: CAM_MIRROR flips left-right (a camera facing you), CAM_FLIP turns the picture 180 degrees.
#define CAM_MIRROR 1
#define CAM_FLIP 2
int cam_poll(Camera *c, float *gray, int orient, float dt);
int cam_live(const Camera *c);
const char *cam_status(const Camera *c);
void cam_close(Camera *c);
