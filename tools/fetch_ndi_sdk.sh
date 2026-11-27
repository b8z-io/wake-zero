#!/bin/sh
# Fetch the official NDI SDK for Linux and unpack what a build needs into ndi-sdk/ (git-ignored: nothing from the
# SDK is committed): the headers, plus the x86_64 library for testing on a desktop. Running this means accepting
# the NDI SDK License Agreement, which is copied in alongside. The installer script is not run: its payload is cut
# out and untarred.
set -eu
URL="https://downloads.ndi.tv/SDK/NDI_SDK_Linux/Install_NDI_SDK_v6_Linux.tar.gz"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/ndi-sdk"
if [ -f "$DEST/include/Processing.NDI.Lib.h" ]; then echo "NDI SDK already in $DEST"; exit 0; fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "Downloading $URL"
curl -fsSL --retry 4 -o "$TMP/sdk.tar.gz" "$URL"
tar -xzf "$TMP/sdk.tar.gz" -C "$TMP"
SH=$(ls "$TMP"/Install_NDI_SDK_*Linux.sh | head -1)
LINE=$(awk '/^__NDI_ARCHIVE_BEGIN__/ { print NR+1; exit 0; }' "$SH")
mkdir -p "$TMP/x"
tail -n +"$LINE" "$SH" | tar -xz -C "$TMP/x"
SDK="$TMP/x/NDI SDK for Linux"
rm -rf "$DEST"; mkdir -p "$DEST/lib/x86_64"
cp -R "$SDK/include" "$DEST/include"
cp "$SDK/lib/x86_64-linux-gnu/libndi.so.6."* "$DEST/lib/x86_64/libndi.so.6"
cp "$SDK/Version.txt" "$SDK/NDI SDK License Agreement.txt" "$DEST/"
echo "NDI SDK $(cat "$DEST/Version.txt") -> $DEST"
