# Art & install UX — full spec (GBA + NDS)

Status: **implemented (2026-07-11) — pending hardware test.** All flows below
are built on `feat/gba` (`artquery` / `artfetch` / `artstore` / `artpicker` +
menu wiring; see the [GBA-PLAN.md](GBA-PLAN.md) progress log for the module
map). Implementation deviations: art.json keys by extension-less stem, the
Manage marker renders as `[!]` (font safety), the NDS notify runs at build
time (after download — the GameTDB chain needs the ROM on SD), and GameTDB
isn't repeated as a picker candidate (the RomM cover shows the same box).

## 1. Principles

- **Install is one tap.** The default path is identical to today's app:
  confirm → progress → "Installed!". No new screens for ~90% of games.
- **Art never blocks an install** and an install never ends artless — worst
  case is the RomM box cover, letterboxed (never cropped).
- **The user is told when art is missing** (bad file name), in one dialog,
  at install time — before the long download, when correcting the name is
  cheapest.
- **Taste fixes happen later, in Manage**, exactly where the user is when
  they think "that icon is wrong" — not predicted up front (iiSU/Cocoon
  lesson: good defaults beat mandatory choice).

## 2. Art pieces and their sources

| piece | GBA | NDS |
|---|---|---|
| HOME icon (48×48 SMDH) | **SGDB icons** → RomM cover tile → plain tile | **ROM's own DS icon** (always exists); SGDB icons as optional override |
| banner (256×128) | **libretro Named_Logos** → tag-stripped retry → SGDB logos → RomM cover centered → tile | existing chain: SD assets → GameTDB → **SGDB logos (new)** → RomM cover centered → DS-icon stamp |
| banner sound | bundled silent WAV | existing YANBF jingle path |

Notes:
- libretro Named_Logos is **empty for NDS** (verified: even Mario Kart DS
  404s) — never queried for NDS.
- RomM provides **covers only** (600×900 portrait, by rom id — immune to bad
  file names; library has ~full coverage). RomM has no icons or banners.
- Transports: RomM + libretro + GameTDB over existing `httpc` (plain HTTP);
  SGDB over the new curl+mbedtls client (TLS 1.2, see GBA-PLAN).

## 3. Name handling

### 3.1 SGDB search query (sanitizer)
From the fs_name stem, ES-DE's verified algorithm plus two extras:
1. strip extension(s)
2. iteratively erase `(...)` and `[...]` blocks (innermost first, loop until
   stable) — ES-DE `StringUtil::removeParenthesis`
3. `_` → space
4. flip the No-Intro article in the first ` - ` segment:
   `"Legend of Zelda, The - The Minish Cap"` → `"The Legend of Zelda - The
   Minish Cap"` (also `", A"`, `", An"`)
5. collapse whitespace, trim spaces and stray `-`

### 3.2 libretro banner URL
The **exact No-Intro stem, parens included** — never sanitized. RetroArch
filename rule applied (`&*/:`<>?\|` → `_`), URL-encoded. On 404, retry once
with `(Translated)` and `[...]` tags stripped (verified fix for
"Mother 3 (Japan) (Translated)" and "F-Zero - Climax [T-En …]").

### 3.3 Match confidence
`norm()` both the query and each autocomplete result: lowercase, fold
accents/macrons (ō→o, é→e), map `&`↔`and`, drop trailing platform tokens
("GBA"), strip non-alphanumerics. Then:
- some result `norm == query norm` → **strong** → auto-pick that game
- top result prefix/substring either direction → **medium**
- otherwise, or zero results → **weak / none**

Strong is silent. Medium/weak/none with the notify setting on → screen S2.
Medium/weak with notify off → silent RomM-cover fallback + ⚠ flag.

Measured on the real library: 14/19 GBA strong as-is, 16–17/19 after the
norm() folds; NDS test set 4/5 strong (miss = Spanish-renamed file).

## 4. Screens

IDs used by the flows in §5. Bottom screen, existing Dialog/Menu widgets
unless said otherwise. Existing screens listed for completeness.

### S1 · Install confirm (existing, one new button)
Trigger: A on a library game.
```
┌──────────────────────────────┐
│ Download + install?          │   NDS: "Download + install forwarder?"
│ Advance Wars (USA) (Rev 1)   │   already-on-SD variant keeps its
│ 6.4 MB                       │   Install / Redownload / Cancel shape
│ [Yes] [+ art…] [No]          │
└──────────────────────────────┘
```
- **Yes** → auto art resolution → F1/F2.
- **+ art…** (tier 2) → title swkbd → picker S4 (icon page, then banner
  page) → then download/install. For users who already know they care.
- **No** → back.

### S2 · Art-missing notify
Trigger: after Yes, auto resolution finished, and ≥1 piece is weak/none
(and notify setting is on). Never shown when everything matched strongly;
never shown for the NDS icon (can't fail).
```
┌──────────────────────────────┐
│ Art not found:               │
│  icon: no match for          │   ← only the lines that failed;
│   "Pokemon - Edicion Esme…"  │     single-line version when one piece
│  banner: not found           │     failed (common NDS case: banner only)
│ [Search]   [Use RomM cover]  │
└──────────────────────────────┘
```
- **Search** → S3.
- **Use RomM cover** → missing pieces built from the cover (icon: letterboxed
  tile; banner: height-fit centered), ⚠ flag persisted → continue to download.
- If the game has no RomM cover either (rare): button reads **[Use plain
  tile]** and the tile/stamp fallback is used.

### S3 · Refine search (swkbd)
Trigger: [Search] on S2, or X inside S4.
Software keyboard, hint "Game name", prefilled with the sanitized query.
One corrected name re-queries **both** SGDB and libretro. Confirm → S4 with
fresh results; cancel → back to where it was opened from.

### S4 · Picker
Trigger: S2→Search path, S1 [+ art…], or M2 [Change art].
Two pages, Y toggles (page skipped if that piece isn't being chosen):
```
┌──────────────────────────────┐   ┌──────────────────────────────┐
│ ICON        ◄Y► 18 found  p1/2│   │ BANNER      ◄Y►  4 found     │
│  ▣ ▣ ▣ ▣ ▣                   │   │  ▭▭▭▭▭   ▭▭▭▭▭               │
│  ▣ ▣ ▣ ▣ ▣    (cursor on one │   │  ▭▭▭▭▭   ▭▭▭▭▭               │
│  ▣ ▣ ▣ ▣ ▣     shows WxH)    │   │  (128×64 thumbs, 2×2)        │
│ A use · X search · B skip    │   │ A use · X search · B skip    │
└──────────────────────────────┘   └──────────────────────────────┘
```
- Icon grid 5×3 of 48×48 cells; L/R pages. First cells are the non-SGDB
  candidates so they're always reachable: current art (in Change-art mode),
  DS icon (NDS), RomM cover tile. Then SGDB results.
- Banner page candidates: libretro logo (GBA) / GameTDB (NDS), SGDB logos,
  RomM cover, tile/stamp.
- Thumbs download lazily (covercache async pattern), placeholder ▒ while
  loading; footer shows count; cursor cell shows source + native dims.
- States: **loading** (placeholders), **empty** ("No results — X to search"),
  **offline** ("SteamGridDB unreachable — [Retry] [Use RomM cover]").
- A = use focused candidate for that page → advance (other page or done).
  B = keep current/fallback for that page and advance.

### S5 · Progress dialogs (existing, unchanged)
"Downloading… (B = cancel)" → optional "Extracting…" (zip) → "Installing…".
GBA adds the CIA build step under the same Installing dialog.

### S6 · Installed! (existing, unchanged)

### M1 · Manage list (existing, one addition)
⚠ prefix on entries whose persisted art is a fallback:
```
│  Advance Wars              │
│  ⚠ Pokemon Esmeralda       │
│  Zelda - Minish Cap        │
```

### M2 · Manage game actions (existing dialog, one new button)
```
│ Pokemon Esmeralda          │
│ fwd: installed  ⚠ art      │
│ [Change art] [Delete] [Back]│    (existing buttons preserved)
```
**Change art** → S4 pre-loaded from `art.json` (cached SGDB id → instant
grid; bad-name games open empty → X to search). After picking: rebuild —
NDS forwarder rebuilds in seconds; GBA re-bakes the CIA (needs the ROM on
SD — if it was deleted, prompt "ROM not on SD — [Re-download] [Cancel]").
Same TID → HOME position and save data untouched.

### SET · Settings (one new row)
`Art: notify when missing (default) / silent fallback`
Plus the SGDB key status line: "SteamGridDB key: found / missing
(sd:/3ds/romm3ds/sgdb.env)".

## 5. Flows

### F1 · Happy path (~90%)
S1[Yes] → auto resolve (strong icon + banner hit) → S5 → S6. Zero new UI.

### F2 · Bad name at install (GBA: icon+banner both miss)
S1[Yes] → S2 → [Search] → S3 ("Pokemon Emerald") → S4 icon (18 results,
pick) → S4 banner (auto-skipped if the corrected name hit libretro;
otherwise pick/B) → S5 → S6. Or S2 → [Use RomM cover] → S5 → S6 with ⚠.

### F3 · NDS banner-only miss
S1[Yes] → S2 (single banner line) → same as F2, icon page never shown.

### F4 · Taste fix later (Peggle)
M1 → M2 [Change art] → S4 (icon page: DS icon + SGDB's 2 Peggle icons;
Y → banner page: GameTDB current, SGDB logo, RomM cover) → pick → rebuild.

### F5 · Offline / SGDB down / key missing
- SGDB unreachable at install: treated as miss → S2, whose [Search] would
  fail again → S4 shows the offline state; [Use RomM cover] always works
  (RomM is LAN).
- `sgdb.env` absent: SGDB source disabled; first GBA install shows a one-time
  hint dialog ("No SteamGridDB key — icons will use RomM covers. See
  sgdb.env"). No repeat nagging; status visible in Settings.
- No network at all: install already fails at download; art never the blocker.

### F6 · Reinstall / redownload
`art.json` hit → art reused silently, no notify, no queries (S2 never shown
for a game with persisted art). Redownload path identical.

### F7 · Batch installs
Notifies queue per game but strong matches sail through — with good names a
10-game batch shows zero art prompts.

## 6. Persistence & caching

`sdmc:/3ds/forwarder/art.json` (next to the TID files):
```json
{
  "Advance Wars (USA) (Rev 1).zip": {
    "query": "Advance Wars",
    "sgdbGameId": 34640,
    "icon":   { "source": "sgdb", "id": 27345 },
    "banner": { "source": "libretro", "name": "Advance Wars (USA) (Rev 1)" },
    "weak": false
  },
  "Pokemon - Edicion Esmeralda.zip": {
    "icon":   { "source": "romm-cover" },
    "banner": { "source": "romm-cover" },
    "weak": true
  }
}
```
- `weak: true` drives the ⚠ marker; cleared when the user picks real art.
- Image cache: `sdmc:/3ds/romm3ds/cache/art/` — `sgdb-<game>-<id>.png`,
  `libretro-<sha1 of name>.png`, `search-<norm query>.json`. Serves the
  picker instantly on revisit and spares SGDB's rate limit.

## 7. Implementation map

New: `artquery.cpp` (sanitize/norm/confidence), `sgdb.cpp` (curl client +
key from `sgdb.env`), `artpicker.cpp` (S4), `artstore.cpp` (art.json +
cache). Touch points: S1/S2 wiring in `menu.cpp` RommInstall
(`menu.cpp:1189`), M1/M2 in ManageRom + `manage.cpp`, SET in settings menu,
`fetchBoxart()` gains the SGDB-logos + RomM-cover tiers, `CtrBuilder::buildSmdh`
gains a PNG-icon input path (48/24 downscale via `renderRegion`, nearest for
upscales — never bilinear on pixel art). Makefile: portlibs + curl/mbedtls
link flags (see GBA-PLAN).

## 8. Verified test data (2026-07-11, real library + real SGDB key)

- GBA libretro logos: 11/11 exact No-Intro names → 200; tag-strip retry
  recovers 2 more; remaining misses are renamed files.
- GBA SGDB: 14/19 strong (16–17 after norm folds); icons per matched game
  1–19; the only true misses are a fan compilation (Mother 1+2) and
  Spanish-renamed files — both exactly the S2/S3 case.
- NDS SGDB: Peggle Dual Shot strong with 2 icons; Ghost Trick 11; Zelda
  Phantom Hourglass 15; Hotel Dusk 1. Spanish Layton → S3 case.
- NDS libretro Named_Logos: empty (excluded as a source).
