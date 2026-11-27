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
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "downloading the NDI SDK..."
curl -fsSL --retry 4 -o "$TMP/sdk.tar.gz" https://downloads.ndi.tv/SDK/NDI_SDK_Linux/Install_NDI_SDK_v6_Linux.tar.gz
tar -xzf "$TMP/sdk.tar.gz" -C "$TMP"
SH=$(ls "$TMP"/Install_NDI_SDK_*Linux.sh | head -1)
LINE=$(awk '/^__NDI_ARCHIVE_BEGIN__/ { print NR+1; exit 0; }' "$SH")
mkdir -p "$TMP/x"
tail -n +"$LINE" "$SH" | tar -xz -C "$TMP/x"
SDK="$TMP/x/NDI SDK for Linux"
install -d "$DEST"
install -m 644 "$SDK/lib/aarch64-rpi4-linux-gnueabi/libndi.so.6."* "$DEST/libndi.so.6"
install -m 644 "$SDK/NDI SDK License Agreement.txt" "$SDK/Version.txt" "$DEST/"
systemctl enable --now avahi-daemon >/dev/null 2>&1 || true
ls -la "$DEST/libndi.so.6"
echo "NDI $(cat "$DEST/Version.txt") installed in $DEST. In Wake Zero, press L: 'Wake Zero' appears as an NDI source."
