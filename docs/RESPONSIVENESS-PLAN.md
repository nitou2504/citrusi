# Responsiveness investigation — how other homebrew stays fast, and what citrusi should adopt

Status: investigation only (2026-07-13). No code changes; this is the design
input for a future "instant UI" milestone. Findings verified against the
actual source of each app (file/function references inline).

## Why this doc exists

Observed on hardware (see `sdmc:/3ds/forwarder/log.txt` sessions from 07-12/13):

- Opening a RomM library is slow, and while the "updating" hint shows, the UI
  is frozen — the background refresh saves/cleans on a worker now, but the
  *initial* open still blocks on cache load + title-id resolution + cover
  work, and the main loop stalls whenever any of it touches SD or network.
- Manage scan under cover prefetch: `fwd=51096ms` in one session (vs ~1.4s
  when the prefetch is idle) — pure SD contention between the worker and the
  scan on the main thread.
- Every screen open re-renders full cover art; there is no art-free path.

Comparable homebrew (FBI, 3hs, Universal-Updater, Anemone3DS, GodMode9) all
feel instant on the same hardware, including old3DS. The difference is
architectural, not raw speed.

## What the fast apps actually do

### FBI — the reference architecture (github.com/Steveice10/FBI)

- Main loop is *only* render + input: `while(aptMainLoop() && ui_update());`
  (`source/fbi/main.c:184`), `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` at 60fps.
- At boot: `osSetSpeedupEnable(true)` and **`APT_SetAppCpuTimeLimit(30)`** —
  the latter unlocks core 1 (syscore) for exactly one app thread.
- **All list-populate and install/copy work runs on core 1** at prio
  0x18–0x19 (`threadCreate(..., 0x19, 1, true)`, `task/listtitles.c:344`),
  leaving core 0 fully to the UI. On old3DS this is the only way to get real
  parallelism — the 3DS scheduler is FIFO with *no timeslicing*, so a busy
  same-core worker never yields.
- **Capture/populate pattern**: the titles screen opens instantly with an
  empty list; the worker enumerates `AM_GetTitleList`, reads each SMDH *on
  the worker*, and inserts sorted into the live list — rows appear
  progressively under the cursor, input never blocks (`fbi/titles.c:312`,
  `task/listtitles.c:77`). Refresh = cancel + repopulate, no modal screen.
- Draw is virtualized: the row loop `break`s at the viewport edge
  (`core/ui/list.c:223-246`).
- Cancellation: a sticky kernel event per task, polled between items; APT
  hooks pause workers on sleep/suspend (`core/task/task.c`).
- Texture uploads are pure CPU on 3DS (tiled memcpy + `C3D_TexFlush`), so
  icon textures are built entirely on the worker thread — the render thread
  just draws them once the item exists (`core/screen.c:283`).

### 3hs (hShop client)

- Browse lists are **text-only** — no per-row art at all; metadata + icon for
  the *selected* entry only (`source/next.cc`, `ui/smdhicon.cc`). Instant on
  old3DS.
- Install progress: the UI thread blocks on
  `svcWaitSynchronizationN({worker event, HID events})` — it wakes only for
  progress or input, and B cancels instantly (`source/install.cc:273`). The
  cleanest producer/consumer handshake of the group.

### Universal-Updater

- Store icons come from **pre-built `.t3x` spritesheets** (GPU-native,
  pre-swizzled atlases) downloaded once per store and cached on SD; browsing
  does zero I/O and zero decode (`store/store.cpp:126,212,585`).
- Background download queue on a worker (prio-1, appcore); the user keeps
  browsing while it runs (`utils/queueSystem.cpp`).
- **Double-buffered download→SD pipeline**: curl fills one 384KB aligned
  buffer while a dedicated commit thread fwrites the other, handed off with a
  `LightEvent` pair + `svcFlushProcessDataCache` (`utils/download.cpp:60-160`).
  Network receive and SD write overlap; the UI thread is uninvolved.

### Anemone3DS

- Icon loader thread at **lower** priority than the UI (0x38 vs 0x30, same
  core) — it only runs in frame idle time (`main.c:196`).
- **Sliding-window atlas**: one shared `C3D_Tex` holds 3 pages of 48px icons
  (above/visible/under). On scroll it rotates the ring and reads only the
  newly exposed SMDHs (`loading.c:197-296`). Hundreds of entries, fixed
  memory, no full-list icon load ever.
- Entries without art get a flat placeholder color — the list is complete and
  navigable before any icon I/O happens.

### GodMode9 / ftpd

- Text-first lists with nothing deferred behind I/O; GodMode9 feels instant
  on a 134MHz ARM9 because the only per-entry work is the directory entry
  itself. The datum: **a list should never wait for anything except its own
  enumeration**.
- ftpd's worker recipe: read own priority, spawn at `prio+1` (lower), and for
  network throughput hold `aptSetSleepAllowed(false)` + NDM exclusive state +
  `NDMU_LockState()` (`3ds/platform.cpp:242,764`).

### Pattern table

| Pattern | FBI | 3hs | Universal-Updater | Anemone3DS |
|---|---|---|---|---|
| Worker core | 1 (syscore) | appcore | appcore | appcore |
| Worker prio vs UI 0x30 | 0x18 (other core) | prio-1 | prio-1 | 0x38 (lower) |
| `APT_SetAppCpuTimeLimit` | 30 | – | 30 | 30 |
| List before art | streams in | text-only | atlas preloaded | placeholder colors |
| Icon residency | tex pool (1024) | selected only | whole atlas | 3-page window |
| Cancel | sticky event/item | flag + event | curl callback | flag + mutex |

## Hardware facts that shape the design

- Old3DS: core 0 = app, core 1 = syscore. `threadCreate(..., core 1)` allows
  **one** app thread there, only after `APT_SetAppCpuTimeLimit(30)` (>30%
  gains nothing on o3DS — 3dbrew). Cannot be undone once set.
- Scheduler is SCHED_FIFO, **no timeslicing**: same-priority threads on one
  core never preempt each other. A worker must either live on core 1, or sit
  at *lower* priority on core 0 and block on events (never spin).
- All SD access serializes through the `fs` sysmodule **per session**. A
  worker's 512KB read stalls the main thread's FS call on the shared session.
  Fixes: per-thread `fsUseSession()` with a second `fs:USER` session for the
  I/O worker, plus `FSUSER_SetPriority`/`FSFILE_SetPriority` to deprioritize
  background streams. SD cost is latency-per-open more than bytes — hundreds
  of small opens (SMDH/PNG probing) hurt more than one big read.
- Texture upload = CPU memcpy (+ cache flush); pre-tiled data (SMDH, t3x) is
  a straight copy, PNG needs a per-pixel Morton swizzle — milliseconds-scale
  for a full cover on o3DS, so decode+swizzle must happen on the worker.
- Frame budget: `C3D_FrameBegin(SYNCDRAW)` gives 16.7ms for both screens on a
  268MHz core; `C3D_GetProcessingTime/GetDrawingTime` are free profilers.

## Where citrusi violates these today

1. Library open path (`ensurePlatformLoaded` → `loadLibCache` →
   `resolveTitleIds` → cover start) runs on the main thread; every SD/JSON/
   network step is a UI stall. The "updating" refresh worker exists, but the
   *first paint* still waits for cache load + (3DS) per-rom title-id fetches.
2. Cover cache worker shares the single FS session and the same httpc with
   the main thread; Manage scans went 1.4s → 51s under prefetch.
3. Covers are decoded PNGs cached as PNGs — every cache hit still pays
   decode + swizzle. No pre-tiled on-SD format, no atlas, no window policy;
   textures are per-entry.
4. Dialog-driven flows redraw at full rate instead of blocking on
   worker/HID events (minor, but battery + contention).
5. There is no art-free rendering mode: rows wait on cover textures for
   their final look, and there's no user way to opt out of art entirely.

## Proposed direction (future milestone, in order of payoff)

1. **No-art list view, configurable default** (small, ship first).
   - Settings → "Library view: covers / list". `list` renders text rows +
     platform-colored placeholder chip (Anemone-style) and *never* starts the
     cover worker; `covers` behaves as today but must render rows before art.
   - This is both the instant-mode for old3DS users and the fallback that
     makes every later step safe to A/B on hardware.
2. **Move library open to capture/populate** (FBI pattern).
   - Screen opens on the SD cache immediately (even empty); one job thread
     (core 1 after `APT_SetAppCpuTimeLimit(30)`, prio ~0x19, job deque +
     LightEvent) owns: cache load, listRoms refresh, title-id/file-pick
     resolution, manage scans. UI snapshots the list under a LightLock per
     frame. Kill the modal "Reading title ids..." screen — ids stream in and
     rows upgrade in place.
3. **Second FS session + FS priority for background I/O.**
   - `fsUseSession` on the worker thread; `FSFILE_SetPriority(low)` for cover
     streams. This alone should collapse the 51s manage-scan case.
4. **Cover cache: pre-tiled blobs + windowed hydration.**
   - On first download, resize to draw size, swizzle once, store raw tiled
     RGB565 (+8-byte header) on SD. Cache hit = read + memcpy + TexFlush.
   - Hydrate only visible ± one page (ring of slots, generation counter to
     drop scrolled-away requests); cap resident textures; evict by distance.
5. **Downloads: double-buffer + commit thread, NDM lock** (UU/ftpd pattern)
   for ROM downloads and prefetch; `aptSetSleepAllowed(false)` during ops
   (already partly done for installs).
6. **Event-driven progress dialogs** (3hs): block on
   `svcWaitSynchronizationN` over worker event + HID instead of polling.

Non-goals for that milestone: touch handling, theming, and any server-side
changes — everything above is client architecture only.

## Sources

- FBI — https://github.com/Steveice10/FBI (`source/core/task/task.c`,
  `source/core/ui/list.c`, `source/fbi/task/listtitles.c`,
  `source/fbi/titles.c`, `source/core/screen.c`, `source/fbi/main.c`)
- 3hs — https://github.com/Tescu48/3hs (`source/install.cc`,
  `source/image_ldr.cc`, `include/thread.hh`, `source/next.cc`)
- Universal-Updater — https://github.com/Universal-Team/Universal-Updater
  (`source/utils/download.cpp`, `source/utils/queueSystem.cpp`,
  `source/store/store.cpp`)
- Anemone3DS — https://github.com/astronautlevel2/Anemone3DS
  (`source/loading.c`, `include/loading.h`, `source/main.c`)
- GodMode9 — https://github.com/d0k3/GodMode9; ftpd —
  https://github.com/mtheall/ftpd (`source/3ds/platform.cpp`)
- libctru `thread.h` / `services/fs.h`; devkitPro 3ds-examples
  `threads/thread-basic`
- 3dbrew: APT:SetApplicationCpuTimeLimit, Multi-threading
- citro3d `renderqueue.h`, `texture.h`
