# 3DS install detection — how it works and its limits

## Mechanism

A library rom shows `*` (installed) when `installed3dsHasTitle(rom.titleId)`:

- `rom.titleId` = title id parsed from the **server cia's** header
  (`resolveTitleIds` / the background refresh worker, cached in
  `lib_3ds.json`).
- Installed set = AM title list (SD+NAND), refreshed per menu build.
- Installs done **through the app** are also recorded in
  `installed3ds.json` (rommId → tid) at install time.

## Known limitation: region variants (2026-07-11)

A game installed from a **different-region copy** than the server's cia has a
different title id and is not marked installed. Real case on the user's
console:

| Game | Installed (AM) | Server cia | Match |
|---|---|---|---|
| Rhythm Megamix | `18A400` (USA "Rhythm Heaven") | `18A500` (EUR "Rhythm Paradise") | ✗ |
| Picross 3D: Round 2 | `187D00` | `187E00` | ✗ |
| Pokemon Picross | `17C100` | `17C100` | ✓ |

Region variants have distinct unique ids (not a flag bit), so there is no
safe tid-level fuzzy match.

## Future improvement options

- **SMDH name match fallback**: another title's ExeFS `icon` (SMDH) IS
  readable from userland (`FSUSER_OpenFileDirectly`, filePath
  `{0,0,2,'icon',0}` — verified rc 0, see NDS-FORWARDER-DETECTION.md).
  Could read installed titles' SMDH short titles once, cache them, and
  fuzzy-match against library names when the tid misses.
- Match on normalized fsName vs installed title name (same normalization the
  NDS marker uses).
- Both would mark the *game* installed even when the exact region differs —
  arguably what the user expects.
