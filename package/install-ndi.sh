#!/bin/sh
# Adds NDI® output to Wake Zero (and Dome Zero) on the CardputerZero: downloads the official NDI SDK for Linux from
# NDI (about 67 MB) and installs its Raspberry Pi 64-bit library. NDI is NDI's, under its own licence, so it isn't
# shipped with these apps; this fetches it for you once you've agreed to that licence.
#   sudo ./install-ndi.sh [--yes]
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo ./install-ndi.sh"; exit 1; }
DEST=/usr/share/APPLaunch/lib/ndi
if [ "$1" != "--yes" ]; then
  echo "NDI is a registered trademark of Vizrt NDI AB. Using the NDI library means accepting the"
  echo "NDI SDK License Agreement: http://ndi.link/ndisdk_license"
  printf "Download and install it? [y/N] "
  read -r ok
  case "$ok" in y|Y|yes|YES) ;; *) echo "not installed"; exit 0 ;; esac
fi
if ! dpkg -s libavahi-client3 avahi-daemon >/dev/null 2>&1; then
  apt-get update -q && apt-get install -y -q --no-install-recommends libavahi-client3 avahi-daemon
fi
# Streamed: the SDK is hundreds of MB unpacked and /tmp here is a small RAM disk, so only the three files needed
# are written (about 5 MB), into /var/tmp on the SD card.
TMP=$(mktemp -d -p /var/tmp)
trap 'rm -rf "$TMP"' EXIT
S="NDI SDK for Linux"
LIB="$S/lib/aarch64-rpi4-linux-gnueabi/libndi.so.6.4.0"
echo "downloading the NDI SDK (about 67 MB; only the Raspberry Pi library is kept)..."
curl -fsSL --retry 4 https://downloads.ndi.tv/SDK/NDI_SDK_Linux/Install_NDI_SDK_v6_Linux.tar.gz |
  tar -xzO | sed '1,/^__NDI_ARCHIVE_BEGIN__$/d' |
  tar -xz -C "$TMP" "$LIB" "$S/Version.txt" "$S/NDI SDK License Agreement.txt" || true
if [ ! -s "$TMP/$LIB" ]; then
  echo "couldn't get the NDI library (download failed, or NDI changed the SDK). Free space:"; df -h /var/tmp /usr
  exit 1
fi
install -d "$DEST"
install -m 644 "$TMP/$LIB" "$DEST/libndi.so.6"
install -m 644 "$TMP/$S/NDI SDK License Agreement.txt" "$TMP/$S/Version.txt" "$DEST/"
systemctl enable --now avahi-daemon >/dev/null 2>&1 || true
ls -la "$DEST/libndi.so.6"
echo "NDI $(cat "$DEST/Version.txt") installed in $DEST. In Wake Zero, press L: 'Wake Zero' appears as an NDI source."
