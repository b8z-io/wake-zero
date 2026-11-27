#!/bin/sh
# Installs Wake on the CardputerZero so it appears in the launcher. Run on the device from the unpacked folder:
#   sudo ./install.sh
set -e
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo ./install.sh"; exit 1; }
APP=/usr/share/APPLaunch

# Libraries: OpenGL ES through Mesa (VideoCore IV), and rpicam-vid for the camera.
need=""
for p in libegl1 libgles2 libgbm1 libegl-mesa0 rpicam-apps-lite; do
  dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"
done
if [ -n "$need" ]; then
  echo "installing:$need"
  apt-get update -q && apt-get install -y -q --no-install-recommends $need
fi

install -D -m 755 wake "$APP/bin/wake"
install -D -m 644 wake_100.png "$APP/share/images/wake_100.png"
install -D -m 644 wake_80.png "$APP/share/images/wake_80.png"
install -D -m 644 wake.desktop "$APP/applications/wake.desktop"

# The launcher runs apps as its configured user (pi unless changed): it needs the GPU, screen, keyboard and camera.
user=${SUDO_USER:-pi}
for g in video render input; do
  getent group "$g" >/dev/null && ! id -nG "$user" | grep -qw "$g" && usermod -aG "$g" "$user" && echo "added $user to $g (takes effect at next login or reboot)" || true
done
echo "installed: Wake is in the launcher. To try it from SSH instead: $APP/bin/wake (Q or Esc quits)"
