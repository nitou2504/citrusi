# NDS forwarder detection — notes + pending investigation

## Fact (user's setup)

- The user's **NDS forwarders were created manually** via the PC/YANBF pipeline
  **before** this app existed.
- NDS games are launched from a **HOME-menu forwarder** (YANBF type) — not from
  TWiLight's file browser. So per-game HOME forwarders **do** exist on the console.

## How the app detects forwarders

`manage.cpp scanManagedRoms(romDir)` matches each SD `.nds` against three
AM-verified sources:

| Type | Title id | How matched |
|---|---|---|
| **romm3ds CTR** | `0004000000xxxx00` | `getRommCtrForwarders` — `sd:/3ds/forwarder/ctr/<tid>.txt` bookkeeping (only exists for forwarders **this app** built), verified against AM |
| **TWL** | `0004800400000000 \| rev(gamecode)` | `computeForwarderTID(nds)` vs `getInstalledTwlTitles` (NAND cat `0x8004/0x8005`) |
| **YANBF** | `000400000FF4xxxx`–`0FF7xxxx` | `getYanbfForwarders` — mounts each title's romfs, reads `path.txt`, keys on the filename |

The library "installed" marker matches a RomM game's **normalized name** (drop
extension / `(region)` tags / punctuation, ASCII-alnum lowercase) against the set
of SD roms that have any forwarder.

## Observed (2026-07-11 device log)

- The app **correctly detected 4 romm3ds forwarders** it built itself
  (HeartGold, SoulSilver, Advance Wars, Super Princess Peach).
- Installed NAND TWL titles were **launchers, not per-game**:
  `…46574452` = `FWDR` (YANBF bootstrap), `…54574C44` = `TWLD` (TWiLight), etc.
- None of the per-rom **computed TWL tids matched** an installed title.
- ⇒ The user's **manually-made YANBF forwarders were not detected** by
  `getYanbfForwarders` in that run.

## Pending investigation

**Can the app detect the user's pre-existing YANBF forwarders?**

Concrete test case: **find the Tetris DS forwarder**.

To check:
1. Which AM title-id **range** the user's YANBF forwarders actually occupy
   (is it outside `000400000FF40000..0FF7FFFF`?).
2. What each forwarder's romfs **`path.txt`** contains (does the stored name match
   the SD `.nds` name / normalize equal?).
3. Whether `getYanbfForwarders`'s range and/or name-match needs widening.

If they're genuinely undetectable from the app, document **why** (e.g., a
forwarder scheme the app can't enumerate or map back to a rom).

Status: **not started** — documented per request; do the investigation later.
