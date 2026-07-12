#!/bin/bash
# Builds romm3ds: the CTR forwarder template, the .3dsx (Homebrew Launcher),
# and the installable .cia. Requires docker (devkitpro/devkitarm) plus makerom
# and bannertool. Point TOOLS at a dir containing them, or let it auto-download.
#
#   ./build.sh          # build everything
#   ./build.sh 3dsx     # just the .3dsx
#   ./build.sh cia      # just the .cia (implies 3dsx)
#   TOOLS=/path ./build.sh
set -e
cd "$(dirname "$0")"
ROOT="$PWD"
TOOLS="${TOOLS:-$ROOT/tools/bin}"
MAKEROM="$TOOLS/makerom"
BANNERTOOL="$TOOLS/bannertool"
DKA="devkitpro/devkitarm:latest"
WHAT="${1:-all}"

fetch_tools() {
  mkdir -p "$TOOLS"
  if [ ! -x "$MAKEROM" ]; then
    echo ">> downloading makerom"
    curl -sL -o /tmp/makerom.zip https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/makerom-v0.18.4-ubuntu_x86_64.zip
    unzip -o -q /tmp/makerom.zip -d "$TOOLS"; chmod +x "$MAKEROM"
  fi
  if [ ! -x "$BANNERTOOL" ]; then
    echo ">> downloading bannertool"
    curl -sL https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/bannertool-1.2.3-linux.tar.gz | tar xz -C /tmp
    cp /tmp/bannertool-1.2.3-linux/bannertool "$BANNERTOOL"; chmod +x "$BANNERTOOL"
  fi
}

build_template() {
  echo ">> building CTR forwarder template"
  docker run --rm -v "$ROOT/ctr-payload":/fwd -w /fwd "$DKA" make >/dev/null
  cp ctr-payload/fwd.elf ctr-template/forwarder.elf
  fetch_tools
  "$BANNERTOOL" makesmdh -i ctr-template/icon.png -s "romm3ds forwarder" -l "romm3ds forwarder" -p romm3ds -o ctr-template/template.smdh >/dev/null
  "$BANNERTOOL" makebanner -i ctr-template/banner.png -a ctr-template/dsboot.wav -o ctr-template/template.bnr >/dev/null
  "$MAKEROM" -f cia -target t -exefslogo -rsf ctr-template/build-cia.rsf -elf ctr-template/forwarder.elf \
    -banner ctr-template/template.bnr -icon ctr-template/template.smdh \
    -major 0 -minor 1 -micro 0 \
    -DAPP_PRODUCT_CODE=CTR-H-ROMM -DAPP_TITLE="romm3ds forwarder" -DAPP_UNIQUE_ID=0xFF800 \
    -o ctr-template/template.cia
  mkdir -p romfs/ctr
  mv ctr-template/template.cia romfs/ctr/template.cia
}

build_3dsx() {
  echo ">> building .3dsx"
  docker run --rm -v "$ROOT":/romm3ds -w /romm3ds "$DKA" sh -c "make clean >/dev/null && make"
}

build_cia() {
  echo ">> building .cia"
  fetch_tools
  "$BANNERTOOL" makebanner -i cia/banner.png -a ctr-template/dsboot.wav -o cia/app.bnr >/dev/null
  "$MAKEROM" -f cia -target t -exefslogo -rsf cia/app.rsf \
    -elf romm3ds.elf -icon romm3ds.smdh -banner cia/app.bnr \
    -major 0 -minor 2 -micro 0 -DAPP_TITLE=romm3ds \
    -o romm3ds.cia
  echo ">> romm3ds.cia ready"
  ls -la romm3ds.cia
}

case "$WHAT" in
  template) build_template ;;
  3dsx)     build_template; build_3dsx ;;
  cia)      build_template; build_3dsx; build_cia ;;
  all)      build_template; build_3dsx; build_cia ;;
  *) echo "usage: $0 [all|template|3dsx|cia]"; exit 1 ;;
esac
