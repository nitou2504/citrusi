# Offline install and fallback-art plan

Status: **design backlog — the remaining offline flow is not fully implemented**.

This document records the current offline behavior, the problems that still
need work, and the acceptance criteria for a reliable offline path. It is
separate from the online art UX because a local install must not depend on an
art server being reachable.

## Scope

This plan covers **Browse SD Card**, local ZIP extraction, local NDS/GBA
installs, and later Manage rebuilds. RomM downloads are intentionally outside
this path: a game that is not already on the SD card cannot be downloaded with
no network.

## Current behavior

### 3DS CIA

- A local `.cia` is installed directly from the SD card.
- Artwork is already inside the CIA.
- No artwork request is needed.
- The offline path is therefore straightforward.

### NDS

- The ROM's embedded DS icon is local and can always be used for the HOME icon.
- The fallback banner can be generated locally from that icon.
- If no custom sound is available, the forwarder template sound is retained.
- The current art chain still contains network requests for YANBF/GameTDB and
  sound through the older `httpc` client. That client has no reliable timeout
  during the art flow, so a disconnected console can stall before reaching the
  local fallback.
- The intended fallback banner is a small raised white 3D card containing the
  original DS icon. It should not require a network request.

### GBA

- The ROM can be injected without artwork.
- The standard GBA ROM header supplies a title/game code, but it does not
  provide a portable standard icon/banner asset.
- Generic title-screen extraction is not a safe default; releases and games
  store graphics differently.
- The current fallback uses the bundled GBA VC glyph/banner and the bundled
  Citrusi startup chime when no custom art is available. A game-specific offline
  art extractor is not planned; any richer fallback still needs a separate
  visual and licensing decision.
- A GBA fallback banner may use a transparent RGBA texture. The HOME icon is
  SMDH/RGB565 and therefore needs a white or opaque background.

## Target offline flow

1. Open **Browse SD Card** without a server connection.
2. Scan local files and ZIP archives without contacting RomM.
3. Before an NDS/GBA install, perform one short key-free connectivity probe.
4. If offline, show one simple warning using the existing Dialog style:

   ```text
   Offline install
   NDS: no custom banner or sound.
   GBA: no custom icon or banner.
   Default art is used.
   Manage: Change art later.
   Reinstall keeps the save.

   [Install anyway] [Cancel]
   ```

   Show only the platform lines that apply. A cached/local asset may still be
   used, but the warning must not make the user navigate through another art
   error dialog.

5. In offline mode, permit cached/local assets but skip uncached network art
   and sound requests.
6. Install from the local ROM using the deterministic fallback.
7. When online later, let Manage retry missing/fallback art without deleting
   the ROM or the installed title.

## Save-data safety requirement

Art changes and reinstalls must use the existing title ID:

- GBA: the allocated GBA VC title ID for the ROM basename.
- NDS: the existing Citrusi forwarder title ID.

The rebuild must use the AM overwrite path. It must never delete the existing
title as an automatic recovery step. If AM rejects the overwrite:

- leave the old title and save untouched;
- report a retry/reboot error;
- do not fall back to `AM_DeleteTitle`, ticket deletion, or filesystem cleanup
  for an installed title.

Hardware testing is required with a save created before an offline install is
rebuilt online.

## Artwork decisions still open

### GBA icon

- **Preferred direction to review:** the existing GBA VC glyph/icon model, with
  no tiny game-name text in the 48×48 HOME icon.
- The icon must remain readable at native HOME size.
- A white opaque background is acceptable and matches the DS/3DS icon style.
- Do not use arbitrary ROM title-screen extraction.

### GBA banner

- Transparent 256×128 RGBA texture is technically supported by the banner
  pipeline.
- The banner should show a recognizable GBA glyph/model and put the game name
  at banner-readable size.
- Avoid a full opaque square or a text-only design.
- The final glyph/model source still needs approval and licensing documentation.

### NDS banner

- Keep the ROM's real DS icon.
- Use the approved layered white 3D-card fallback rather than a flat white
  stamp.
- Preserve alpha behavior and verify the result on HOME hardware.

## Acceptance checklist

- [ ] Browse SD lists and installs a local NDS with Wi-Fi disabled.
- [ ] NDS offline install does not enter an uncapped `httpc` art/sound request.
- [ ] NDS HOME icon remains the ROM's own DS icon.
- [ ] NDS fallback banner is the approved white 3D icon card.
- [ ] GBA offline install completes with the approved fallback icon/banner.
- [ ] GBA banner transparency composites correctly on HOME.
- [ ] GBA and NDS offline warning is one concise Dialog, not multiple prompts.
- [ ] Cached art/assets still work in offline mode.
- [ ] Online Manage rebuild fetches improved art later.
- [ ] Existing GBA/NDS save survives an online rebuild after the offline install.
- [ ] Failure of the overwrite path leaves the existing title and save intact.
- [ ] Local ZIP extraction remains offline and does not delete the source before
      extraction/install succeeds.

## Test fixtures

The host library has representative ZIP fixtures under:

- `/mnt/data2/Emulation/roms/gba/`
- `/mnt/data2/Emulation/roms/nds/`

Useful extracted test ROMs include Pokémon Ruby, Metroid Fusion, Advance Wars,
Mario Kart DS, Phoenix Wright, and Phantom Hourglass. Extract test copies into
`/tmp`; do not modify the master ZIPs.
