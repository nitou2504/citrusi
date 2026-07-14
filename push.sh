#!/bin/bash
# build + push to 3DS ftpd. usage: ./push.sh [3ds-ip]
set -e
IP="${1:-192.168.0.9}"
cd "$(dirname "$0")"
# clean build: the 3dsx does not depend on romfs contents, stale embeds otherwise
docker run --rm -v "$PWD":/romm3ds -w /romm3ds devkitpro/devkitarm:latest sh -c \
  "mkdir -p romfs/gfx && cd gfx && tex3ds -i battery.t3s -o ../romfs/gfx/battery.t3x && cd .. && make clean >/dev/null && make"
curl -s --connect-timeout 10 -T romm3ds.3dsx "ftp://$IP:5000/3ds/romm3ds.3dsx"
echo "pushed to $IP — relaunch from Homebrew Launcher"
