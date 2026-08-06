# romm3ds

[![Build 3DS artifacts](https://github.com/nitou2504/romm3ds/actions/workflows/build.yml/badge.svg)](https://github.com/nitou2504/romm3ds/actions/workflows/build.yml)

Install 3DS, Nintendo DS and Game Boy Advance games directly on the
console. Copy `.cia`, `.nds` or `.gba` files to the SD card, or connect an
optional [RomM](https://github.com/rommapp/romm) server. Install one game or a
whole folder; artwork can be selected automatically or chosen by you. DS
games also get matching banner art and sound, and artwork is cached for offline
use.

ZIP files are supported, but uncompressed files are recommended because ZIP
extraction takes longer on the console.

**Current release: `v1.1`** — on-console installation, artwork and sound,
GBA screen presets, folder installs and RomM support. Download it from the
[v1.1 release page](https://github.com/nitou2504/romm3ds/releases/tag/v1.1).

![romm3ds library, manage and HOME screens](docs/screens.png)

| Library | Manage | HOME menu |
|---|---|---|
| ![Library screen](docs/library.png) | ![Manage screen](docs/manage.png) | ![HOME screen](docs/home.png) |

## Features

- **Browse SD Card** — open `sd:/roms`, enter folders, return to the SD root
  and install games wherever they are stored. Use `Y` to mark several games
  and `R` to select all or none.
- **Install 3DS games** — install a supplied `.cia` to the HOME menu. The
  source CIA is deleted after installation by default because the installed
  title is the copy that remains on the console.
- **Install Nintendo DS games** — turn an `.nds` file into its own HOME-menu
  game with its icon, banner art and matching jingle.
- **Install Game Boy Advance games** — turn a `.gba` file into its own
  HOME-menu game with artwork and five built-in screen presets.
- **Artwork and sound** — automatically search several game-art sources for
  the best available cover, icon, banner and DS jingle. Chosen artwork can be
  changed later from Manage.
- **Manage installed games** — see storage usage, reinstall games, change art,
  remove ROMs, uninstall titles and remove 3DS updates/DLC where applicable.
- **Optional RomM Library** — browse the `3ds`, `nds` and `gba` platforms over
  HTTP Basic authentication. RomM 4.x and 5.x base-game responses are
  supported, and downloads use the same install flow as local files.
- **Settings** — configure RomM, language, GBA screen presets, artwork
  notifications and the delete-after-install policy.

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
checks the `.3dsx`, `.smdh` and `.cia`. Push a matching version tag such as
`v1.1` to create a GitHub Release with the individual files as raw downloads.
The separate Actions build artifact may still be presented as a ZIP because
that is how GitHub packages workflow artifacts; use the Release page for the
uncompressed files.

For hardware iteration over FTP:

```sh
./push.sh 192.168.0.23
```

This performs a clean 3DSX build and uploads it to `sd:/3ds/romm3ds.3dsx`.

## Release files

The release page contains these individual downloads:

- **`romm3ds.3dsx`** — the executable Homebrew Launcher build. Copy it to
  `sd:/3ds/` and launch it from the Homebrew Launcher.
- **`romm3ds.smdh`** — the 3DS metadata file. It contains the app title, author,
  description and icon used by 3DS homebrew tooling. It is not executable; the
  3DSX build already receives this metadata during packaging, but keeping the
  sidecar beside the 3DSX is useful for tools and launchers.
- **`romm3ds.cia`** — the installable HOME-menu title. Install it with FBI;
  this is the file to use when you want a normal icon on the HOME menu.
- **`SHA256SUMS`** — checksums for the three release files.

GitHub may also show automatic source-code ZIP/TAR links. Those are generated
by GitHub and are separate from the raw application assets above.

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
