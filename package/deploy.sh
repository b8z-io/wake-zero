#!/bin/sh
# From your computer: install the latest build of Wake Zero on a CardputerZero over SSH.
#   ./deploy.sh [pi@cardputerzero.local]
# (On the device itself, run the curl line below without ssh.)
set -e
HOST=${1:-pi@cardputerzero.local}
ssh -t "$HOST" 'rm -rf /tmp/wake-zero && mkdir /tmp/wake-zero &&
  curl -fsSL https://github.com/b8z-io/wake-zero/releases/download/latest/wake-zero.tar.gz | tar -xz -C /tmp/wake-zero &&
  sudo /tmp/wake-zero/install.sh'
