#pragma once
#include <string>
#include <functional>
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

// pick + fetch + render an SGDB icon for a game. Preference: .ico assets
// (native 48px frames), then smallest PNG >= 48px. *pickedId = asset id.
std::string artSgdbIcon(SgdbClient& sgdb, RommClient& romm, int gameId, int* pickedId);
// shared preference order (auto pick + picker grid)
void artSortIconAssets(std::vector<SgdbIcon>& list);
bool artAssetIsIco(const SgdbAsset& a);
// same for a specific asset id (reinstall path); "" if the id vanished
std::string artSgdbIconById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId);
// SGDB logo (banner source) by asset id
std::string artSgdbLogoById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId);
// SGDB horizontal grid capsule (banner-shaped art) by asset id
std::string artSgdbGridById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId);
// auto banner tier (NDS): strong SGDB match over the query tiers -> its
// first logo. On success fills entry.sgdbGameId/bannerSource/bannerId.
std::string artSgdbLogoAuto(SgdbClient& sgdb, RommClient& romm,
                            const std::vector<std::string>& queries, ArtEntry& entry);

// SGDB query tiers: RomM metadata title (IGDB match by rom id — survives bad
// file names) first, then the sanitized file name
std::vector<std::string> artQueriesFor(const std::string& fsName, const std::string& title);
// try the queries in order; game id on a strong match, 0 otherwise.
// *usedQuery = the query that hit (or queries[0] as the S3 prefill).
int artSgdbStrongMatch(SgdbClient& sgdb, const std::vector<std::string>& queries,
                       std::string* usedQuery);

// RomM box cover (server path, by rom id — immune to bad file names) ->
// requested pieces. Fills only the requested members of out.
bool artFromRommCover(SgdbClient& sgdb, RommClient& romm, const std::string& coverPath,
                      bool wantIcon, bool wantBanner, ArtPieces& out);

// ---- iiSU community asset server (assets.iisu.community) ------------------
// Open JSON API, no key. iisu_box_art = square launcher icons; logo/hero =
// banner-shaped. Downloads 302 to storage.googleapis.com PNGs (curl follows).
struct IisuAsset {
    int id = 0;
    std::string type;        // "iisu_box_art" | "logo" | "hero"
    int width = 0, height = 0;
    std::string gameName;
    std::string url;         // /api/assets/<id>/download
};
// server-side filtered search (q + platform from the RomM slug)
bool iisuSearch(SgdbClient& net, const std::string& query, const std::string& slug,
                std::vector<IisuAsset>& out);
// fetch + render by asset id (download url is deterministic — reinstall safe)
std::string artIisuIcon48ById(SgdbClient& sgdb, RommClient& romm, int assetId);
std::string artIisuBannerById(SgdbClient& sgdb, RommClient& romm, int assetId);
// auto icon tier: box art whose name norm-matches the query exactly
std::string artIisuIconAuto(SgdbClient& sgdb, RommClient& romm, const std::string& query,
                            const std::string& slug, ArtEntry& entry);
// auto banner tier: clear-logo on an exact name match (last before the cover)
std::string artIisuBannerAuto(SgdbClient& sgdb, RommClient& romm, const std::string& query,
                              const std::string& slug, ArtEntry& entry);

// ---- auto resolution (tier 1) ---------------------------------------------

// GBA auto path: SGDB search (strong match -> icon; title tier then file
// name) + libretro banner. Fills entry (query/gameId/sources/weak) and the
// resolved pieces; missing pieces stay "" and entry.weak reflects them only
// after the caller applies a fallback. entry.valid is NOT set (caller
// persists after install).
// status (optional) gets short progress lines ("Searching SteamGridDB...")
// for the caller to put on screen between the blocking network steps.
void artResolveGba(SgdbClient& sgdb, RommClient& romm, const std::string& fsName,
                   const std::string& title, ArtEntry& entry, ArtPieces& out,
                   std::function<void(const std::string&)> status = nullptr);

// rebuild pieces from a persisted entry (cache-first). false if a piece that
// the entry names can't be produced anymore.
bool artBuildFromEntry(SgdbClient& sgdb, RommClient& romm, const std::string& fsName,
                       const std::string& coverPath, const ArtEntry& entry, ArtPieces& out);
