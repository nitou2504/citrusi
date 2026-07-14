# Changelog

All notable changes to romm3ds. Dates are the working days the changes landed.

## [Unreleased] — the GBA + art release (app v0.2, 2026-07-11/12)

### Added (2026-07-14)
- **Per-game screen filter memory**: every GBA bake records its preset in
  art.json; reinstalls and art changes keep the game's preset instead of
  reverting to the Settings default, and the Screen picker tags/preselects
  the game's current one. Batch apply logs one result line per game.
- **Manage art setting** (Settings → "Manage art", default *title icons*):
  all Manage tabs show each installed game's own HOME icon — GBA injects and
  NDS forwarders included (their SMDH is read once on demand). Switch to
  *RomM covers* for the old look. Not-installed rows keep the cover.
- **Baked-banner preview**: GBA bakes save an untiled copy of the banner and
  the Manage details card shows it — exactly what HOME displays.
- Random title ID note: it applies to **NDS** TWL forwarders (rom hacks
  sharing the original's game code would collide) via the existing Settings
  toggle. GBA injects can't collide (per-filename title ids), and 3DS .cia
  files can't be re-id'd on device (the title id keys the NCCH encryption).

### Fixed (2026-07-14)
- **B goes back instantly**: every screen rebuild paused the cover worker by
  draining its in-flight download — B mid-fetch waited for the whole
  transfer. The pause now cuts the transfer (the job retries later, no false
  miss), and B to the Manage system picker skips the pause when the storage
  tally is cached. Art settle raised to 15 frames.
- **Manage → 3DS scroll**: the real culprit — a FULL AM enumeration of every
  SD title ran on each selection change (the update/DLC chip lookup). The SD
  title list is now RAM-cached (invalidated with the storage tally); the
  extras lookup is microseconds.
- **Slow app exit**: quitting joined the library-refresh worker mid-download
  (a GBA refresh runs 10s+). Workers are now abortable — the in-flight
  transfer is cut between chunks — so START/HOME exit is immediate.
- **Browse scroll**: the INSTALLED chip recomputed installed state every
  frame (GBA: path build + tid-owner walk at 60Hz) — now per selection
  change. Art settle raised to 12 frames so fast repeated presses never
  fire a load between steps; covers still being downloaded back off 30
  frames instead of stat()ing the SD every frame; the cover worker erases
  finished jobs instead of re-stat()ing hundreds of them on every pick.

### Changed (2026-07-13, night)
- **Scrolling never waits on art**: NDS Manage was fast, GBA slower, 3DS
  unbearable — ranked exactly by per-step art I/O. Covers and 3DS title
  icons now share one 8-frame settle debounce (rapid steps do zero SD work),
  seen covers live in a 16-texture RAM LRU (scrolling back is free), and
  title-icon reads are RAM-cached. Every list should feel like NDS Manage.
- **Screen-filter picker redesigned**: vertical list of the five presets
  (default tagged) with a plain-words explanation of the highlighted one
  underneath, instead of five cramped horizontal buttons.

### Fixed (2026-07-13, evening)
- **0xC8E083FC root cause found and fixed**: the new write-offset logging
  showed the cursed tid failing on the very first write with the title absent
  from the AM db; ftpd inspection found an orphan import dir on SD
  (`<id0>/<id1>/title/<high>/<low>` with a `00000000.ctx` import context +
  `content/*.app`) left by an aborted import — it blocks every future install
  of that tid. When db cleanup + retry still hits already-exists and AM
  confirms the title is not installed, the app now removes that orphan dir
  (the on-device equivalent of the GodMode9 folder-delete fix) and retries.

### Changed (2026-07-13, evening)
- **Manage → 3DS shows title icons only**: rows no longer borrow the RomM
  cover — the rail always draws the title's own HOME icon, and opening the
  screen no longer kicks the background library refresh or the cover
  prefetch worker (their SD reads fought the SMDH/tally pass and made the
  screen crawl once the 3DS library worked again). Names/year still come
  from the cached library.

### Fixed (2026-07-13)
- **3DS library empty on RomM >= 4.9.2**: the server's list response now sends
  `files: []` (file lists are no longer inlined), so every multi-file 3DS game
  got dropped client-side. Multi-file roms are now listed immediately and the
  base `.cia` is picked lazily via a `/api/roms/{id}` detail fetch — during the
  title-id pass on library open, on the background refresh worker, or at
  install time as a fallback; the pick persists in the lib cache.
- **"Install failed: write 0xC8E083FC" on reinstall/rebuild** (AM "already
  exists", e.g. Screen/Change art twice in a row): installs now use
  `AM_StartCiaInstallOverwrite` when the title id is already installed —
  keeping the save data — instead of delete-then-install; a stale
  title/ticket/pending import that still triggers the error is cleared
  (title + ticket + pending) and the install retried once automatically.

### Changed (2026-07-13)
- **GBA "Screen" action now opens a preset picker** (single + batch): the five
  presets, preselected on the Settings default, instead of silently re-baking
  with the global setting. Batch picks once and applies to all selected.

### Added — Manage → 3DS: all installed titles + storage (`feat/manage-3ds`)
- **Manage → 3DS now lists every installed application**, not only the ones
  that match a RomM library entry. Names come from each title's own SMDH (read
  from its ExeFS `icon` via `FSUSER_OpenFileDirectly`), sizes and versions from
  `AM_GetTitleInfo`. Games the library does know keep their cover and metadata
  and get an "on RomM" chip. Region variants the library can't match
  (Rhythm Megamix, Picross 3D R2 …) finally show up.
- **Sorted biggest first** — the screen is about reclaiming space; the details
  panel shows the installed size.
- **Uninstall + extras** — uninstalling a game that has an update (`0004000E`)
  or DLC (`0004008C`) installed offers to remove those too. This app, system
  titles, our own forwarders/injects and DSiWare are never listed or deleted.
- **Per-system storage** on the Manage system picker: bytes + counts for
  Nintendo DS (forwarders + DSiWare), Game Boy Advance (injects), Nintendo 3DS
  (apps), updates/DLC, plus SD free.
- SMDH names are cached on SD (`smdhnames.json`, keyed `tid|version`), so only
  the first open pays the one-FS-open-per-title pass ("Reading titles…", with
  the cover worker paused so it doesn't fight for the card).

### Added — storage cleanup + GBA screen on installed games
- **Duplicates row** in Manage → 3DS: `.cia` installer files whose game is
  already installed are pure duplicates of it (6.6 GB on the dev console).
  The row is tinted, reads "Duplicates - <size>", explains itself in the
  details panel, and deletes only those files — games are untouched. The
  storage panel gained an "Installer files (.cia)" row so this can't hide.
- **Storage rows now count the roms on the card** (`sd:/roms/...`): a
  forwarder title is a few hundred KB while the rom it launches is hundreds
  of MB, so the DS/GBA numbers were meaningless without them.
- **Every 3DS title has art**: its own 48×48 HOME icon is untiled out of the
  SMDH we already read, cached under `/titleicons`, and drawn on a white
  rounded plate with nearest filtering when the game isn't in the RomM library.
- **Per-game update/DLC sizes**: a row's size includes its update and DLC,
  with a chip and a "Game X + N update/DLC Y" breakdown (managing them
  individually is future work).
- **GBA "Screen" action** on installed injects (single + batch): re-bakes the
  title with the Settings screen preset, reusing its stored art — no picker,
  same TID, saves kept.

### Added — Manage every installed 3DS title + storage (`feat/manage-3ds`)
- **Manage → 3DS now lists every installed title**, not just RomM matches:
  names come from each title's own SMDH (ExeFS `icon`, English short title),
  cached on SD (`smdhnames.json`, keyed tid+version), sizes from a batched
  `AM_GetTitleInfo`. Sorted **biggest first** — the tab is for reclaiming
  space. Library matches keep their cover/metadata and get an "on RomM" chip.
  Our own forwarders/injects, DSiWare, system titles and the app itself are
  filtered out (they have their own tabs, and deleting them would be a foot-gun).
- **Uninstall any title**, with protected titles refused; when a game has an
  update (`0004000E`) or DLC (`0004008C`) installed, a `+ extras` button
  removes those too and shows their size.
- **Per-system storage panel** on the Manage system picker: count + size per
  system (Nintendo DS forwarders incl. YANBF/DSiWare, GBA injects, 3DS apps),
  updates/DLC when present, and SD free.
- First open reads uncached SMDHs behind "Reading titles… n/N" (later opens
  are instant); both Manage screens hold a `CoverCachePause` while scanning.

### Added — batch operations (`feat/batch-ops`, merged into `feat/integration`)
- **Multiselect with Y** on the RomM library and Manage lists (non-installable
  3DS rows are skipped); the header shows "N selected". Details-panel
  scroll-up moved from Y to L.
- **Batch install from the library** (START): one confirm with the summed
  download size, then **art first** — every GBA game's art (and its
  missing-art prompts) is resolved up front — followed by an unattended
  download/extract/install pass. B cancels the rest, failures don't stop the
  run, and the summary reports "Installed X of N — <size>" with any failures
  named in full.
- **Batch Manage** (START): the action dialog follows the selection —
  all-uninstalled offers *Install selected* / *Delete ROMs*, a mixed selection
  offers *Install* / *Uninstall*, all-installed keeps *Uninstall* / *Change
  art* (3DS: uninstall only). Batch install and batch change-art reuse the
  single-item helpers; both report a summary.
- Internals: the ~150-line `RommInstall` body was extracted into
  `installOneRomm()` (single-install behavior preserved byte-for-byte) with an
  `interactive` flag that suppresses notifies/error dialogs in the unattended
  phase.

### Added — Install from SD (`feat/local-install`, merged into `feat/integration`)
- The old SD-card browser became **Install from SD**: one screen listing local
  `.cia` (`sd:/cia`), `.nds` (`sd:/roms/nds`) and `.gba` (`sd:/roms/gba`)
  files, tagged `[CIA]`/`[NDS]`/`[GBA]`, with the same install-state markers.
  No RomM needed — drop files on the card and the app installs the .cia,
  builds the NDS forwarder or bakes the GBA inject, art pipeline included.
- Single install (A), Y-marks, **Install selected** / **Install all**, same
  art-first-then-unattended batch flow and summary.
- Cached RomM library entries are reused for title/cover when a filename
  matches (never forces a library load).
- Installed files are **hidden by default** (the rom dirs also hold every RomM
  download, so the list was the whole library); the heading reports how many
  are hidden and **X** toggles them back for Reinstall / Change art.

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
- **Installed-title names were all product codes (CTR-P-AMKE)**: the SMDH read
  asked for 0x288 bytes; FBI reads the whole 0x36C0 struct and requires the
  full byte count — the short read fails on retail titles (homebrew happened
  to work). Now a full read, title picked by system language with fallbacks,
  and failures log their Result. Fallback names are no longer cached, so a
  bad name can't become permanent.
- **Title icons never appeared**: a cached name short-circuited the SMDH read,
  which is also where the icon comes from. A missing icon now triggers the
  read too (backfill).
- **The duplicates row was unreachable**: it tested install state before
  `installed3dsRefresh()` had run, so nothing looked installed.
- **Backing out of a Manage tab was slow**: the storage tally was recomputed on
  every visit; it is cached again (installs/uninstalls invalidate it), and
  opening Manage shows "Opening Manage..." while the first sweep runs.
- **App crash mid-batch (data abort)**: `readEntireFile` called `fseek` on a
  NULL `FILE*` when an open failed. It now returns empty and logs the path;
  the GBA template pieces are read once at `CtrBuilder::initialize()` instead
  of on every build, and curl's connection cache is capped at 2 so the art
  phase can't hoard sockets (file descriptors) across five hosts.
- **GBA installs failed in any fresh clone/worktree**: `.gitignore`'s `*.bin`
  rule silently excluded `ncchheader/exheader/romfs/config_block/gba_db.bin`,
  so the app shipped with an empty GBA romfs (`open failed: romfs:/gba/...`).
  The romfs assets are now tracked (`!romfs/**/*.bin`).
- **Settings rows opening the RomM server screen**: the "GBA screen" row's id
  landed inside the server-row id range; the id moved out and the server
  branch is bounded to its own ids.
- **Multi-second freeze on Manage / Install from SD**: their AM+SD scans now
  pause the cover-prefetch worker (the same SD contention already handled for
  art fetches).
- Batch summaries: no `x` markers, full (word-wrapped) names under "Could not
  install:", and the installed size is reported.
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
