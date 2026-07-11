# GBA support — design & decisions

Status: **core shipped, hardware-verified (2026-07-11); art layer next.** The
on-device VC injector, save-type detection, and full GBA app integration work
on a New 3DS XL. What remains is the icon/banner art layer (see
[ART-UX-SPEC.md](ART-UX-SPEC.md) and the progress log below). Companion server
is the user's existing RomM instance.

## Progress log

Done (on `feat/gba`, verified on hardware):
- **SGDB TLS transport** — curl+mbedtls (TLS 1.2 ECDHE-ECDSA) reaches
  SteamGridDB where `httpc`/`sslc` (TLS 1.1) can't. `sgdb.cpp` client (search
  / icons / image fetch), key from `sd:/3ds/romm3ds/sgdb.env`. Hardware-tested
  end to end; **not yet wired to any UI** (that's the art layer).
- **VC inject builder** (`ctrbuilder.cpp buildGbaCIA`) — `.code` = ROM +
  AGB_FIRM `.CAA` footer; NCCH assembled from vendored vcoven template pieces
  (`romfs/gba/`, MIT); CIA shell reused from the forwarder template. Assembles
  to an SD temp file (ROM streamed, never 32 MB in RAM), installs via the
  proven `installCiaFromFile`. Boots on hardware.
- **Launch-crash fix** — TMD save-data size must be copied from the exheader
  (found by diffing a vcoven-built CIA with ctrtool); RomFS aligned to 0x1000.
- **Save-type detection** — 3 tiers: SHA1 → cart serial → version-string LUT,
  all from open_agb_firm's `gba_db.bin` (bundled, public). Serial tier catches
  translations/hacks. Benchmarked on the user's 174-game library: **174/174
  exact**, zero LUT guesses. Corrects real errors the naive/NSUI heuristic got
  wrong (EEPROM 4k vs 64k freezes, none→SRAM). Per-game override still TODO
  for ROMs not in the DB at all.
- **App integration** — `gba` slug, browse/download to `sd:/roms/gba/`,
  on-device zip extract (deterministic name), inject+install, library markers
  by AM install state, a Manage → Game Boy Advance section (install/uninstall),
  single-pass uninstall (title + ROM). Inherits the cached-first library +
  background-refresh perf work. Cached tid-ownership lookups fixed the GBA
  library open lag.

Not started:
- **Art layer** (icons + banners) for GBA *and* NDS — the whole
  [ART-UX-SPEC.md](ART-UX-SPEC.md) flow. Injects currently ship the donor
  template's placeholder icon + a silent banner. This is the next work item;
  see "Next: art layer" below.

## TL;DR decision

Build an **on-device GBA VC injector** — one native CIA per game with the ROM
baked in, installed via AM like our NDS forwarders. **Not** a forwarder.

Why: the only *forwarder* (thin, ROM-on-SD) route for GBA is open_agb_firm,
which requires **fastboot3DS or GodMode9 as FIRM0** (FCRAM boot). A standard
boot9strap + Luma + GM9-payload console (the current 3ds.hacks.guide setup)
**will not boot it**, and adding fastboot3DS is a FIRM0 NAND write with real
hardbrick risk (ntrboot-only recovery), unmaintained since 2019. Not worth it
for a cosmetic icon. VC injection needs **no console changes** and runs on the
real GBA hardware in the 3DS SoC (AGB_FIRM) — full speed, best compatibility.

## Native vs emulation (context)

| | Native (VC inject / open_agb_firm) | Emulation (mGBA) |
|---|---|---|
| Speed/compat | Real hardware, perfect 60fps, every game | ~full on New 3DS; mode7 racers dip 48–59 |
| Gyro/tilt (WarioWare Twisted, Yoshi) | ❌ not supported | ✅ real 3DS gyroscope |
| Save states / fast-forward / cheats | ❌ | ✅ |
| Rumble | ❌ (no 3DS has vibration hardware) | ❌ |
| RTC (Pokémon Gen 3) | ✅ | ✅ |
| Reboot to play | VC inject: no (HOME title) | no |
| On-device buildable | **yes (this plan)** | no — mGBA bakes path at compile time |

Recommendation to the user for daily use: VC injects (or open_agb_firm via the
Luma chainloader) for everything; keep mGBA around only for tilt games.

## Build architecture (on-device VC injector)

Reference implementation: **vcoven** (`vedoot/vcoven`, open-source Python GBA VC
injector) — the byte-level blueprint. ~80% overlaps our existing `ctrbuilder`.

### The content (`.code` in the ExeFS)
```
.code = [GBA ROM][config block 0x324][12 pad][2× section descriptor 0x10][.CAA footer 0x10]
```
- **config block** (0x324): save-performance settings + a 0x300-byte colour LUT.
  Copied verbatim from a template; patch only `romSize`@0x004 and
  `saveType`@0x008.
- **`.CAA` footer** (0x10): magic `.CAA`, active=1, offset to section table,
  nDesc<<4. Section table = ROM (type 0) + config (type 1). AGB_FIRM reads this.
- **Save type auto-detect**: scan the ROM for the standard signature strings —
  `FLASH1M_V`, `FLASH512_V`/`FLASH_V`, `EEPROM_V`, `SRAM_V`, else no-save. No DB.

### CIA assembly (reuses ctrbuilder)
- **NCCH header**: title ID @0x108/0x118, product code @0x150, **NoCrypto bit
  @0x18F |= 0x04**.
- **exheader**: title ID @0x1C8/0x200, and the **8-byte app-title tag @0x000
  must be non-empty** (derive from product code) — zeroing it silent-fails to
  launch.
- **ExeFS** packs, in order: `.code`, `banner`, `icon`, **`logo`**. `logo.darc.lz`
  is **required** or AGB_FIRM refuses to boot.
- **SMDH icon**: titles + 24×24 @0x2040 + 48×48 @0x24C0, RGB565 Morton-tiled —
  this is our existing `tileIcon` / `buildSmdh`.
- **Hashes**: exheader SHA @0x160, ExeFS superblock @0x1C0, RomFS @0x1E0 — our
  existing `ctrbuilder` hash rebuild.
- **Ticket / TMD / cert chain + AM install**: our existing `ctrbuilder` path.
- vcoven uses `3dstool`/`makerom` on PC; we assemble bytes in-memory on device
  exactly as we already do for NDS — no PC tools on the console.

### Template pieces (bundle in romfs, extract once from a donor GBA VC CIA)
`ncchheader.bin`, `exheader.bin`, `romfs.bin`, `config_block.bin` (0x324),
`icon.icn` (SMDH template), `logo.darc.lz`. Small, shared across all games.

### Memory
VC bakes the ROM in → CIA is ROM-sized. GBA ROMs are 4–32 MB (cap 32 MB =
Mother 3; user's library mostly 8–16 MB). New 3DS XL gives ~124 MB app RAM
(Old 3DS ~64 MB). Peak ~35 MB if we **stream** content into `AM_StartCiaInstall`
(hashing via the hardware SHA engine in a streaming pass) instead of holding the
whole CIA. Fits comfortably on New 3DS. Cost: each title is ROM-sized on SD and
counts against the 300-title HOME limit — so inject selectively, don't dump all.

## Art decisions

Keying: all sources key by the **No-Intro ROM filename**, which equals the RomM
`fs_name` — one lookup covers banner + icon.

### Banner (256×128) — decided
**libretro `Named_Logos`** — wide transparent clear-logo (~512×154), letterboxed
onto 256×128. HTTP, keyless, on-device fetchable. *(Verified 2026-07-11: plain
`http://` serves `200 OK image/png`, no HTTPS redirect — existing `httpc` path
works unchanged.)*
```
http://thumbnails.libretro.com/Nintendo%20-%20Game%20Boy%20Advance/Named_Logos/<No-Intro name>.png
```

### HOME icon (48×48) — decided source, fetched directly on-device
The nice square icons (like Cocoon/iiSU) come from **SteamGridDB's `icons`
category** — purpose-made square launcher icons, transparent PNG. iiSU uses
exactly this.
```
GET https://www.steamgriddb.com/api/v2/icons/game/{id}?mimes=image/png   (Bearer key)
     resolve name -> id via /search/autocomplete/{name}
     icon files served from https://cdn2.steamgriddb.com/icon/<hash>.png
```
Reality check on sizes: native 48×48 is rare. A 6-title GBA sample (Advance
Wars, Pokémon Emerald, Metroid Fusion, Mother 3, Golden Sun, WarioWare) all had
PNG icons, but dims range 16×16–1024×1024. So: keep `mimes=image/png` (the
category also holds `.ico` files), pick the smallest candidate ≥48px, downscale
to 48×48 on device.

**TLS: solved — software TLS, not `sslc`.** The 3DS `sslc` sysmodule tops out
at **TLS 1.1** with RSA-key-exchange suites and was never updated, while
SteamGridDB (Cloudflare) presents an **ECDSA-only** edge cert and requires
TLS ≥1.2 with ECDHE-ECDSA — so `httpc`/`sslc` can never handshake, and no
installed certificate fixes a protocol gap. The fix is to skip `sslc` entirely
and link **`3ds-curl` + `3ds-mbedtls`** (mbedTLS 2.28 = TLS 1.2 + ECDHE-ECDSA,
runs over plain `soc:u` sockets). Both portlibs ship **preinstalled** in the
`devkitpro/devkitarm` docker image we already build with — only Makefile link
flags needed. Precedent: Universal-Updater/3hs reach GitHub (also TLS 1.2
ECDHE-only) this way. Cert verification: bundle the GTS/ISRG roots as pinned
CAs, or `CURLOPT_SSL_VERIFYPEER=0` to match the app's existing
`SSLCOPT_DisableVerify` posture.

*Verified 2026-07-11* with the real RomM key under a pinned mbedTLS-2.28
profile (`--tls-max 1.2 --ciphers ECDHE-ECDSA-AES128-GCM-SHA256`):
autocomplete search, `/icons/game/{id}`, and cdn2 PNG downloads all succeed.
**Hardware-verified 2026-07-11** on the New 3DS XL (`sgdb.cpp` smoke build):
search, icon list, and cdn2 fetch over curl+mbedtls — byte-identical PNG.

**API key: user-supplied, never embedded.** The remaining blocker was only key
secrecy for public distribution. Delivery: read the key from a dotenv-style
file on SD (e.g. `sd:/3ds/romm3ds/sgdb.env`, `STEAMGRIDDB_API_KEY=...`) at
startup, with a swkbd prompt as fallback/first-run setup — same pattern as the
RomM host config. The user copies the key from their RomM compose file (RomM
does **not** expose `/icons` through its own API — only box covers — so going
through RomM instead is not an option).

Fallback tiers when SteamGridDB misses: RomM cover letterboxed onto a tile
(never cropped) → plain tile. ScreenScraper `support-2D` dropped for now (needs
dev creds on SD; revisit via the bridge alternative if ever wanted).

Rejected: cropping box art for the icon (user dislikes it). The RomM-cover
fallback letterboxes the full box onto the tile instead — recognizable, not
cropped — and is explicitly a "until I bother fixing it" state, marked ⚠ in
Manage.

### Sound — decided
No per-game GBA jingle library exists (unlike YANBF for NDS). Use one bundled
**silent/generic** banner WAV for all.

## Install UX & art picking (decided)

Full screen-by-screen spec: [ART-UX-SPEC.md](ART-UX-SPEC.md) (flows, screen
contents, persistence schema, implementation map). Summary below.

Guiding lesson from iiSU/Cocoon: nobody misses choice when defaults are good —
so choice lives NEXT TO the install path, never in it. Install stays one tap.
Verified on the real library (2026-07-11 batch test): ~85% of GBA and the
tested NDS titles auto-match SGDB strongly (→ ~90% after the normalization
fixes below); libretro GBA logos hit 11/11 exact No-Intro names.

### Name handling (verified against ES-DE source)
- **SGDB search query** — sanitize the fs_name stem exactly like ES-DE's
  scraper (`StringUtil::removeParenthesis`): iteratively erase `(...)`/`[...]`
  blocks, `_`→space, trim; plus our extras: flip the No-Intro article
  ("Legend of Zelda, The - X" → "The Legend of Zelda - X"), collapse spaces.
- **libretro banner URL** — the EXACT No-Intro stem, parens included (never
  sanitized). On 404, retry once with `(Translated)`/`[...]` tags stripped
  (verified: fixes "Mother 3 (Japan) (Translated)", "F-Zero - Climax [T-En]").
- **Match confidence** — normalize both sides (lowercase, alnum only, fold
  accents/macrons ō→o é→e, map &↔and, drop trailing platform tokens like
  "GBA"): exact = strong (auto-pick), prefix/contains = medium, else weak.

### The three entrances
```
TIER 1 · default (~90%): A → "Download + install?" → Yes → Installed!
        Strong match → SGDB icon + libretro banner, zero extra prompts.
TIER 2 · missing-art notify (bad names): see below — one dialog, only when
        icon and/or banner genuinely not found.
TIER 3 · Manage → game → "Change art": picker for taste fixes any time
        after install (rebuild in place, same TID → HOME position kept).
```

### Missing-art notify (at install, right after Yes, before the download)
Icon and banner failures report together in ONE dialog; install is never
blocked and never ends artless:
```
┌──────────────────────────────┐
│ Art not found:               │
│  icon:   no match for "..."  │   (lines shown only for the pieces
│  banner: not found           │    that actually failed)
│ [Search]   [Use RomM cover]  │
└──────────────────────────────┘
```
- **[Use RomM cover]** — build the missing pieces from the RomM box cover
  (600×900 portrait, IGDB): icon = cover letterboxed onto a 48×48 tile,
  banner = cover height-fit centered on 256×128 (same look as today's
  GameTDB NDS banners). Works even for badly named files because RomM's
  match is by rom id, not filename — the library has 2992/~all covers.
  Entry gets a ⚠ marker in Manage for later fixing.
- **[Search]** — swkbd prefilled with the sanitized name; one corrected name
  re-queries SGDB *and* libretro. Then the picker grid (icon), then banner
  candidates (auto-skipped for pieces already found).
- Settings toggle: "notify when art missing: on (default) / off (silent
  RomM-cover fallback)".

### Picker (one shared component, three entrances)
Bottom-screen grid (~5×3 thumbs, L/R pages), lazily downloaded via the
covercache async pattern, disk-cached under `sdmc:/3ds/romm3ds/cache/sgdb/`.
Controls: D-pad move, A use, B back/skip, X refine search (swkbd), Y switch
art page (icon ↔ banner) / source tab. Footer shows dims + count.
- GBA icon sources: SGDB icons → RomM cover tile.
- GBA banner sources: libretro logo → SGDB logos → RomM cover → tile.
- NDS icon: the ROM's own DS icon is the permanent default (always exists,
  RomM/network never involved); SGDB icons offered as optional override.
- NDS banner sources: SD assets/GameTDB (existing chain) → SGDB logos →
  RomM cover → DS-icon stamp. (libretro Named_Logos is empty for NDS —
  verified, even Mario Kart DS 404s — excluded.)

### Persistence
`art.json` next to the TID files in the forwarder config dir, keyed by
fs_name: chosen SGDB game id + icon/banner ids (or "romm-cover"/"stamp"
markers). Reinstalls reuse it silently; ⚠ in Manage = fallback art in use.
"Change art" rebuilds only the CIA/forwarder — ROM untouched, ~20s.

## Alternative: icon bridge (server-side, sits next to RomM) — demoted

Superseded by the direct curl+mbedtls path above for personal use. Keep in the
back pocket only if: (a) public release where users shouldn't need their own
SGDB key, (b) ScreenScraper fallback (its dev creds shouldn't live on SD
either), or (c) server-side caching/normalization becomes worth a container.
Note the icon-picker goal never required direct TLS — a bridge could serve
candidate lists as JSON + proxied images over plain HTTP too.

~100-line microservice on g3 alongside the RomM Docker stack. Holds the
SteamGridDB key (RomM already has one — `STEAMGRIDDB_API_ENABLED: true`), serves
the 3DS plain HTTP:
```
GET http://<bridge>:PORT/gba-icon?name=<No-Intro name>        (or ?crc= / ?sgdb_id=)
  1. resolve name -> SGDB id            (/search/autocomplete, Bearer, server-side)
  2. GET /icons/game/{id}?mimes=image/png -> pick best size
  3. fallback: ScreenScraper jeuInfos systemeid=12 + crc -> media=support-2D
  4. normalize -> pad/fit to 48x48 PNG (flatten onto a tile if transparent)
  5. cache on disk (key by id/hash); serve image/png over HTTP
```

Alternative if any inject step runs on a PC: bake the SteamGridDB icon straight
into the SMDH at build time (Option B) — highest fidelity, zero 3DS networking.

## Next: art layer (NOT STARTED — the remaining work)

Everything below is documented in full in [ART-UX-SPEC.md](ART-UX-SPEC.md);
this is the build-order summary. Goal: replace the placeholder inject icon and
silent/blank banner with real art, via the shared picker + auto/notify flow.

Build order:
1. **`sgdb.cpp` → UI glue.** The client works; add: sanitizer (`artquery`),
   `norm()` confidence scoring, on-disk cache under
   `sdmc:/3ds/romm3ds/cache/art/`. First consumer: bake the chosen 48×48 into
   the GBA SMDH (`buildGbaCIA` already accepts an `icon48` arg — currently
   passed `""`).
2. **libretro banner fetch** (GBA) over the existing `httpc` — exact No-Intro
   name, 404-retry with tags stripped. Feeds `buildGbaCIA`'s `bannerTex` arg.
3. **Auto path + missing-art notify** (screens S1/S2 in the spec): strong SGDB
   match → silent; weak/none → the one notify dialog with `[Search]` /
   `[Use RomM cover]`. Wire into `RommInstall`.
4. **Picker** (screen S4): bottom-screen thumb grid, reuse the covercache
   async pattern; `X` refine, `Y` icon/banner page. Shared by GBA + NDS.
5. **`art.json` persistence** + the ⚠ Manage marker; **Change art** in Manage
   (rebuild in place — GBA re-bakes the CIA, NDS rebuilds the forwarder).
6. **NDS side**: same picker, but the SMDH icon stays the ROM's own DS icon;
   only the 256×128 banner is user-choosable. SGDB `logos` category as a new
   banner source alongside the existing GameTDB chain.

Deferred / open:
- **Per-game save-type override** — for the rare ROM not in `gba_db.bin` at
  all (none in the current library). Fits naturally in the same pre-install
  options surface as the art picker. See the save-type discussion above.
- **Save handling on hardware** — AGBSAVE / SD `.sav` round-trip behaviour
  across uninstall/reinstall not yet exercised end to end.
- **HOME 300-title limit** awareness when batch-injecting many GBA games
  (each inject is a full title, ROM-sized on SD).

## References

- vcoven (VC inject blueprint): https://github.com/vedoot/vcoven
- agb_edit (`.CAA` footer + save types): https://github.com/joemck/agb_edit
- open_agb_firm (native loader): https://github.com/profi200/open_agb_firm
- open_agb_firm forwarder (needs fastboot3DS FIRM0): https://github.com/ismailhasannnnnn/OPEN_AGB_FIRM_Forwarder
- mGBA 3DS forwarder (compile-time path): https://github.com/HeyItsJono/mgba-3DS-Forwarder
- NSUI (PC VC injector, closed): https://3ds.eiphax.tech/nsui
- libretro GBA thumbnails: https://github.com/libretro-thumbnails/Nintendo_-_Game_Boy_Advance
- SteamGridDB API: https://www.steamgriddb.com/api/v2 (icons category)
- ScreenScraper API: https://api.screenscraper.fr/api2/ (systemeid=12, support-2D)
- iiSU (SGDB home-icon scraping precedent): https://github.com/iisu-network/iiSU
- 3dbrew GBA VC / AGBSAVE: https://www.3dbrew.org/wiki/3DS_Virtual_Console
- fastboot3DS (FIRM0, brick risk): https://wiki.hacks.guide/wiki/3DS:Fastboot3DS
- 3DS TLS limitation: https://github.com/MrNbaYoh/3ds-ssloth
