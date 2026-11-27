#pragma once
#define CAM_W 128
#define CAM_H 72
#define CAM_ASPECT (16.0f / 9.0f)

typedef struct Camera Camera;
Camera *cam_open(const char *mode);   // "rpicam", "fake" or "off"
int cam_poll(Camera *c, float *gray, int mirror, float dt);
int cam_live(const Camera *c);
const char *cam_status(const Camera *c);
void cam_close(Camera *c);
