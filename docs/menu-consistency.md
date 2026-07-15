# Menu consistency model — the 3 roots

Three roots do the **same operations on the same games** from different sources:

- **RomM Library** (`MENU_ROMM`) — download from server, then install. `case RommInstall`, `case BatchRommInstall`.
- **Browse SD Card** (`MENU_LOCAL`) — local files. `case LocalInstall`, `case LocalInstallSelected`.
- **Manage Installed** (`MENU_MANAGE`) — what's installed. `case ManageRom`, `case ManageZip`, `case BatchManage`.

**Rule: the option set is keyed on `(platform, state)` only.** All three roots must show the same labels/order for the same state. Root differences are only: description wording (download vs SD vs in-place), an extra `Delete file` row (Manage/on-SD), and whether `Reinstall` exists (Manage-3DS-installed has no source → no Reinstall).

Language: **install · art · filter · manage · "the game"**. No forwarder/inject/bake/batch/title-database jargon in labels.

## Platform capabilities
| | 3DS (.cia) | NDS (forwarder) | GBA (inject) |
|---|---|---|---|
| Art picker | ❌ (own icon) | ✅ icon/banner | ✅ icon/banner |
| Filter | ❌ | ❌ | ✅ |

## Canonical single-item menus

**Not installed**
```
3DS : [ Install ]
NDS : [ Install ] [ Install + choose art ]
GBA : [ Install ] [ Install + choose art ] [ Install + filter ] [ Install + art + filter ]
```
On-SD / Manage append `[ Delete the file ]` last. 3DS-on-SD also gets `[ Download again ]` (RomM).

**Installed** — order: **Uninstall first, Reinstall last; art/filter behind a submenu.**
```
3DS : [ Uninstall (+ update/DLC rows) ] [ Reinstall ]      (Manage: no Reinstall)
NDS : [ Uninstall ] [ Change art ] [ Reinstall ]
GBA : [ Uninstall ] [ Art & filter > ] [ Reinstall ]
        Art & filter > : [ Change art ] [ Filter ] [ Art + filter ]
```
GBA installed is built by the shared `gbaInstalledMenu()` (menu.cpp) — all three roots call it.
3DS uninstall rows come from the shared `addUninstall3dsOpts()` / `execUninstall3ds()`.

## Canonical batch menus (selection has installed items)

Subtitle everywhere: `"M selected - K installed"`. Count every row.
```
[ Install new (N) ]              (if any not-installed)
[ Install + reinstall all (M) ]  (or "Reinstall (K)" when N==0)
[ Uninstall installed (K) ]      ← present in RomM, Browse AND Manage
```
All-new selection: `[ Install (M) ]` (+GBA art/filter sub-choice), Manage adds `[ Delete files (M) ]`.

## Dialogs
Every confirm / OK / error renders with the **actionMenu visual language** (`Dialog::draw` in dialog.cpp mirrors `actionMenu` in menu.cpp): full-screen, centered title + subtitle, full-width accent option rows at x+12 (label x+22), bottom hint. One surface.

## Anti-drift
When changing an installed/batch menu, change it for the state in ALL THREE roots (or better, in the shared helper). Grep the call sites: `gbaInstalledMenu`, `addUninstall3dsOpts`, the batch scope blocks in `BatchRommInstall` / `LocalInstallSelected` / `BatchManage`.
