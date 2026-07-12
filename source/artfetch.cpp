#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "artfetch.hpp"
#include "teximg.hpp"
#include "helpers.hpp"
#include "json.hpp"
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
    if (rgba.empty()) {
        aflog.error("banner decode fail, " + std::to_string(bytes.size()) + "B");
        return "";
    }
    static u8 canvas[BNR_W * BNR_H * 4];
    memset(canvas, 0, sizeof(canvas));
    int ox = (BNR_W - w) / 2, oy = (BNR_H - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(&canvas[((oy + y) * BNR_W + ox) * 4], &rgba[y * w * 4], w * 4);
    return tile4444(canvas);
}

// ---- .ico container -------------------------------------------------------
// SGDB icon assets are often .ico with native 48x48/24x24 frames — exactly
// the SMDH sizes, so they beat downscaled PNGs when present.

static inline u16 icoRd16(const u8* p) { return p[0] | (p[1] << 8); }
static inline u32 icoRd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

bool artIsIco(const std::string& b) {
    return b.size() >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0;
}

// decodes one ico frame (PNG or 32/24/8bpp BMP + AND mask) to RGBA
static std::vector<u8> icoFrameRGBA(const u8* d, u32 size, int* w, int* h) {
    std::vector<u8> out;
    if (size >= 8 && memcmp(d, "\x89PNG", 4) == 0) {
        int comp = 0;
        u8* px = stbi_load_from_memory(d, size, w, h, &comp, 4);
        if (!px) return out;
        out.assign(px, px + (size_t)*w * *h * 4);
        stbi_image_free(px);
        return out;
    }
    if (size < 40 || icoRd32(d) != 40) return out;          // BITMAPINFOHEADER only
    int bw = (int)icoRd32(d + 4);
    int bh = (int)icoRd32(d + 8) / 2;                        // height counts XOR+AND
    u16 bpp = icoRd16(d + 14);
    if (icoRd32(d + 16) != 0) return out;                    // BI_RGB only
    if (bw <= 0 || bh <= 0 || bw > 512 || bh > 512) return out;
    u32 palN = (bpp == 8) ? (icoRd32(d + 32) ? icoRd32(d + 32) : 256) : 0;
    const u8* pal = d + 40;
    const u8* xorD = pal + palN * 4;
    u32 xorStride = ((u32)bw * bpp + 31) / 32 * 4;
    u32 andStride = ((u32)bw + 31) / 32 * 4;
    const u8* andD = xorD + xorStride * bh;
    if ((u32)(andD - d) + andStride * bh > size) return out;
    out.resize((size_t)bw * bh * 4);
    bool anyAlpha = false;
    for (int y = 0; y < bh; y++) {
        const u8* row = xorD + xorStride * (bh - 1 - y);     // bottom-up
        const u8* mrow = andD + andStride * (bh - 1 - y);
        for (int x = 0; x < bw; x++) {
            u8* o = &out[((size_t)y * bw + x) * 4];
            if (bpp == 32) {
                o[0] = row[x*4+2]; o[1] = row[x*4+1]; o[2] = row[x*4]; o[3] = row[x*4+3];
                if (o[3]) anyAlpha = true;
            } else if (bpp == 24) {
                o[0] = row[x*3+2]; o[1] = row[x*3+1]; o[2] = row[x*3]; o[3] = 255;
            } else if (bpp == 8) {
                const u8* c = &pal[(u32)row[x] * 4];
                o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = 255;
            } else return std::vector<u8>();                 // 4/1bpp: unsupported
            if ((mrow[x >> 3] >> (7 - (x & 7))) & 1) o[3] = 0;   // AND mask = transparent
        }
    }
    // 32bpp icons with an all-zero alpha channel rely on the AND mask alone
    if (bpp == 32 && !anyAlpha)
        for (int y = 0; y < bh; y++) {
            const u8* mrow = andD + andStride * (bh - 1 - y);
            for (int x = 0; x < bw; x++)
                out[((size_t)y * bw + x) * 4 + 3] =
                    ((mrow[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;
        }
    *w = bw; *h = bh;
    return out;
}

// best frame for the 48px target: smallest frame >= 48, else the largest
static std::vector<u8> icoBestRGBA(const std::string& bytes, int* w, int* h) {
    const u8* d = (const u8*)bytes.data();
    u32 n = icoRd16(d + 4);
    int best = -1, bestDim = 0;
    for (u32 i = 0; i < n && 6 + i * 16 + 16 <= bytes.size(); i++) {
        const u8* e = d + 6 + i * 16;
        int fw = e[0] ? e[0] : 256, fh = e[1] ? e[1] : 256;
        int dim = std::min(fw, fh);
        bool better = (best < 0) ||
                      (dim >= ICON_DIM && (bestDim < ICON_DIM || dim < bestDim)) ||
                      (dim < ICON_DIM && bestDim < ICON_DIM && dim > bestDim);
        if (better) { best = (int)i; bestDim = dim; }
    }
    if (best < 0) return std::vector<u8>();
    const u8* e = d + 6 + best * 16;
    u32 fsize = icoRd32(e + 8), foff = icoRd32(e + 12);
    if (foff + fsize > bytes.size()) return std::vector<u8>();
    return icoFrameRGBA(d + foff, fsize, w, h);
}

std::string artIcon48FromImage(const std::string& bytes) {
    // source pixels: .ico container (best frame) or any stbi format
    std::vector<u8> rgba;
    int sw = 0, sh = 0;
    if (artIsIco(bytes)) {
        rgba = icoBestRGBA(bytes, &sw, &sh);
        if (!rgba.empty()) aflog.info("ico frame " + std::to_string(sw) + "x" + std::to_string(sh));
    } else {
        int comp = 0;
        u8* px = stbi_load_from_memory((const u8*)bytes.data(), bytes.size(), &sw, &sh, &comp, 4);
        if (px) {
            rgba.assign(px, px + (size_t)sw * sh * 4);
            stbi_image_free(px);
        }
    }
    if (rgba.empty() || sw <= 0 || sh <= 0) {
        aflog.error("icon decode fail, " + std::to_string(bytes.size()) + "B");
        return "";
    }

    // white 48x48 canvas, contain-fit centered: area average when shrinking,
    // nearest when growing (degenerate 1x1 box) — never bilinear on pixel art
    static u8 canvas[ICON_DIM * ICON_DIM * 4];
    for (int i = 0; i < ICON_DIM * ICON_DIM; i++) {
        canvas[i*4] = canvas[i*4+1] = canvas[i*4+2] = 255;
        canvas[i*4+3] = 255;
    }
    float scale = std::min((float)ICON_DIM / sw, (float)ICON_DIM / sh);
    int dw = std::max(1, (int)(sw * scale)), dh = std::max(1, (int)(sh * scale));
    if (dw > ICON_DIM) dw = ICON_DIM;
    if (dh > ICON_DIM) dh = ICON_DIM;
    int ox = (ICON_DIM - dw) / 2, oy = (ICON_DIM - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
            if (x1 > sw) x1 = sw;
            if (y1 > sh) y1 = sh;
            u32 acc[4] = {0, 0, 0, 0};
            u32 cnt = (u32)((x1 - x0) * (y1 - y0));
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++)
                    for (int c = 0; c < 4; c++)
                        acc[c] += rgba[((size_t)sy * sw + sx) * 4 + c];
            u32 a = acc[3] / cnt;
            u8* dpx = &canvas[((oy + y) * ICON_DIM + ox + x) * 4];
            dpx[0] = (u8)(((acc[0] / cnt) * a + 255 * (255 - a)) / 255);
            dpx[1] = (u8)(((acc[1] / cnt) * a + 255 * (255 - a)) / 255);
            dpx[2] = (u8)(((acc[2] / cnt) * a + 255 * (255 - a)) / 255);
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
    if (url.empty()) return false;
    // ALL art fetches go over curl/soc — httpc requests from this flow hung
    // the app (see GBA-PLAN); curl handles plain http (libretro) and the LAN
    // RomM host fine. Server-relative paths get host + Basic auth.
    std::string full = url;
    std::string auth;
    if (url[0] == '/') {
        full = romm.host + url;
        auth = "Authorization: Basic " + base64Encode(romm.user + ":" + romm.pass);
    }
    if (!sgdb.fetchUrl(full, bytes, auth) || bytes.empty()) return false;
    artCacheWrite(cacheKey, bytes);
    return true;
}

// ---- source fetchers --------------------------------------------------------

std::string artLibretroBanner(SgdbClient& sgdb, RommClient& romm,
                              const std::string& fsName, std::string* usedName) {
    for (const std::string& name : libretroNameVariants(fsName)) {
        std::string url = LIBRETRO_GBA_LOGOS + urlEncodePath(name) + ".png";
        std::string key = "libretro-gba-" + name;
        std::string bytes;
        if (!artGetUrl(sgdb, romm, url, key, bytes))
            continue;
        std::string tex = artBannerFromImage(bytes);
        if (tex.empty()) {
            // a truncated download may have been cached (e.g. by the freeze
            // session): purge the slot and refetch once
            aflog.error("libretro decode fail (" + std::to_string(bytes.size()) +
                        "B cached), refetching: " + name);
            remove(artCachePath(key).c_str());
            bytes.clear();
            if (artGetUrl(sgdb, romm, url, key, bytes))
                tex = artBannerFromImage(bytes);
        }
        if (!tex.empty()) {
            aflog.info("libretro logo hit: " + name);
            if (usedName) *usedName = name;
            return tex;
        }
    }
    return "";
}

bool artAssetIsIco(const SgdbAsset& a) {
    return a.url.size() > 4 && a.url.compare(a.url.size() - 4, 4, ".ico") == 0;
}

// preference order shared by the auto pick and the picker grid: .ico assets
// first (they usually hold native 48px frames), then adequate PNGs by
// ascending area, then the too-small ones by descending area
void artSortIconAssets(std::vector<SgdbIcon>& list) {
    auto rank = [](const SgdbIcon& a) -> int {
        if (artAssetIsIco(a)) return 0;
        return (std::min(a.width, a.height) >= ICON_DIM) ? 1 : 2;
    };
    std::stable_sort(list.begin(), list.end(), [&](const SgdbIcon& a, const SgdbIcon& b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        long aa = (long)a.width * a.height, ba = (long)b.width * b.height;
        if (ra == 1) return aa < ba;
        if (ra == 2) return aa > ba;
        return false;                 // icos keep the api (score) order
    });
}

std::string artSgdbIcon(SgdbClient& sgdb, RommClient& romm, int gameId, int* pickedId) {
    std::vector<SgdbIcon> list;
    if (!sgdb.icons(gameId, list) || list.empty()) return "";
    artSortIconAssets(list);
    aflog.info("icons: " + std::to_string(list.size()) + " assets");
    int tries = 0;
    for (auto& a : list) {
        if (++tries > 4) break;       // a bad asset shouldn't stall the install
        std::string bytes;
        if (!artGetUrl(sgdb, romm, a.url, "sgdb-icon-" + std::to_string(a.id), bytes))
            continue;
        std::string icon = artIcon48FromImage(bytes);
        if (!icon.empty()) {
            aflog.info("icon picked id=" + std::to_string(a.id) + " " +
                       (artAssetIsIco(a) ? "ico" : std::to_string(a.width) + "x" + std::to_string(a.height)));
            if (pickedId) *pickedId = a.id;
            return icon;
        }
    }
    return "";
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

std::string artSgdbGridById(SgdbClient& sgdb, RommClient& romm, int gameId, int assetId) {
    std::string bytes = artCacheRead("sgdb-grid-" + std::to_string(assetId));
    if (bytes.empty()) {
        std::vector<SgdbAsset> list;
        if (!sgdb.grids(gameId, list)) return "";
        for (auto& a : list)
            if (a.id == assetId) {
                artGetUrl(sgdb, romm, a.url, "sgdb-grid-" + std::to_string(assetId), bytes);
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

// ---- iiSU community asset server -------------------------------------------

#define IISU_BASE "https://assets.iisu.community"

static const char* iisuPlatformFor(const std::string& slug) {
    if (slug == ROMM_SLUG_GBA) return "Game Boy Advance";
    if (slug == ROMM_SLUG_3DS) return "Nintendo 3DS";
    return "Nintendo DS";
}

bool iisuSearch(SgdbClient& net, const std::string& query, const std::string& slug,
                std::vector<IisuAsset>& out) {
    out.clear();
    std::string body;
    std::string url = std::string(IISU_BASE) + "/api/explorer/all-assets/list?limit=40&platform=" +
                      urlEncodePath(iisuPlatformFor(slug)) + "&q=" + urlEncodePath(query);
    if (!net.fetchUrl(url, body)) return false;      // search results: never disk-cached
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        for (auto& a : j["assets"]) {
            IisuAsset ia;
            ia.id = a.value("id", 0);
            ia.type = a.value("asset_type", "");
            ia.width = a.value("width", 0);
            ia.height = a.value("height", 0);
            ia.gameName = a.value("game_name", "");
            ia.url = std::string(IISU_BASE) + "/api/assets/" + std::to_string(ia.id) + "/download";
            if (ia.id) out.push_back(ia);
        }
    } catch (...) {
        aflog.error("bad JSON from iisu");
        return false;
    }
    aflog.info("iisu: " + std::to_string(out.size()) + " assets for '" + query + "'");
    return true;
}

static std::string iisuFetch(SgdbClient& sgdb, RommClient& romm, int assetId, std::string& bytes) {
    std::string url = std::string(IISU_BASE) + "/api/assets/" + std::to_string(assetId) + "/download";
    artGetUrl(sgdb, romm, url, "iisu-" + std::to_string(assetId), bytes);
    return bytes;
}

std::string artIisuIcon48ById(SgdbClient& sgdb, RommClient& romm, int assetId) {
    std::string bytes;
    iisuFetch(sgdb, romm, assetId, bytes);
    return bytes.empty() ? "" : artIcon48FromImage(bytes);
}

std::string artIisuBannerById(SgdbClient& sgdb, RommClient& romm, int assetId) {
    std::string bytes;
    iisuFetch(sgdb, romm, assetId, bytes);
    return bytes.empty() ? "" : artBannerFromImage(bytes);
}

std::string artIisuIconAuto(SgdbClient& sgdb, RommClient& romm, const std::string& query,
                            const std::string& slug, ArtEntry& entry) {
    std::vector<IisuAsset> assets;
    if (!iisuSearch(sgdb, query, slug, assets)) return "";
    std::string qn = artNorm(query);
    for (auto& a : assets) {
        if (a.type != "iisu_box_art" || artNorm(a.gameName) != qn) continue;
        std::string icon = artIisuIcon48ById(sgdb, romm, a.id);
        if (!icon.empty()) {
            aflog.info("iisu icon hit id=" + std::to_string(a.id) + " '" + a.gameName + "'");
            entry.iconSource = "iisu";
            entry.iconId = a.id;
            return icon;
        }
    }
    return "";
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
    if (out.icon48.empty()) {   // iiSU box art on an exact name match
        if (status) status("Searching iiSU...");
        for (const std::string& q : artQueriesFor(fsName, title)) {
            out.icon48 = artIisuIconAuto(sgdb, romm, q, ROMM_SLUG_GBA, entry);
            if (!out.icon48.empty()) { entry.query = q; break; }
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
    } else if (entry.iconSource == "iisu") {
        out.icon48 = artIisuIcon48ById(sgdb, romm, entry.iconId);
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
    } else if (entry.bannerSource == "sgdb-grid") {
        out.bannerTex = artSgdbGridById(sgdb, romm, entry.sgdbGameId, entry.bannerId);
        if (out.bannerTex.empty()) ok = false;
    } else if (entry.bannerSource == "iisu") {
        out.bannerTex = artIisuBannerById(sgdb, romm, entry.bannerId);
        if (out.bannerTex.empty()) ok = false;
    } else if (entry.bannerSource == "romm-cover") {
        ArtPieces cov;
        if (artFromRommCover(sgdb, romm, coverPath, false, true, cov))
            out.bannerTex = cov.bannerTex;
        else ok = false;
    }
    return ok;
}
