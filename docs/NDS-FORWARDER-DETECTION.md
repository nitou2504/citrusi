# NDS forwarder detection — resolved

## Fact (user's setup)

- The user's **NDS forwarders were created manually** via the PC/YANBF pipeline
  **before** this app existed.
- NDS games are launched from a **HOME-menu forwarder** (YANBF type) — not from
  TWiLight's file browser. The generator's `.cia` output still lives on SD under
  `sd:/cias/nds-fwd/`.

## How the app detects forwarders

`manage.cpp scanManagedRoms(romDir)` matches each SD `.nds` against three
AM-verified sources:

| Type | Title id | How matched |
|---|---|---|
| **romm3ds CTR** | `0004000000xxxx00` | `getRommCtrForwarders` — `sd:/3ds/forwarder/ctr/<tid>.txt` bookkeeping (only exists for forwarders **this app** built), verified against AM |
| **TWL** | `0004800400000000 \| rev(gamecode)` | `computeForwarderTID(nds)` vs `getInstalledTwlTitles` (NAND cat `0x8004/0x8005`) |
| **YANBF** | `000400000FF4xxxx`–`0FF7xxxx` | `getYanbfForwarders` — romfs mount when possible, else **plaintext `.cia` parsing** (see below) |

The library "installed" marker matches a RomM game's **normalized name** against
the set of SD roms that have any forwarder.

## Investigation result (2026-07-11)

**Symptom:** the user's 23 installed YANBF forwarders were all in the expected
TID range but `getYanbfForwarders` found 0.

**Root cause:** `romfsMountFromTitle(tid, MEDIATYPE_SD, ...)` fails with
`0xD9004676` — FS module, "command not allowed" (Citra's
`ERROR_COMMAND_NOT_ALLOWED`: level Permanent, summary WrongArgument, desc 630).
FS **refuses to open another title's RomFS from userland**, and it is NOT an
exheader/ACL issue:

- Fails identically from the 3DSX (HBL) and from the installed CIA, whose RSF
  has `CategorySystemApplication` + `CategoryFileSystemTool` + full FS access.
- The **ExeFS icon** of the same titles IS readable
  (`FSUSER_OpenFileDirectly`, filePath `{0,0,2,'icon',0}` → rc 0). Only RomFS
  is blocked. This means an **SMDH-name fallback is viable** if ever needed
  (YANBF sets the SMDH title from the rom name).

**Fix (implemented):** `scanForwarderCias` — parse the generator's leftover
plaintext `.cia` files on SD (`sd:/cias`, `sd:/cia`, recursive, depth ≤ 2):

1. CIA header → 0x40-aligned cert/ticket/tmd offsets → NCCH.
2. NCCH: require `NoCrypto` flag; read programID (+0x118) and romfs offset
   (+0x1B0, media units).
3. RomFS IVFC → level3 at +0x1000 → file-metadata walk → `path.txt` content
   → rom filename (basename, lowercased).
4. TID must be in the YANBF range and **installed per AM** → detected
   forwarder. Verified on device: `yanbf=23`, all matched (incl. Tetris DS,
   tid `000400000FF41A00`).

## Orphan forwarder cias

A parsed forwarder `.cia` whose TID is **not installed** is tracked as an
*orphan* (`getOrphanForwarderCias`, `ManagedRom::orphanCia`). Real case:
*Animal Crossing – Wild World* — cia present (`0FF40100`), title never
installed, so "not detected" was correct.

Manage UI:
- chip shows `no forwarder detected` / `fwd cia on SD`;
- bottom text explains rom-on-SD state;
- if an orphan cia exists, the A-press dialog offers **Install cia**
  (via `installCiaFromFile`) alongside Build FWD / Delete ROM.

## Notes

- YANBF romfs `path.txt` content looks like
  `sd:/roms/nds/Tetris DS (Europe) (En,Fr,De,Es,It).nds`.
- The romfs-mount path is kept in `getYanbfForwarders` in case some setup
  allows it; the cia scan fills whatever the mount could not resolve.
- YANBF detection results are session-cached (`invalidateYanbfCache()` after
  installs/deletes).
