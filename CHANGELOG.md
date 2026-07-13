# Changelog

All notable changes to romm3ds. Dates are the working days the changes landed.

## [Unreleased] — the GBA + art release (app v0.2, 2026-07-11/12)

### Fixed (2026-07-13)
- **3DS library empty on RomM >= 4.9.2**: the server's list response now sends
  `files: []` (file lists are no longer inlined), so every multi-file 3DS game
  got dropped client-side. Multi-file roms are now listed immediately and the
  base `.cia` is picked lazily via a `/api/roms/{id}` detail fetch — during the
  title-id pass on library open, on the background refresh worker, or at
  install time as a fallback; the pick persists in the lib cache.
- **"Install failed: write 0xC8E083FC" on reinstall/rebuild** (AM "already
  exists", e.g. Change art twice in a row): installs now use
  `AM_StartCiaInstallOverwrite` when the title id is already installed —
  keeping the save data — instead of delete-then-install; a stale
  title/ticket/pending import that still triggers the error is cleared
  (title + ticket + pending) and the install retried once automatically.

### Added (2026-07-13)
- **Per-game screen filter** (Manage → GBA game → Filter): pick any of the five
  presets and the inject is rebuilt in place — same TID, HOME position, save
  data and stored art kept. Settings → "GBA screen" remains the default for
  new installs.

### Added — GBA support (hardware-verified)
- **GBA Virtual Console injects built on-device**: ROM baked into a native
  AGB_FIRM title (vcoven layout), streamed install, save-type detection via
  open_agb_firm's `gba_db.bin` (SHA1 → cart serial → version LUT; 174/174 on
  the real library). Library browse/download for the `gba` platform, Manage
  section, single-pass uninstall (title + ROM).
- **GBA screen presets** (Settings → "GBA screen"): the donor config block
  carries Nintendo's dark filter — a linear 60% darken LUT (white → 153).
  Presets bake a replacement video LUT into each install: **AGS-101 colors**
  (gamma 2.2→1.54, default), original dark filter, unfiltered, brighter gamma
  (2.2→1.7), night (AGS-101 + 3400K blue-light whitepoint).
- **Lid-close sleep combo**: injects set `sleepButtons` = L+R+SELECT, so games
  with a built-in sleep mode (NSUI sleep-patch convention) sleep on lid close.
- Injects carry an explicitly generated silent banner CWAV.

### Added — art layer (icons + banners, GBA & NDS)
- **Auto art on install**: SGDB icon on a strong name match (RomM metadata
  title first, then filename) with `.ico` assets preferred (native 48px
  frames), libretro clear-logo banner by exact No-Intro name; iiSU community
  box art / logo as the tier above the RomM-cover fallback. One notify dialog
  when art is missing ([Search] / [Use cover]); installs never end artless.
- **Art picker** (install "Choose art", on-SD and Manage "Change art"):
  bottom-screen thumb grid — SGDB icons/logos/grid capsules, libretro logo,
  iiSU assets, RomM cover — with X refine-search and Y icon/banner page swap.
- **art.json persistence** keyed by extension-less stem; reinstalls reuse
  choices; `[!]` marker in Manage while fallback art is in use.
- SteamGridDB over curl+mbedtls (TLS 1.2), key from `sgdb.env` or typed into
  Settings; libretro/RomM/iiSU art also fetched over curl.

### Changed
- **Unified dialog copy** across 3DS/GBA/NDS: "Install this game?",
  Install / Change art / Back, single Install/Uninstall/Art-update failure
  phrases; forwarder/inject/cia jargon removed from primary flows.
- **Coherent screens + busy feedback**: Manage system picker has its own
  bottom panel (no more RomM Library text), platform-correct empty states,
  and immediate "Uninstalling…/Downloading…/Installing…/Deleting…" status
  before every blocking action.
- **New brand**: pixel-cartridge icon + transparent 3DS-style banner with
  Pixelify Sans wordmark (RomM violet palette); SMDH description covers all
  three systems; app version 0.2 (also forces HOME to refresh the icon).
- NDS banner chain reordered: SD assets → YANBF → SGDB logo → iiSU logo →
  GameTDB box → notify → RomM cover → DS-icon stamp (boxes are fallbacks now).

### Fixed
- **Console freeze in the art picker**: GX display transfers need width ≥ 64 /
  height ≥ 16 — smaller thumb textures wedged the transfer engine (even
  Rosalina died). Textures are padded to the hardware minimum.
- **HOME icon cache**: reinstalls now bump the TMD/ticket title version, so a
  Change-art icon actually shows (HOME caches icons per tid+version).
- Solid-black donor template icon replaced with a generic GBA tile.
- Corrupt cached art (bit-flipped downloads) is purged and refetched once
  instead of poisoning the slot forever.
- RomM ≥ 4.9 renamed the rom-list filter to `platform_ids` — both params are
  sent now (4.4 servers ignored the new one and returned the whole library).
- soc/curl init moved to app boot; the cover-prefetch worker pauses while the
  foreground fetches art (shared httpc + SD contention).

### Earlier unreleased
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
