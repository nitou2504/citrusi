# Speed-up plan — instant navigation + background refresh

Goal: every screen opens instantly from cached data; anything slow (network,
AM/FS scans) runs in the background with an "updating…" hint, then the screen
refreshes in place.

## Where the time actually goes today

### Library open (NDS / 3DS)
- SD json cache (`lib_<slug>.json`) already makes reopening fast, **but**:
  - First open per session also pays `refreshNdsForwarders()` →
    `scanManagedRoms()` (see below) — even when opening the *3DS* library
    (`buildRommMenu` always calls it).
  - 3DS: `resolveTitleIds()` blocks with a loading screen when any rom lacks
    a cached title id (one HTTP header fetch per rom).
  - Server refresh is all-or-nothing: "Refresh from server" wipes both caches
    and refetches synchronously.

### Manage → NDS (the slow one)
`scanManagedRoms(ROMM_NDS_DIR)` per open (session-cached only for YANBF):
1. `computeForwarderTID()` × ~108 roms — fopen + 3 seek/reads **per .nds
   file**, every time. Dominant SD-latency cost.
2. Logger: **every log line reopens/closes `log.txt`** — the scan logs
   ~140 lines → ~140 extra SD file opens. Hidden multiplier on everything.
3. `getYanbfForwarders()` first call: 23 × `romfsMountFromTitle` that we now
   KNOW always fail with `0xD9004676` + full `sd:/cias` walk.
4. Diagnostic AM title dump (investigation leftover) — AM lists twice + ~30
   log lines.
5. Cover-art match: 108 roms × linear scan of the NDS lib cache with
   `toLowerCase`/path ops per pair (O(n²) CPU).

### Manage → 3DS (fast — reference point)
`installed3dsRefresh()` = two AM title lists + set lookups. That's the level
everything else should feel like.

## Plan

### Phase 1 — cheap wins, no architecture change
1. **Logger: keep the file handle open** (static `std::ofstream` append,
   flush per line). One-line class change, speeds up every screen.
2. **Delete the diagnostic title dump** in `scanManagedRoms` and demote the
   per-rom log lines to `debug()`.
3. **Skip doomed romfs mounts**: try `romfsMountFromTitle` on the *first*
   YANBF-range title only; if it returns `0xD9004676`, skip the mount for the
   rest (straight to the cia-scan fallback).
4. **Persist a TWL-tid cache**: `sd:/3ds/forwarder/ndstids.json` mapping
   `filename|size` → computed tid. `computeForwarderTID` only reads headers
   for new/changed files. First run unchanged, every later run ~0 SD reads.
5. **Session-cache the whole `scanManagedRoms` result** (like the YANBF
   cache; invalidate on any install/delete/rom-delete). Reopening Manage→NDS
   within a session becomes instant.
6. **Index the cover-match**: build `map<lowercased fsName → RommRom*>` once
   per scan instead of the nested loop.

### Phase 2 — background refresh (one worker, "updating…" hint)
Reuse the cover-worker pattern (`covercache.cpp`): one background thread +
job queue; **all httpc and AM/FS work for refreshes goes through it** so
there's no concurrent-session problem (title-id resolve already had to dodge
this). Main thread polls a job flag each frame.

UI contract:
- Screens always render immediately from cache/snapshot.
- While a refresh job for the visible screen runs, draw a small "updating…"
  chip in the heading.
- When the job lands, rebuild the menu in place, preserving selection.

Jobs:
1. **Library refresh** (per slug): on library open, show cached list, queue
   `findPlatform + listRoms`; on completion diff against cache → if changed,
   `saveLibCache` + rebuild menu. "Refresh from server" stops being a wipe —
   it just forces the same job.
2. **3DS title-id resolve**: fold into the same job (after listRoms), instead
   of the blocking "Reading title ids…" screen.
3. **Manage NDS scan**: persist last scan as
   `sd:/3ds/forwarder/manage_nds.json` (display/tids/orphan per rom). Open =
   render snapshot instantly, queue a real rescan, swap when done. With
   Phase 1 items 3–5 the rescan itself drops to: AM lists + dir listing +
   headers of *new* roms only.
4. **Forwarder markers for the library** (`refreshNdsForwarders`): consume
   the same manage snapshot/job instead of calling `scanManagedRoms`
   synchronously inside `buildRommMenu` — first library open stops paying the
   manage cost. Markers pop in when the job lands.

### Sequencing / risk
- Phase 1 first (measurable alone, zero UI risk). Add `osGetTime()` timings
  around the scan phases behind `#ifdef DEBUG` to confirm the wins on device.
- Phase 2 lands screen by screen: library first (pattern already proven by
  the cover worker), then manage snapshot.
- Thread rules: worker owns httpc + AM during a job; UI thread only reads
  completed results (double-buffered vectors + `LightLock`/atomic flag —
  same discipline as `coverCacheStart`).

### Non-goals
- No change to install/delete flows (already interactive).
- No speculative prefetch of covers beyond what the cover worker does.

## Status (2026-07-11)

Phase 1 landed. Manage→NDS first open went from ~56s to seconds; the 51s was
`scanForwarderCias` walking the romfs file tables of decrypted *full-game*
cias in `sd:/cias` before range-checking the tid (fixed: tid checked right
after the NCCH header, >16MB files skipped, table walk capped).

Phase 2 library part landed (`librefresh.cpp`): SD cache renders instantly,
worker refetches list + missing 3DS tids, heading shows `~ updating...`,
worker does the json save + cover-miss cleanup, take is a vector swap.

### Known issue — residual hitch when "updating..." ends

A brief UI freeze remains right as the refresh completes, even on
`unchanged` runs (worker save skipped, take is trivial). Untested
hypotheses, in likelihood order:

1. **Worker JSON parse on the app core.** The refresh worker runs on core 0
   (`threadCreate(..., core -1)`) at prio+4; nlohmann parse of the full rom
   list is CPU-bound right before completion. In theory main preempts it —
   verify with timestamps. Fix candidate: move the worker to core 1
   (`APT_SetAppCpuTimeLimit` + `threadCreate(..., 1)`), like other homebrew
   does for network workers.
2. **`coverCacheStart` on take** — rebuilds the job list under the lock the
   cover worker holds mid-fetch; if the worker is inside a slow fetch the
   main thread blocks on `LightLock_Lock`.
3. **Menu rebuild on changed lists** (`buildRommMenu` + `init()` for the
   whole list on the UI thread). Only when the list actually changed; also
   resets the selection to the top — preserve selection when addressing.

### Remaining Phase 2 items
- Manage-NDS snapshot json + background rescan (open instantly even on the
  first Manage visit of a session).
- Feed library forwarder markers from the same snapshot so the first library
  open never pays the manage scan.
- Make "Refresh from server" non-blocking (reuse the background job).
