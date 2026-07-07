#!/bin/bash
# build + push to 3DS ftpd. usage: ./push.sh [3ds-ip]
set -e
IP="${1:-192.168.0.9}"
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/romm3ds -w /romm3ds devkitpro/devkitarm:latest make
curl -s --connect-timeout 10 -T romm3ds.3dsx "ftp://$IP:5000/3ds/romm3ds.3dsx"
echo "pushed to $IP — relaunch from Homebrew Launcher"
