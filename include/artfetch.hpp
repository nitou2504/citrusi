#pragma once
#include <string>
#include "romm.hpp"
#include "sgdb.hpp"
#include "artstore.hpp"
#include "artquery.hpp"

// Art fetching + rendering for the GBA inject / NDS forwarder art layer
// (docs/ART-UX-SPEC.md). Produces the two baked pieces:
//   icon48:    48*48*2 linear RGB565 for the SMDH (flattened on white)
//   bannerTex: 0x10000-byte 256x128 RGBA4444 tiled banner texture
struct ArtPieces {
    std::string icon48;
    std::string bannerTex;
};

// ---- low-level render helpers -------------------------------------------

// image bytes -> 48x48 linear RGB565: contain-fit centered, box-filter
// downscale / nearest upscale (pixel art), alpha flattened onto white
std::string artIcon48FromImage(const std::string& bytes);
// image bytes -> 256x128 RGBA4444 tiled: contain-fit centered on transparent
// (same look as the GameTDB NDS banners)
std::string artBannerFromImage(const std::string& bytes);

// ---- cached transport ----------------------------------------------------

// fetch a url through the right client (https -> SGDB curl, http/server path
// -> RomM httpc) with the on-disk raw cache. cacheKey names the cache slot.
bool artGetUrl(SgdbClient& sgdb, RommClient& romm, const std::string& url,
               const std::string& cacheKey, std::string& bytes);

// ---- source fetchers -----------------------------------------------------

// libretro Named_Logos banner (GBA only): exact No-Intro stem, one
// tag-stripped retry on 404. Returns tex; *usedName = the name that hit.
std::string artLibretroBanner(SgdbClient& sgdb, RommClient& romm,
                              const std::string& fsName, std::string* usedName);

// pick + fetch + render an SGDB icon for a game (smallest asset >= 48px).
// Returns icon48; *pickedId = asset id.
std::string artSgdbIcon(SgdbClient& sgdb, RommClient& romm, int gameId, int* pickedId);
// same for a specific asset id (reinstall path); "" if the id vanished
std::string artSgdbIconById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId);
// SGDB logo (banner source) by asset id
std::string artSgdbLogoById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId);

// RomM box cover (server path, by rom id — immune to bad file names) ->
// requested pieces. Fills only the requested members of out.
bool artFromRommCover(SgdbClient& sgdb, RommClient& romm, const std::string& coverPath,
                      bool wantIcon, bool wantBanner, ArtPieces& out);

// ---- auto resolution (tier 1) ---------------------------------------------

// GBA auto path: SGDB search (strong match -> icon) + libretro banner.
// Fills entry (query/gameId/sources/weak) and the resolved pieces; missing
// pieces stay "" and entry.weak reflects them only after the caller applies
// a fallback. entry.valid is NOT set (caller persists after install).
void artResolveGba(SgdbClient& sgdb, RommClient& romm, const std::string& fsName,
                   ArtEntry& entry, ArtPieces& out);

// rebuild pieces from a persisted entry (cache-first). false if a piece that
// the entry names can't be produced anymore.
bool artBuildFromEntry(SgdbClient& sgdb, RommClient& romm, const std::string& fsName,
                       const std::string& coverPath, const ArtEntry& entry, ArtPieces& out);
