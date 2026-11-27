# Wake Zero

Abstract art from a camera, on the [M5Stack CardputerZero](https://docs.m5stack.com/en/CardputerZero): a
credit-card-sized Linux computer (Raspberry Pi CM0, VideoCore IV GPU, 320x170 screen, 8 MP camera).

Point it at someone and the art is the wake they leave as they move. The camera picture is never shown or stored:
each frame is reduced to 128x72 greyscale, then to blurred brightness, motion and optical flow, and only those
fields drive the art. It's a native port of Wake, a browser piece, to OpenGL ES 2.0 on a GPU from 2012.

## Scenes

| Key | Scene | |
|---|---|---|
| 1 | **Strata** | A creeping contour map. Whoever is in view raises the terrain; movement heats the lines. |
| 2 | **Lattice** | A hex field of dots that swell, shove and light up with movement. |
| 3 | **Ripple** | A water surface. Movement presses in and leaves a wake; raindrops fall; caustics shimmer. |
| 4 | **Echo** | Video feedback. Movement pours light into an endless spiralling tunnel. |
| 5 | **Thermal** | A heat camera. You glow in false colour and leave a warm haze that drifts up as it cools. |

**Space** / **Enter** / **→** next scene · **←** previous · **1–5** pick one · **P** palette · **A** auto-cycle
(every 90 s) · **F** flip the camera 180° (on by default: the CardputerZero's camera is mounted upside down) ·
**M** mirror (for a camera facing you) · **S** snapshot · **V** start/stop recording · **C** camera on/off ·
**D** show the analysis fields and frame rate · **Q** or **Esc** quit. Settings are kept in
`~/.config/wake-cardputer`.

## Snapshots, recordings and live streams

Everything goes through `ffmpeg` (already on the CardputerZero image) and carries the clean picture, without the
on-screen labels.

- **S** saves a PNG, scaled up 4x with hard pixel edges (1280x680), to `~/Pictures/wake/`.
- **V** records H.264 MP4 at the screen's native 320x170 to `~/Videos/wake/` (a red dot shows while it records).
  To scale one up crisply on your computer:
  `ffmpeg -i wake.mp4 -vf scale=iw*4:ih*4:flags=neighbor -c:v libx264 -crf 16 wake-4x.mp4`
- **`--stream udp://<your computer>:5000`** sends a live H.264 (MPEG-TS) stream: open `udp://@:5000` in VLC,
  add it to OBS as a Media Source (untick *Local File*, input `udp://@:5000`), or use TouchDesigner's
  *Video Stream In*. `--stream-scale 2` enlarges it on the device first (more CPU). The camera faces away from
  the screen, so this is also how you watch yourself: point it at you and watch the stream.

To stream whenever it starts from the launcher, add the option to `Exec=` in
`/usr/share/APPLaunch/applications/wake.desktop`.

## Install

On the CardputerZero (needs Wi-Fi):

```sh
rm -rf /tmp/wake-zero && mkdir /tmp/wake-zero &&
curl -fsSL https://github.com/b8z-io/wake-zero/releases/download/latest/wake-zero.tar.gz | tar -xz -C /tmp/wake-zero &&
sudo /tmp/wake-zero/install.sh
```

Or from your computer over SSH: `package/deploy.sh pi@<device address>`.

The installer adds any missing libraries (`libegl1 libgles2 libgbm1 libegl-mesa0 rpicam-apps-lite`), puts Wake in
the launcher and makes sure the launcher's user is in the `video`, `render` and `input` groups. It needs the
camera working (`rpicam-hello --list-cameras` should list the imx219); without one it still runs, on its own.

If the picture is sideways, add `--rotate 90` (or `270`) to `Exec=` in
`/usr/share/APPLaunch/applications/wake.desktop`; if the colours are wrong, add `--swap`.

## How it works

- **`src/gfx.c`**: OpenGL ES 2.0 through EGL with no window: GBM on `/dev/dri/renderD128`, or Mesa's surfaceless
  platform elsewhere. Everything renders into textures.
- **`src/main.c`**: each frame is read back (`glReadPixels`, 320x170) and written to `/dev/fb_lcd` in the
  panel's pixel format. It runs as an ordinary launcher app (`package/wake.desktop`).
- **`src/camera.c`**: `rpicam-vid` at 256x144 YUV420 on a pipe; the luma is averaged down to 128x72.
- **`src/analysis.c`**: auto-levels, motion over a noise floor, trails, Lucas–Kanade optical flow. A port of
  Wake's JavaScript (`reference/`); `tests/parity.sh` checks they agree to float precision.
- **`src/scenes.c`**: the scenes in GLSL ES 1.00. VideoCore IV can only render to 8-bit targets, so:
  - feedback (Echo, Thermal) uses dithered rounding, so trails fade all the way out instead of sticking;
  - Ripple packs each wave height into two channels as 16-bit fixed point and blends between texels itself,
    since texture filtering rounds to 8 bits;
  - the camera's brightness field goes up with 16 bits the same way, so contour lines stay smooth.

## Building and testing

```sh
sudo apt install gcc make libegl-dev libgles-dev libgbm-dev     # on the device, or any Debian/Ubuntu
make
./wake --camera fake --scene 2 --out /tmp/frames --frames 300 --every 30   # headless: PPM frames
tests/parity.sh                                                         # C analysis vs the JavaScript
```

CI runs both, builds the arm64 binary on Debian trixie and publishes it as the `latest` release.
