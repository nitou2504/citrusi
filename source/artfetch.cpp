#include <3ds.h>
#include <cstring>
#include <algorithm>
#include "artfetch.hpp"
#include "teximg.hpp"
#include "helpers.hpp"
#include "logger.hpp"

// stb_image implementation lives in boxart.cpp
#define STBI_NO_STDIO
#include "stb_image.h"

static Logger aflog("artfetch");

#define BNR_W 256
#define BNR_H 128
#define ICON_DIM 48

#define LIBRETRO_GBA_LOGOS \
    "http://thumbnails.libretro.com/Nintendo%20-%20Game%20Boy%20Advance/Named_Logos/"

// ---- render helpers -------------------------------------------------------

// bannertool texture layout: 8x8 morton-tiled RGBA4444
static std::string tile4444(const u8* canvas /*BNR_W*BNR_H*4 linear RGBA*/) {
    std::string out(BNR_W * BNR_H * 2, '\0');
    u16* dst = (u16*)&out[0];
    for (u32 y = 0; y < BNR_H; y++) {
        for (u32 x = 0; x < BNR_W; x++) {
            u32 index = (((y >> 3) * (BNR_W >> 3) + (x >> 3)) << 6) +
                        ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                         ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
            const u8* p = &canvas[(y*BNR_W + x)*4];
            dst[index] = (u16)(((p[0] & ~0xF) << 8) | ((p[1] & ~0xF) << 4) |
                               (p[2] & ~0xF) | (p[3] >> 4));
        }
    }
    return out;
}

std::string artBannerFromImage(const std::string& bytes) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba = decodeImageRGBA(bytes, BNR_W, BNR_H, &w, &h);
    if (rgba.empty()) return "";
    static u8 canvas[BNR_W * BNR_H * 4];
    memset(canvas, 0, sizeof(canvas));
    int ox = (BNR_W - w) / 2, oy = (BNR_H - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(&canvas[((oy + y) * BNR_W + ox) * 4], &rgba[y * w * 4], w * 4);
    return tile4444(canvas);
}

std::string artIcon48FromImage(const std::string& bytes) {
    int sw = 0, sh = 0, comp = 0;
    u8* probe = stbi_load_from_memory((const u8*)bytes.data(), bytes.size(), &sw, &sh, &comp, 4);
    if (!probe) return "";

    // white 48x48 canvas, contain-fit centered
    static u8 canvas[ICON_DIM * ICON_DIM * 4];
    for (int i = 0; i < ICON_DIM * ICON_DIM; i++) {
        canvas[i*4] = canvas[i*4+1] = canvas[i*4+2] = 255;
        canvas[i*4+3] = 255;
    }
    if (sw >= ICON_DIM && sh >= ICON_DIM) {
        stbi_image_free(probe);
        // box-filtered downscale via the shared decoder
        int w = 0, h = 0;
        std::vector<unsigned char> rgba = decodeImageRGBA(bytes, ICON_DIM, ICON_DIM, &w, &h);
        if (rgba.empty()) return "";
        int ox = (ICON_DIM - w) / 2, oy = (ICON_DIM - h) / 2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const u8* s = &rgba[(y * w + x) * 4];
                u8* d = &canvas[((oy + y) * ICON_DIM + ox + x) * 4];
                u32 a = s[3];
                d[0] = (u8)((s[0]*a + 255*(255-a)) / 255);
                d[1] = (u8)((s[1]*a + 255*(255-a)) / 255);
                d[2] = (u8)((s[2]*a + 255*(255-a)) / 255);
            }
    } else {
        // small source: nearest upscale, never bilinear on pixel art
        float scale = std::min((float)ICON_DIM / sw, (float)ICON_DIM / sh);
        int dw = (int)(sw * scale), dh = (int)(sh * scale);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int ox = (ICON_DIM - dw) / 2, oy = (ICON_DIM - dh) / 2;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                const u8* s = &probe[((y * sh / dh) * sw + (x * sw / dw)) * 4];
                u8* d = &canvas[((oy + y) * ICON_DIM + ox + x) * 4];
                u32 a = s[3];
                d[0] = (u8)((s[0]*a + 255*(255-a)) / 255);
                d[1] = (u8)((s[1]*a + 255*(255-a)) / 255);
                d[2] = (u8)((s[2]*a + 255*(255-a)) / 255);
            }
        stbi_image_free(probe);
    }
    // linear RGB565
    std::string out(ICON_DIM * ICON_DIM * 2, '\0');
    u16* px = (u16*)&out[0];
    for (int i = 0; i < ICON_DIM * ICON_DIM; i++) {
        const u8* p = &canvas[i*4];
        px[i] = (u16)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
    }
    return out;
}

// ---- cached transport ------------------------------------------------------

bool artGetUrl(SgdbClient& sgdb, RommClient& romm, const std::string& url,
               const std::string& cacheKey, std::string& bytes) {
    bytes = artCacheRead(cacheKey);
    if (!bytes.empty()) return true;
    bool ok;
    if (url.rfind("https://", 0) == 0)
        ok = sgdb.fetchImage(url, bytes);      // curl+mbedtls (TLS 1.2)
    else
        ok = romm.fetchUrl(url, bytes);        // httpc: plain http / server path
    if (!ok || bytes.empty()) return false;
    artCacheWrite(cacheKey, bytes);
    return true;
}

// ---- source fetchers --------------------------------------------------------

std::string artLibretroBanner(SgdbClient& sgdb, RommClient& romm,
                              const std::string& fsName, std::string* usedName) {
    for (const std::string& name : libretroNameVariants(fsName)) {
        std::string bytes;
        if (!artGetUrl(sgdb, romm, LIBRETRO_GBA_LOGOS + urlEncodePath(name) + ".png",
                       "libretro-gba-" + name, bytes))
            continue;
        std::string tex = artBannerFromImage(bytes);
        if (!tex.empty()) {
            aflog.info("libretro logo hit: " + name);
            if (usedName) *usedName = name;
            return tex;
        }
    }
    return "";
}

// smallest asset whose smaller side still covers 48px; else the largest one
static int pickIconAsset(const std::vector<SgdbIcon>& list) {
    int best = -1, bestBig = -1;
    long bestArea = 0, bestBigArea = 0;
    for (size_t i = 0; i < list.size(); i++) {
        long area = (long)list[i].width * list[i].height;
        if (area <= 0) continue;
        if (std::min(list[i].width, list[i].height) >= ICON_DIM) {
            if (best < 0 || area < bestArea) { best = (int)i; bestArea = area; }
        } else {
            if (bestBig < 0 || area > bestBigArea) { bestBig = (int)i; bestBigArea = area; }
        }
    }
    if (best >= 0) return best;
    if (bestBig >= 0) return bestBig;
    return list.empty() ? -1 : 0;   // no dims reported: take the first
}

std::string artSgdbIcon(SgdbClient& sgdb, RommClient& romm, int gameId, int* pickedId) {
    std::vector<SgdbIcon> list;
    if (!sgdb.icons(gameId, list) || list.empty()) return "";
    int pick = pickIconAsset(list);
    if (pick < 0) return "";
    aflog.info("icons: " + std::to_string(list.size()) + " assets, picked id=" +
               std::to_string(list[pick].id) + " " + std::to_string(list[pick].width) +
               "x" + std::to_string(list[pick].height));
    std::string bytes;
    if (!artGetUrl(sgdb, romm, list[pick].url,
                   "sgdb-icon-" + std::to_string(list[pick].id), bytes)) return "";
    std::string icon = artIcon48FromImage(bytes);
    if (!icon.empty() && pickedId) *pickedId = list[pick].id;
    return icon;
}

std::string artSgdbIconById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId) {
    std::string bytes = artCacheRead("sgdb-icon-" + std::to_string(assetId));
    if (bytes.empty()) {
        std::vector<SgdbIcon> list;
        if (!sgdb.icons(gameId, list)) return "";
        for (auto& a : list)
            if (a.id == assetId) {
                artGetUrl(sgdb, romm, a.url, "sgdb-icon-" + std::to_string(assetId), bytes);
                break;
            }
        if (bytes.empty()) return "";
    }
    return artIcon48FromImage(bytes);
}

std::string artSgdbLogoById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId) {
    std::string bytes = artCacheRead("sgdb-logo-" + std::to_string(assetId));
    if (bytes.empty()) {
        std::vector<SgdbAsset> list;
        if (!sgdb.logos(gameId, list)) return "";
        for (auto& a : list)
            if (a.id == assetId) {
                artGetUrl(sgdb, romm, a.url, "sgdb-logo-" + std::to_string(assetId), bytes);
                break;
            }
        if (bytes.empty()) return "";
    }
    return artBannerFromImage(bytes);
}

std::string artSgdbLogoAuto(SgdbClient& sgdb, RommClient& romm,
                            const std::vector<std::string>& queries, ArtEntry& entry) {
    int gameId = artSgdbStrongMatch(sgdb, queries, &entry.query);
    if (!gameId) return "";
    std::vector<SgdbAsset> logos;
    if (!sgdb.logos(gameId, logos) || logos.empty()) return "";
    std::string bytes;
    if (!artGetUrl(sgdb, romm, logos[0].url,
                   "sgdb-logo-" + std::to_string(logos[0].id), bytes)) return "";
    std::string tex = artBannerFromImage(bytes);
    if (!tex.empty()) {
        entry.sgdbGameId = gameId;
        entry.bannerSource = "sgdb";
        entry.bannerId = logos[0].id;
        aflog.info("sgdb logo hit for '" + entry.query + "'");
    }
    return tex;
}

bool artFromRommCover(SgdbClient& sgdb, RommClient& romm, const std::string& coverPath,
                      bool wantIcon, bool wantBanner, ArtPieces& out) {
    if (coverPath.empty()) return false;
    std::string bytes;
    if (!artGetUrl(sgdb, romm, coverPath, "cover-" + coverPath, bytes)) return false;
    bool ok = true;
    if (wantIcon) {
        out.icon48 = artIcon48FromImage(bytes);
        ok = ok && !out.icon48.empty();
    }
    if (wantBanner) {
        out.bannerTex = artBannerFromImage(bytes);
        ok = ok && !out.bannerTex.empty();
    }
    return ok;
}

// ---- auto resolution ---------------------------------------------------------

int artSgdbStrongMatch(SgdbClient& sgdb, const std::vector<std::string>& queries,
                       std::string* usedQuery) {
    if (usedQuery && !queries.empty()) *usedQuery = queries[0];
    if (!sgdb.hasKey()) return 0;
    for (const std::string& q : queries) {
        if (q.empty()) continue;
        std::vector<SgdbGame> games;
        if (!sgdb.search(q, games)) {
            aflog.error("sgdb search failed: " + sgdb.lastError);
            continue;
        }
        std::vector<std::string> names;
        for (auto& g : games) names.push_back(g.name);
        int best = -1;
        ArtConfidence conf = artConfidence(q, names, &best);
        if (conf == ART_MATCH_STRONG) {
            if (usedQuery) *usedQuery = q;
            aflog.info("sgdb strong id=" + std::to_string(games[best].id) + " for '" + q + "'");
            return games[best].id;
        }
        aflog.info("sgdb match " + std::to_string((int)conf) + " for '" + q + "'");
    }
    return 0;
}

// query tiers: the RomM metadata title (IGDB match by rom id — immune to bad
// file names) first, then the sanitized file name
std::vector<std::string> artQueriesFor(const std::string& fsName,
                                       const std::string& title) {
    std::vector<std::string> qs;
    std::string qt = artSanitizeQuery(title);
    std::string qf = artSanitizeQuery(fsName);
    if (!qt.empty()) qs.push_back(qt);
    if (!qf.empty() && qf != qt) qs.push_back(qf);
    return qs;
}

void artResolveGba(SgdbClient& sgdb, RommClient& romm, const std::string& fsName,
                   const std::string& title, ArtEntry& entry, ArtPieces& out,
                   std::function<void(const std::string&)> status) {
    aflog.info("resolve start: " + fsName);
    // icon: SGDB, only on a strong (auto-pick) match — title tier, then file name
    if (status) status("Searching SteamGridDB...");
    entry.sgdbGameId = artSgdbStrongMatch(sgdb, artQueriesFor(fsName, title), &entry.query);
    if (entry.sgdbGameId) {
        if (status) status("Fetching icon...");
        int pickedId = 0;
        out.icon48 = artSgdbIcon(sgdb, romm, entry.sgdbGameId, &pickedId);
        if (!out.icon48.empty()) {
            entry.iconSource = "sgdb";
            entry.iconId = pickedId;
        }
    }

    // banner: libretro exact-name logo (+ tag-stripped retry)
    if (status) status("Fetching banner logo...");
    std::string usedName;
    out.bannerTex = artLibretroBanner(sgdb, romm, fsName, &usedName);
    if (!out.bannerTex.empty()) {
        entry.bannerSource = "libretro";
        entry.bannerName = usedName;
    }
    aflog.info(std::string("resolve done: icon=") + (out.icon48.empty() ? "miss" : "ok") +
               " banner=" + (out.bannerTex.empty() ? "miss" : "ok"));
}

bool artBuildFromEntry(SgdbClient& sgdb, RommClient& romm, const std::string& fsName,
                       const std::string& coverPath, const ArtEntry& entry, ArtPieces& out) {
    bool ok = true;
    if (entry.iconSource == "sgdb") {
        out.icon48 = artSgdbIconById(sgdb, romm, entry.sgdbGameId, entry.iconId);
        if (out.icon48.empty()) ok = false;
    } else if (entry.iconSource == "romm-cover") {
        ArtPieces cov;
        if (artFromRommCover(sgdb, romm, coverPath, true, false, cov))
            out.icon48 = cov.icon48;
        else ok = false;
    }
    if (entry.bannerSource == "libretro") {
        std::string bytes;
        if (artGetUrl(sgdb, romm, LIBRETRO_GBA_LOGOS + urlEncodePath(entry.bannerName) + ".png",
                      "libretro-gba-" + entry.bannerName, bytes))
            out.bannerTex = artBannerFromImage(bytes);
        if (out.bannerTex.empty()) ok = false;
    } else if (entry.bannerSource == "sgdb") {
        out.bannerTex = artSgdbLogoById(sgdb, romm, entry.sgdbGameId, entry.bannerId);
        if (out.bannerTex.empty()) ok = false;
    } else if (entry.bannerSource == "romm-cover") {
        ArtPieces cov;
        if (artFromRommCover(sgdb, romm, coverPath, false, true, cov))
            out.bannerTex = cov.bannerTex;
        else ok = false;
    }
    return ok;
}
