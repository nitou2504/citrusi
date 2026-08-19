<div align="center">
  <h1><img src="icon.png" width="48" alt="Citrusi logo" style="vertical-align: middle;"> citrusi</h1>
  <p>
    <strong>Install 3DS, DS and GBA games directly on your console with nice looking art!</strong><br>
    Build and install everything directly on your 3DS.
  </p>
  <p>
    <a href="https://github.com/nitou2504/citrusi/releases/tag/v1.2.0"><img src="https://github.com/nitou2504/citrusi/actions/workflows/release.yml/badge.svg" alt="Release build"></a>
  </p>
  <p>
    <a href="https://github.com/nitou2504/citrusi/releases/tag/v1.2.0"><strong>Download v1.2.0</strong></a>
    · <a href="#quick-start">Quick start</a>
    · <a href="#build-from-source">Build from source</a>
  </p>
  <p align="center">
    <img src="docs/home-pokemon-pinball.jpg" width="250" alt="Pokémon Pinball installed on the 3DS HOME menu">
  </p>
  <p><em>Pokémon Pinball HOME-menu title.</em></p>
</div>

## What is citrusi?

A 3DS Homebrew app to install 3DS, NDS, GBA games to your console's Home-menu including art. 

To use it just copy `.cia`, `.nds` or `.gba` files to the SD card (or connect to your
[RomM](https://github.com/rommapp/romm) instance). Install one game or a whole
folder. Artwork (Icon and Banner) is scraped automatically (or chosen per
game). DS games also get a jingle from the
[YANBF sounds repo](https://github.com/YANBForwarder/assets).

ZIP files are supported, but uncompressed files are recommended to speed up
the installs.

## App screenshots

<table align="center">
  <tr>
    <td align="center" valign="top">
      <img src="docs/home.png" width="210" alt="Current citrusi base screen"><br>
      <strong>citrusi</strong>
    </td>
    <td align="center" valign="top">
      <img src="docs/search.png" width="210" alt="Current RomM search screen"><br>
      <strong>RomM search</strong>
    </td>
    <td align="center" valign="top">
      <img src="docs/manage.png" width="210" alt="Current Manage Installed screen with sizes"><br>
      <strong>Manage</strong>
    </td>
  </tr>
</table>

<p align="center"><strong>Art selection</strong> — icon and banner pickers</p>

<table align="center">
  <tr>
    <td align="center" valign="top">
      <img src="docs/gba-art-icon.png" width="210" alt="GBA icon art selection screen"><br>
      <strong>Icon</strong>
    </td>
    <td align="center" valign="top">
      <img src="docs/gba-art-banner.png" width="210" alt="GBA banner art selection screen"><br>
      <strong>Banner</strong>
    </td>
  </tr>
</table>

## Features

- **Browse SD Card** — open `sd:/roms` and choose one of the three exposed
  folders: `3ds`, `nds` or `gba`.
- **Install 3DS games** — install a supplied `.cia` to the HOME menu.
- **Install DS games** — turn an `.nds` file into its own HOME-menu
  game with its icon, banner art and jingle.
- **Install GBA games** — turn a `.gba` file into its own
  HOME-menu game with artwork (and optionally choose screen presets).
- **Artwork** — automatically search several online sources for icon and banner
  art.
- **Manage installed games** — see storage usage, change art, uninstall titles
  or remove 3DS updates/DLC to free up space.
- **RomM Integration** — browse and install from your `3ds`, `nds` and `gba`
  RomM library. RomM 4.x and 5.x are
  supported.


## Quick start

Before starting, your 3DS must already have Luma3DS CFW. The Homebrew Launcher
is only needed if you use the `.3dsx` version instead of installing the CIA.

### 1. Install the application

Choose one format:

- **CIA:** install `citrusi.cia` from Universal Updater (or with FBI). This
  creates a normal HOME-menu app.
- **3DSX:** copy `citrusi.3dsx` to `sd:/3ds/` and launch it from the Homebrew
  Launcher.

### 2. Install the YANBF bootstrap title

Download [`bootstrap.cia`](https://github.com/YANBForwarder/YANBF/releases/latest)
from the YANBF release page and install it with FBI. This title is required for
NDS HOME-menu forwarders. You may delete the CIA after it is installed.

### 3. Install the NDS runtime files

Download
[`DS.Game.Forwarder.pack.nds-bootstrap.7z`](https://github.com/RocketRobz/NTR_Forwarder/releases/latest)
from the NTR_Forwarder release page. Open the archive's `for SD Card root`
folder and copy its contents to the root of the SD card. Verify that these
files exist:

```text
sd:/_nds/ntr-forwarder/sdcard.nds
sd:/_nds/nds-bootstrap-release.nds
sd:/_nds/nds-bootstrap-hb-release.nds
sd:/_nds/nds-bootstrap.ini
sd:/_nds/ntr_forwarder.ini
sd:/_nds/release-bootstrap.ver
```

The full TWiLight Menu++ application is not required by citrusi when these
NTR_Forwarder and nds-bootstrap files are present.

### 4. Add local games

The local SD browser exposes only these folders:

- `sd:/roms/3ds` — `.cia` files
- `sd:/roms/nds` — `.nds` and supported ZIP archives
- `sd:/roms/gba` — `.gba` and supported ZIP archives

> **Scope:** The local browser does not show arbitrary folders outside
> `/roms`. Put each file in the matching platform folder.

Open **Browse SD Card**, choose a platform, and press **A** on a game. Use
**Y** to mark several games and **R** to select all or none.

### 5. SGDB key (optional but recommended)

- Get a free API key from **https://www.steamgriddb.com/profile/preferences/api**.
- Save your key in `sd:/3ds/citrusi/sgdb.env` or input it manually in Settings.

Existing installations keep reading saved SteamGridDB keys and cached artwork.

### 6. Connect RomM (optional)

To use RomM, open **Settings**, enter the server address, username and password.

Internet access is optional for local installation, but is needed for RomM,
online artwork, and DS jingles. The citrusi templates are bundled in the app;
no separate template or artwork files are required.

## Manage and customize

From **Manage Installed**, you can:

- see installed titles and storage usage
- change icons, banners, and GBA screen filters
- uninstall games
- remove 3DS updates and DLC

**Settings** controls the RomM connection, GBA defaults, missing-art prompts,
art display preferences, and the delete-after-install policy.


## Art Sources

Artwork is scraped in the following order:

1. **SteamGridDB** *(recommended)*
   - Uses the API key from `sd:/3ds/citrusi/sgdb.env` or input it in Settings.
2. **libretro Named Logos** *(GBA banners only)*
   - Requires an exact No-Intro ROM filename match.
3. **iiSU**
   - Uses exact game names.
   - No API key required.
4. **YANBF assets**
   - For NDS banner and jingle.
5. **RomM covers**
   - Final fallback for icons and banners from your configured RomM library.

> **Note:** SteamGridDB is always preferred when an API key is configured. For GBA banners, the libretro logo is checked before iiSU and RomM covers.

## How each format works

### 3DS

A `.cia` is streamed into the 3DS title database with
`AM_StartCiaInstall`. Each game has its icon, banner and sound.

### Nintendo DS

An NDS ROM is installed as its own HOME-menu forwarder with artwork and sound.
It launches the game from the SD card through the NDS runtime.

### Game Boy Advance

A GBA ROM is packaged as a native Virtual Console CIA and installed as its own
HOME-menu game. It runs on the 3DS's GBA hardware, not an emulator.

GBA ROMs remain on the SD card after installation because the app needs the
source ROM for artwork changes.

## Build from source

Builds require Docker and internet access on the first run so `makerom` and
`bannertool` can be downloaded.

```sh
./build.sh              # template + 3DSX + CIA
./build.sh 3dsx         # template + 3DSX
./build.sh cia          # template + 3DSX + CIA
./build.sh template     # forwarder template only
```

Generated files are written at the repository root and are ignored by Git:

```text
citrusi.3dsx
citrusi.smdh
citrusi.elf
citrusi.cia
```

GitHub Actions builds the same files. Version tags create releases with the
CIA, 3DSX, SMDH, and checksum files.

### Hardware iteration over FTP

```sh
./push.sh 192.168.0.22 # current 3DS IP; change it if DHCP assigns another address
```

This performs a clean 3DSX build and uploads it to
`sd:/3ds/citrusi.3dsx`.

## Credits and licenses

`citrusi` started as a fork of **NDSForwarder** and incorporates work from
many upstream projects:

- **[NDSForwarder](https://github.com/volkanturkut/NDSForwarder)** by Volkan
  Turkut, itself based on
  **[ndsForwarder](https://github.com/MechanicalDragon0687/ndsForwarder)** by
  RandalHoffman. GPL-3.0.
- **[YANBF](https://github.com/YANBForwarder/YANBF)** by lifehackerhansol — the
  `bootstrap.cia` title and the YANBF forwarder launch chain used for NDS titles.
- **[YANBF assets](https://github.com/YANBForwarder/assets)** by lifehackerhansol —
  the DS banner artwork and jingles used by the artwork pipeline.
- **[NTR_Forwarder](https://github.com/RocketRobz/NTR_Forwarder)** by RocketRobz —
  the DS Forwarder Pack distributed with this project as an NDS runtime
  dependency.
- **[nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap)** and
  **[TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu)** by RocketRobz
  and the DS-Homebrew team — the runtime ecosystem used to boot NDS games.
- **[RomM](https://github.com/rommapp/romm)** by the RomM team.
- The forwarder DSiWare template from **Olmectron**'s Forwarder3-DS.
- **[makerom / ctrtool](https://github.com/3DSGuy/Project_CTR)**,
  **[bannertool](https://github.com/carstene1ns/3ds-bannertool)**, and the
  **[devkitPro](https://devkitpro.org/)** toolchain.
- **[FBI](https://github.com/Steveice10/FBI)** for FS/AM usage patterns.

Vendored third-party source:

- [miniz](https://github.com/richgel999/miniz) — MIT.
- [stb_image](https://github.com/nothings/stb) — public domain / MIT.
- [nlohmann/json](https://github.com/nlohmann/json) — MIT.

This project is released under the **GPL-3.0**. See [LICENSE.md](LICENSE.md).
The upstream README is preserved as [README.upstream.md](README.upstream.md).
