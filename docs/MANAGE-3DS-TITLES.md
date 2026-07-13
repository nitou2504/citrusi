# Manage → 3DS: list ALL installed titles (DONE, 2026-07-12)

Shipped on `feat/manage-3ds`. What landed:

- `installedtitles.{hpp,cpp}` — AM enumeration + `TitleKind` classification,
  SMDH name reader with an SD name cache (`sd:/3ds/forwarder/smdhnames.json`,
  keyed `tid|version`), `listInstalledApps()`, `findTitleExtras()`,
  `computeStorageTally()`.
- Manage → 3DS lists **every** installed app on SD, biggest first, with the
  installed size in the details panel. Library matches keep their cover /
  metadata and get an "on RomM" chip; the rest are named from their SMDH.
  Our own forwarders/injects, DSiWare, system titles and the app itself are
  filtered out.
- Uninstall works on any listed title and offers **+ extras** when an update
  (`0004000E`) or DLC (`0004008C`) with the same unique id is installed.
- The Manage system picker's bottom panel shows bytes + counts per system
  (DS forwarders / GBA injects / 3DS apps / updates+DLC) and SD free.
- Deferred: A-Z sort toggle, "orphaned update/DLC" cleanup for games that are
  no longer installed, and using SMDH names for the region-variant install
  detection described in 3DS-INSTALL-DETECTION.md.

## Original request and plan (kept for context)

## Request (2026-07-11)

Manage → 3DS currently shows only installed titles that match a RomM library
entry (by cia title id). The user also wants to see and manage the rest of
his installed 3DS games there:

- games not on RomM at all (Bravely Default, another Picross, …)
- region-variant installs the library can't match (Rhythm Megamix `18A400`
  vs server `18A500`, Picross 3D R2 `187D00` vs `187E00` — see
  3DS-INSTALL-DETECTION.md)

Wanted per title: name, **size**, and **uninstall** — same actions the
library-matched entries already have.

## Implementation sketch

Extend `generateManageMenu` (3DS branch):

1. **Enumerate**: `AM_GetTitleList(MEDIATYPE_SD)`, keep `hi == 0x00040000`
   (apps). Exclude the app's own forwarder titles:
   - YANBF range `0FF40000–0FF7FFFF`
   - romm3ds CTR forwarders (`getRommCtrForwarders` tids + `0FF3FF00` self)
2. **Size/version**: `AM_GetTitleInfo` → `AM_TitleEntry.size` (also gives
   version — could show `v1.1`).
3. **Names**: read each title's SMDH — the ExeFS `icon` IS readable from
   userland (`FSUSER_OpenFileDirectly`, archive `0x2345678A`, archPath
   `{lo, hi, media, 0}`, filePath `{0,0,2,'icon',0}`; verified rc=0 during
   the YANBF investigation). SMDH short title (English block) = display
   name. Cache `tid → name` in a json (e.g. `smdhnames.json`) so this costs
   one pass ever; invalidate entry on uninstall/install.
4. **Merge**: entries matched to the library (current behavior) keep their
   cover art + RomM metadata; unmatched titles get plain entries with the
   SMDH name + size + tid.
5. **Actions**: same ManageRom uninstall flow (`AM_DeleteTitle` +
   `AM_DeleteTicket`), which already exists — just stop requiring a library
   match to reach it.
6. Optional: `* on RomM` chip for matched entries; updates (`0004000E`) and
   DLC (`0004008C`) of an uninstalled game could be offered for cleanup too.

## Perf note

SMDH reads are one FS open per title (~30 titles) — do the first pass behind
the existing loading screen or the background worker, then rely on the name
cache. Fits the PERF-PLAN "cached-first + background refresh" pattern.

## Bonus

The same SMDH name source enables the install-detection fallback described
in 3DS-INSTALL-DETECTION.md (mark region variants as installed by name).

---

## Implementation plan (agent-researched, 2026-07-12 — ready to build)

Verified foundations: FBI-style SMDH name reads work via FSUSER_OpenFileDirectly
(archive 0x2345678A, filePath {0,0,2,'icon',0}; English short title at 0x208);
AM_GetTitleInfo batch-fills sizes/versions; our UID ranges classify ours
(YANBF 0xFF400+, fwd 0xFF800+, GBA 0xFFC00+, app itself 0xFF3FF = protected).

Steps (see conversation log / agent report for full detail):
1. New module installedtitles.{hpp,cpp}: enumerate SD apps, classify TitleKind,
   SMDH name reader with smdhnames.json cache keyed tid|version, StorageTally.
2. Manage->3DS lists ALL apps (merge RomM matches for covers/names).
3. First pass behind "Reading titles..." loading (cache makes later passes instant).
4. Uninstall any title + protection (self/system) + optional update/DLC cleanup.
5. Storage breakdown on the Manage system-picker bottom panel (per-system bytes
   + SD free, humanSize).
6. Sort by size desc (default) with A-Z toggle.
Effort ~12-17h. Pitfalls documented: no-SMDH titles, media types, DSiWare has
banners not SMDH, orphaned updates/DLC, cover-worker SD contention (pause it).


---

## Status: implemented (2026-07-12, branch feat/manage-3ds -> feat/integration)

Built as planned: `installedtitles.{hpp,cpp}` (enumerate + classify + SMDH
names + `smdhnames.json` cache + storage tally), Manage->3DS listing every
installed app biggest-first, uninstall with protection and an optional
`+ extras` (update/DLC) step, and the storage panel on the Manage system
picker. Build-verified; not yet exercised on hardware.

Deferred:
- A-Z sort toggle (size-desc is the only order today).
- Batch uninstall doesn't offer the `+ extras` step (single uninstall does).
- Orphaned update/DLC cleanup for games that are no longer installed.
