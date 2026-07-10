# Changelog

All notable changes to romm3ds. Dates are the working days the changes landed.

## [Unreleased]

### Added
- Banner fallback to the ROM's own DS icon (rendered as a white "stamp") when a
  game has no YANBF asset and no GameTDB cover, instead of the plain template.

## [v1.0] — 2026-07-08

First tagged release. A self-contained RomM NDS client and on-device forwarder
manager for the 3DS, distributed as `.3dsx` (Homebrew Launcher) and installable
`.cia`. Built on the NDSForwarder (ndsForwarder) codebase.

### App
- **RomM Library** — browse the server's `nds` platform over HTTP Basic auth.
  Title, year, genres, rating and summary come from RomM metadata; `SELECT`
  searches the cached library instantly.
- **Download + install** — download a ROM (zip archives are extracted on-device,
  with progress and a `B` cancel), then build and install a YANBF-style CTR
  forwarder entirely on the console. No ~40-title DSiWare cap.
- **Install a whole folder at once** — SD Card Browser → "Install All".
- **Manage Installed** — every ROM in `sd:/roms/nds` with its forwarder state
  (romm3ds / TWL / YANBF) and title IDs; install a forwarder for a ROM that has
  none, or delete forwarder / ROM / both.
- **Settings** — installer options plus a dedicated RomM server screen (edit
  host / user / password individually, test the connection).

### Forwarder generation (on-device)
- CTR forwarder built by patching a prebuilt template CIA: per-game title IDs,
  ExeFS rebuilt with an SMDH icon from the DS banner, banner RGBA4444 box-art
  texture + CWAV sound swapped, all hashes recomputed, installed via AM.
- Box art sources match YANBF: SD-mirrored assets → YANBF assets over the
  network → GameTDB (http). Sound from YANBF assets by game code.
- Launch chain: payload writes the ROM path and chainloads the YANBF bootstrap
  TWL title via `aptSetChainloader` → NTR_Forwarder → nds-bootstrap → game.

### UI
- Flat dark design system; list with box-art rail, marquee titles and scrollbar;
  bottom details card with cover, chips, genre ticker and scrollable summary.
- Background cover prefetch/cache (raw RGBA on SD) so the UI never blocks on
  decode; box-filtered downscaling for sharp thumbnails.
- UTF-8 Latin-diacritic folding so accented titles render on the system font.

### Tooling
- `build.sh` builds the forwarder template, `.3dsx` and `.cia` in the devkitPro
  docker image, auto-fetching `makerom` / `bannertool`.
- `push.sh` clean-builds and FTPs the `.3dsx` to a 3DS running ftpd.

### Notable fixes during development
- CTR→TWL jump: `aptSetChainloader` + clean exit (raw `APT_DoApplicationJump`
  wedged NS on modern libctru).
- Forwarder ROM path must carry the `sd:` prefix for nds-bootstrap.
- Banner texture is RGBA4444 at a fixed offset in the CGFX (bannertool layout).
- HTTP client stack overflow (oversized on-stack buffer); chunked zip extract to
  keep the UI responsive; New3DS 804 MHz enabled for download/extract/hashing.

## [v0.1]

Initial fork of NDSForwarder 1.4.7: RomM NDS browser with Basic auth, download to
`sd:/roms/nds`, on-device forwarder install, and manage/delete.
