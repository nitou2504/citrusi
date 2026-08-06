# romm3ds

[![Build 3DS artifacts](https://github.com/nitou2504/romm3ds/actions/workflows/build.yml/badge.svg)](https://github.com/nitou2504/romm3ds/actions/workflows/build.yml)

Offline-first Nintendo 3DS homebrew for turning a personal ROM library into
proper HOME-menu titles. `romm3ds` works from files on the SD card and can also
use a self-hosted [RomM](https://github.com/rommapp/romm) server as an optional
download source.

![romm3ds library, manage and HOME screens](docs/screens.png)

| Library | Manage | HOME menu |
|---|---|---|
| ![Library screen](docs/library.png) | ![Manage screen](docs/manage.png) | ![HOME screen](docs/home.png) |

## Features

- **Browse SD Card** — navigate from `sd:/roms` into subfolders and back to the
  SD root. Files are identified by extension, so `.cia`, `.nds` and `.gba`
  files can live wherever the user keeps them. ZIP archives are inspected and
  extracted on the 3DS.
- **Install 3DS games** — install a `.cia` directly to the 3DS title database
  through `am:net`. A source CIA is deleted after installation by default,
  because the installed title is the copy that remains on the console.
- **Install Nintendo DS games** — build a YANBF-style HOME-menu forwarder for
  an `.nds` file. The forwarder launches the ROM through nds-bootstrap and can
  use the ROM's icon plus downloaded banner art and sound.
- **Install Game Boy Advance games** — build a native AGB_FIRM Virtual Console
  CIA on the device. GBA installs support artwork and five built-in screen
  presets, including per-game filter memory.
- **Artwork tools** — use RomM covers, YANBF assets, GameTDB, libretro logos,
  SteamGridDB and iiSU where available. Artwork is cached on the SD card and
  can be changed later from Manage.
- **Manage installed titles** — view storage usage, installation state and
  title metadata; reinstall, change artwork, remove ROMs, uninstall games and
  remove 3DS updates/DLC where applicable.
- **Optional RomM Library** — browse the `3ds`, `nds` and `gba` platforms over
  HTTP Basic authentication. RomM 4.x and 5.x base-game responses are
  supported. Downloads use the same local install flow as files from the SD
  card.
- **Settings** — configure the RomM server, language, GBA screen preset,
  artwork notifications, artwork source preferences and the delete-after-
  install policy.

## Requirements

### Console

- A 3DS with Luma3DS CFW and the Homebrew Launcher.
- For Nintendo DS forwarders: the [NTR_Forwarder](https://github.com/RocketRobz/NTR_Forwarder)
  DS Forwarder Pack (`_nds` on the SD root), a working
  [TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu) /
  nds-bootstrap installation, and the YANBF `bootstrap.cia` TWL title
  (`0004800546574452`) installed to NAND.
- A RomM instance is optional. When used, it must be reachable from the 3DS
  over the local network. HTTP and HTTPS are accepted; HTTPS certificate
  verification is disabled by the current client.

### Build machine

- Docker, for the official `devkitpro/devkitarm` image.
- Internet access on the first build so `makerom` and `bannertool` can be
  downloaded into the ignored `tools/bin/` directory.

## Install on a 3DS

Choose one of these application formats:

- **CIA:** install `romm3ds.cia` with FBI. This creates a normal HOME-menu
  application.
- **3DSX:** copy `romm3ds.3dsx` to `sd:/3ds/` and launch it from the Homebrew
  Launcher.

On first launch, the app opens with the offline SD-card flow. To use RomM,
open **Settings**, enter the server address, username and password, then test
the connection from the RomM settings screen.

The default browse location is `sd:/roms`. The browser is not restricted to
that folder: use `B` or the `.. (up)` row to reach the SD root and browse any
other folder containing ROMs.

## Build

The normal build creates the forwarder template, the Homebrew Launcher build
and the installable CIA:

```sh
./build.sh              # template + 3DSX + CIA
./build.sh 3dsx         # template + 3DSX
./build.sh cia          # template + 3DSX + CIA
./build.sh template     # forwarder template only
```

Generated files are written at the repository root and are ignored by Git:

```text
romm3ds.3dsx
romm3ds.smdh
romm3ds.elf
romm3ds.cia
```

The GitHub Actions workflow runs the complete `./build.sh all` path and
uploads the `.3dsx`, `.smdh` and `.cia` as one build artifact.

For hardware iteration over FTP:

```sh
./push.sh 192.168.0.23
```

This performs a clean 3DSX build and uploads it to `sd:/3ds/romm3ds.3dsx`.

## How the formats work

### 3DS CIA

A downloaded or local `.cia` is streamed into the 3DS title database with
`AM_StartCiaInstall`. It does not need the DS forwarder chain and has no
artwork or screen-filter customization because the CIA already contains its
own application metadata.

### Nintendo DS forwarder

The 3DS cannot run DS games directly. A forwarder is a small HOME-menu title
that tells nds-bootstrap which `.nds` file to boot. `romm3ds` patches a
prebuilt template CIA on the console, adds the selected icon/banner/sound and
installs it with a unique title ID.

At launch, the forwarder writes the ROM path to
`sd:/_nds/ntr-forwarder/path.txt` and chainloads the YANBF bootstrap title.
The runtime chain is:

```text
HOME menu → romm3ds forwarder → YANBF bootstrap → NTR_Forwarder → nds-bootstrap → game
```

Banner assets are mirrored under `sd:/3ds/forwarder/assets/` for offline use.
The [YANBF assets](https://github.com/YANBForwarder/assets) repository is the
primary source, with GameTDB and other artwork sources used as fallbacks.

### Game Boy Advance VC inject

A GBA ROM is embedded into a native AGB_FIRM Virtual Console CIA. The builder
uses the bundled template pieces in `romfs/gba/`, detects the save type using
the bundled database and patches the selected colour LUT before installing the
CIA to the HOME menu.

GBA ROMs remain on the SD card after installation because the app needs the
source ROM for artwork changes and reinstall operations.

## Documentation

- [GBA design and implementation notes](docs/GBA-PLAN.md)
- [Artwork and install UX](docs/ART-UX-SPEC.md)
- [Manage installed 3DS titles](docs/MANAGE-3DS-TITLES.md)
- [3DS installation detection](docs/3DS-INSTALL-DETECTION.md)
- [NDS forwarder detection](docs/NDS-FORWARDER-DETECTION.md)
- [Menu consistency model](docs/menu-consistency.md)
- [Performance investigation](docs/PERF-PLAN.md)
- [Responsiveness investigation](docs/RESPONSIVENESS-PLAN.md)

## Credits and licences

`romm3ds` is a fork of **NDSForwarder** and incorporates work from many
upstream projects:

- **[NDSForwarder](https://github.com/volkanturkut/NDSForwarder)** by Volkan
  Turkut, itself based on **[ndsForwarder](https://github.com/MechanicalDragon0687/ndsForwarder)**
  by RandalHoffman. GPL-3.0.
- **[YANBF](https://github.com/YANBForwarder/YANBF)** and
  **[YANBF assets](https://github.com/YANBForwarder/assets)** by lifehackerhansol.
- **[NTR_Forwarder](https://github.com/RocketRobz/NTR_Forwarder)**,
  **[nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap)** and
  **[TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu)** by the
  RocketRobz and DS-Homebrew teams.
- **[RomM](https://github.com/rommapp/romm)** by the RomM team.
- The forwarder DSiWare template from **Olmectron**'s Forwarder3-DS.
- **[makerom / ctrtool](https://github.com/3DSGuy/Project_CTR)**,
  **[bannertool](https://github.com/carstene1ns/3ds-bannertool)** and the
  **[devkitPro](https://devkitpro.org/)** toolchain.
- **[FBI](https://github.com/Steveice10/FBI)** for FS/AM usage patterns.

Vendored third-party source:

- [miniz](https://github.com/richgel999/miniz) — MIT.
- [stb_image](https://github.com/nothings/stb) — public domain / MIT.
- [nlohmann/json](https://github.com/nlohmann/json) — MIT.

This project is released under the **GPL-3.0**. See [LICENSE.md](LICENSE.md).
The upstream README is preserved as [README.upstream.md](README.upstream.md).
