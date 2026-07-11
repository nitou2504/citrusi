# GBA support — design & decisions

Status: **planned / not started.** This captures the research and the chosen
approach for adding GBA HOME-menu entries to romm3ds, so the build can start
from a settled design. Companion server is the user's existing RomM instance.

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
onto 256×128. HTTP, keyless, on-device fetchable.
```
http://thumbnails.libretro.com/Nintendo%20-%20Game%20Boy%20Advance/Named_Logos/<No-Intro name>.png
```

### HOME icon (48×48) — decided source, needs a bridge
The nice square icons (like Cocoon/iiSU) come from **SteamGridDB's `icons`
category** — purpose-made square launcher icons, native 48×48, transparent PNG.
iiSU uses exactly this.
```
GET https://www.steamgriddb.com/api/v2/icons/game/{id}?dimensions=48x48&mimes=image/png   (Bearer key)
     resolve name -> id via /search/autocomplete/{name}
```
**Blocker:** SteamGridDB (HTTPS + secret Bearer) and ScreenScraper (HTTPS + dev
creds) can't be fetched from the 3DS — its TLS is obsolete and we can't embed a
secret. RomM does **not** store square icons (only box covers; its SteamGridDB
integration pulls grids/capsules, never `/icons`). libretro has no icon folder.
No keyless-HTTP square-icon source exists.

**Chosen delivery — a small RomM-sidecar icon bridge** (see below). Fallback
tiers when SteamGridDB misses: ScreenScraper `support-2D` (GBA cartridge-label
scan) → libretro `Named_Logos` composited on a GBA-cartridge tile → RomM cover.

Rejected: cropping box art for the icon (user dislikes it). GBA box art is square
(512×512) so it *works* as a last-ditch fallback, but it's not the goal.

### Sound — decided
No per-game GBA jingle library exists (unlike YANBF for NDS). Use one bundled
**silent/generic** banner WAV for all.

## Icon bridge (server-side, sits next to RomM)

~100-line microservice on g3 alongside the RomM Docker stack. Holds the
SteamGridDB key (RomM already has one — `STEAMGRIDDB_API_ENABLED: true`), serves
the 3DS plain HTTP:
```
GET http://<bridge>:PORT/gba-icon?name=<No-Intro name>        (or ?crc= / ?sgdb_id=)
  1. resolve name -> SGDB id            (/search/autocomplete, Bearer, server-side)
  2. GET /icons/game/{id}?dimensions=48x48,64x64,128x128&mimes=image/png -> pick best
  3. fallback: ScreenScraper jeuInfos systemeid=12 + crc -> media=support-2D
  4. normalize -> pad/fit to 48x48 PNG (flatten onto a tile if transparent)
  5. cache on disk (key by id/hash); serve image/png over HTTP
```
This mirrors how the app already fetches RomM covers, and how iiSU gets its home
icons — just moved server-side to satisfy the 3DS transport limits. It's a clean,
self-contained first deliverable, buildable/testable independent of the 3DS.

Alternative if any inject step runs on a PC: bake the SteamGridDB icon straight
into the SMDH at build time (Option B) — highest fidelity, zero 3DS networking.

## Open questions / next steps

1. Extract the six template pieces from one GBA VC donor CIA; verify a hand-built
   inject boots on the New 3DS XL.
2. Prototype the `.code` + `.CAA` builder and the streaming AM install (32 MB
   worst case).
3. Build the icon bridge (standalone; RomM sidecar container).
4. Wire GBA into the app: platform pick (`gba` slug), library browse/download to
   `sd:/roms/gba/`, inject, manage. Decide on a per-game menu to choose
   inject-now vs skip (HOME-limit awareness).
5. Save handling: AGBSAVE / SD `.sav` behaviour to confirm on hardware.

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
