#!/bin/bash
# builds the CTR forwarder template CIA embedded in romm3ds' romfs.
# needs: docker (devkitpro/devkitarm), makerom + bannertool in TOOLS dir.
set -e
cd "$(dirname "$0")"
TOOLS="${TOOLS:-/tmp/claude-1000/-mnt-data2-Code-gba/a977f1d5-0ca1-4d8c-bbf8-324a40cb284e/scratchpad}"
MAKEROM="$TOOLS/makerom"
BANNERTOOL="$TOOLS/bannertool-1.2.3-linux/bannertool"

# 1. payload ELF
docker run --rm -v "$PWD/../ctr-payload":/fwd -w /fwd devkitpro/devkitarm:latest make >/dev/null
cp ../ctr-payload/fwd.elf forwarder.elf

# 2. base SMDH + banner (placeholders; patched per-game on device)
"$BANNERTOOL" makesmdh -i icon.png -s "romm3ds forwarder" -l "romm3ds forwarder" -p "romm3ds" -o template.smdh >/dev/null
"$BANNERTOOL" makebanner -i banner.png -a dsboot.wav -o template.bnr >/dev/null

# 3. template CIA (NoCrypto, fake sign, UniqueId placeholder 0xFF800)
"$MAKEROM" -f cia -target t -exefslogo -rsf build-cia.rsf -elf forwarder.elf \
  -banner template.bnr -icon template.smdh \
  -major 0 -minor 1 -micro 0 \
  -DAPP_PRODUCT_CODE=CTR-H-ROMM -DAPP_TITLE="romm3ds forwarder" -DAPP_UNIQUE_ID=0xFF800 \
  -o template.cia

mkdir -p ../romfs/ctr
mv template.cia ../romfs/ctr/template.cia
ls -la ../romfs/ctr/template.cia
echo OK
