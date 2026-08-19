#!/bin/bash
# build + push to 3DS ftpd. usage: ./push.sh [3ds-ip]
set -e
IP="${1:-192.168.0.9}"
cd "$(dirname "$0")"
# clean build: the 3dsx does not depend on romfs contents, stale embeds otherwise
docker run --rm -v "$PWD":/citrusi -w /citrusi devkitpro/devkitarm:latest sh -c \
  "mkdir -p romfs/gfx && cd gfx && tex3ds -i battery.t3s -o ../romfs/gfx/battery.t3x && cd .. && make clean >/dev/null && make"
curl -s --connect-timeout 10 -T citrusi.3dsx "ftp://$IP:5000/3ds/citrusi.3dsx"
echo "pushed to $IP — relaunch from Homebrew Launcher"
