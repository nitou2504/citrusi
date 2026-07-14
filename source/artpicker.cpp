#include <3ds.h>
#include <citro2d.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "artpicker.hpp"
#include "artquery.hpp"
#include "covercache.hpp"
#include "dialog.hpp"
#include "teximg.hpp"
#include "helpers.hpp"
#include "settings.hpp"
#include "logger.hpp"
extern "C" {
#include "graphics.h"
}

static Logger aplog("artpicker");

extern RommClient gRomm;
extern SgdbClient gSgdb;

#define LIBRETRO_GBA_LOGOS \
    "http://thumbnails.libretro.com/Nintendo%20-%20Game%20Boy%20Advance/Named_Logos/"

namespace {

struct Cand {
    std::string source;      // "sgdb" | "sgdb-grid" | "iisu" | "romm-cover" | "libretro"
    int id = 0;
    int w = 0, h = 0;        // native dims (0 = unknown)
    std::string url;         // full-res url
    std::string thumbUrl;
    std::string cacheKey;    // full-res cache slot
    std::string thumbKey;
    std::string name;        // libretro No-Intro name / short label
    C2D_Image img = {nullptr, nullptr};
    int state = 0;           // 0 pending, 1 loaded, 2 failed
    int tries = 0;           // thumb fetch attempts (cdn timeouts get a retry)
};

void freeCands(std::vector<Cand>& v) {
    for (auto& c : v)
        if (c.img.tex) freeTexImage(&c.img);
    v.clear();
}

// resolve the query to one SGDB game (best-confidence result)
int resolveGame(const std::string& query, std::string* gameName, bool* offline) {
    if (offline) *offline = false;
    if (!gSgdb.hasKey()) return 0;
    aplog.info("resolve game: '" + query + "'");
    std::vector<SgdbGame> games;
    if (!gSgdb.search(query, games)) {
        if (offline) *offline = true;
        aplog.error("search failed: " + gSgdb.lastError);
        return 0;
    }
    if (games.empty()) return 0;
    std::vector<std::string> names;
    for (auto& g : games) names.push_back(g.name);
    int best = -1;
    ArtConfidence conf = artConfidence(query, names, &best);
    if (conf < ART_MATCH_MEDIUM || best < 0) best = 0;
    if (gameName) *gameName = games[best].name;
    return games[best].id;
}

// same preference order as the auto path: SGDB icons first (smallest
// adequate first), then iiSU box arts, the RomM cover tile last (fallback)
void buildIconCands(int gameId, const std::string& query, const std::string& slug,
                    const std::string& coverPath,
                    std::vector<Cand>& out, bool* offline) {
    freeCands(out);
    if (gameId && gSgdb.hasKey()) {
        std::vector<SgdbIcon> icons;
        if (gSgdb.icons(gameId, icons)) {
            artSortIconAssets(icons);   // same preference as the auto pick
            for (auto& i : icons) {
                Cand c;
                c.source = "sgdb";
                c.id = i.id;
                c.w = i.width; c.h = i.height;
                c.url = i.url;
                c.thumbUrl = i.thumbUrl;
                c.cacheKey = "sgdb-icon-" + std::to_string(i.id);
                c.thumbKey = "sgdb-icon-th-" + std::to_string(i.id);
                out.push_back(c);
            }
        } else if (offline) *offline = true;
    }
    // iiSU community box arts (square launcher icons); exact matches first
    std::vector<IisuAsset> iisu;
    if (iisuSearch(gSgdb, query, slug, iisu)) {
        std::string qn = artNorm(query);
        std::stable_sort(iisu.begin(), iisu.end(), [&](const IisuAsset& a, const IisuAsset& b) {
            return (artNorm(a.gameName) == qn) > (artNorm(b.gameName) == qn);
        });
        for (auto& a : iisu) {
            if (a.type != "iisu_box_art") continue;
            Cand c;
            c.source = "iisu";
            c.id = a.id;
            c.w = a.width; c.h = a.height;
            c.url = a.url;
            c.name = a.gameName;
            c.cacheKey = "iisu-" + std::to_string(a.id);
            c.thumbKey = c.cacheKey;   // their thumbnails are webp; reuse the PNG
            out.push_back(c);
        }
    }
    if (!coverPath.empty()) {
        Cand c;
        c.source = "romm-cover";
        c.url = coverPath;
        c.cacheKey = "cover-" + coverPath;
        c.thumbKey = c.cacheKey;
        c.name = "RomM cover";
        out.push_back(c);
    }
    aplog.info("icon cands: " + std::to_string(out.size()));
}

// candidate libretro names: fsName-derived variants, plus (after a refine)
// the corrected query text itself
void buildBannerCands(int gameId, const std::string& query, const std::string& fsName,
                      const std::string& refineName, const std::string& coverPath,
                      const std::string& slug, std::vector<Cand>& out, bool* offline) {
    freeCands(out);
    if (slug == ROMM_SLUG_GBA) {
        std::vector<std::string> names = libretroNameVariants(fsName);
        if (!refineName.empty()) names.insert(names.begin(), refineName);
        for (const std::string& n : names) {
            std::string bytes;
            std::string key = "libretro-gba-" + n;
            if (!artGetUrl(gSgdb, gRomm, LIBRETRO_GBA_LOGOS + urlEncodePath(n) + ".png",
                           key, bytes))
                continue;
            Cand c;
            c.source = "libretro";
            c.name = n;
            c.cacheKey = key;
            c.thumbKey = key;
            out.push_back(c);
            break;   // one logo is enough; variants are the same art
        }
    }
    if (gameId && gSgdb.hasKey()) {
        std::vector<SgdbAsset> logos;
        if (gSgdb.logos(gameId, logos)) {
            for (auto& l : logos) {
                Cand c;
                c.source = "sgdb";
                c.id = l.id;
                c.w = l.width; c.h = l.height;
                c.url = l.url;
                c.thumbUrl = l.thumbUrl;
                c.cacheKey = "sgdb-logo-" + std::to_string(l.id);
                c.thumbKey = "sgdb-logo-th-" + std::to_string(l.id);
                out.push_back(c);
            }
        } else if (offline) *offline = true;
        // horizontal grid capsules: full art + logo, near-banner shape
        std::vector<SgdbAsset> grids;
        if (gSgdb.grids(gameId, grids)) {
            for (auto& g : grids) {
                Cand c;
                c.source = "sgdb-grid";
                c.id = g.id;
                c.w = g.width; c.h = g.height;
                c.url = g.url;
                c.thumbUrl = g.thumbUrl;
                c.cacheKey = "sgdb-grid-" + std::to_string(g.id);
                c.thumbKey = "sgdb-grid-th-" + std::to_string(g.id);
                out.push_back(c);
            }
        }
    }
    // iiSU logos + heroes (banner-shaped community art); exact matches first
    std::vector<IisuAsset> iisu;
    if (iisuSearch(gSgdb, query, slug, iisu)) {
        std::string qn = artNorm(query);
        std::stable_sort(iisu.begin(), iisu.end(), [&](const IisuAsset& a, const IisuAsset& b) {
            return (artNorm(a.gameName) == qn) > (artNorm(b.gameName) == qn);
        });
        for (auto& a : iisu) {
            if (a.type != "logo" && a.type != "hero") continue;
            Cand c;
            c.source = "iisu";
            c.id = a.id;
            c.w = a.width; c.h = a.height;
            c.url = a.url;
            c.name = a.gameName + " (" + a.type + ")";
            c.cacheKey = "iisu-" + std::to_string(a.id);
            c.thumbKey = c.cacheKey;
            out.push_back(c);
        }
    }
    if (!coverPath.empty()) {
        Cand c;
        c.source = "romm-cover";
        c.url = coverPath;
        c.cacheKey = "cover-" + coverPath;
        c.thumbKey = c.cacheKey;
        c.name = "RomM cover";
        out.push_back(c);
    }
    aplog.info("banner cands: " + std::to_string(out.size()));
}

// keys pressed while a thumb transfer was in flight: the transfer is cut so
// input wins, and the presses are replayed into the picker's next frame
#define PICKER_KEYS (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | \
                     KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)
static u32 gPendingKeys = 0;

// fetch + decode ONE pending thumb (called once per frame; blocking but short)
void loadOneThumb(std::vector<Cand>& cands, size_t first, size_t last, int maxW, int maxH) {
    for (size_t i = first; i < last && i < cands.size(); i++) {
        Cand& c = cands[i];
        if (c.state != 0) continue;
        aplog.info("thumb fetch: " + c.thumbKey);
        std::string bytes;
        // ANY picker key during the transfer aborts it (curl progress hook):
        // moving the cursor or picking must never wait for a slow cdn. The
        // aborted thumb stays pending and refetches on a later idle frame.
        gSgdb.abortCheck = []() -> bool {
            hidScanInput();
            gPendingKeys |= hidKeysDown() & PICKER_KEYS;
            return gPendingKeys != 0;
        };
        artGetUrl(gSgdb, gRomm, c.thumbUrl.empty() ? c.url : c.thumbUrl, c.thumbKey, bytes);
        gSgdb.abortCheck = nullptr;
        // cut for input: bytes may hold a PARTIAL transfer — decoding it
        // failed and permanently x-ed the thumb. Always retry later instead.
        if (gPendingKeys) return;
        aplog.info("thumb bytes: " + std::to_string(bytes.size()));
        if (!bytes.empty() && loadTexImage(bytes, &c.img, maxW, maxH)) {
            c.state = 1;
        } else if (++c.tries < 2 && bytes.empty()) {
            // cdn timeout: leave pending, retried on a later frame
            aplog.error("thumb retry: " + c.thumbKey);
        } else {
            c.state = 2;
            aplog.error("thumb fail: " + c.thumbKey);
        }
        return;
    }
}

std::string swkbdQuery(const std::string& prefill) {
    SwkbdState kb;
    char buf[128] = {0};
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 63);
    swkbdSetHintText(&kb, "Game name");
    swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
    if (!prefill.empty()) swkbdSetInitialText(&kb, prefill.c_str());
    if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM) return "";
    return std::string(buf);
}

} // namespace

bool artPickerRun(C3D_RenderTarget* target, const std::string& fsName,
                  const std::string& title, const std::string& coverPath,
                  const std::string& slug, ArtEntry& entry, ArtPieces& pieces,
                  bool wantIcon, bool wantBanner,
                  bool* iconChosen, bool* bannerChosen) {
    if (iconChosen) *iconChosen = false;
    if (bannerChosen) *bannerChosen = false;
    if (!wantIcon && !wantBanner) return false;
    CoverCachePause coverPause;   // picker thumbs own the network
    aplog.info("open: " + fsName);

    std::string query = entry.query.empty() ? artSanitizeQuery(fsName) : entry.query;
    std::string gameName;
    bool offline = false;
    int gameId = entry.sgdbGameId;
    if (!gameId) gameId = resolveGame(query, &gameName, &offline);

    std::vector<Cand> iconCands, bannerCands;
    // a caller-corrected name (S3) also gets tried against libretro
    std::string refineName;
    if (!entry.query.empty() && entry.query != artSanitizeQuery(fsName))
        refineName = entry.query;
    bool iconLoaded = false, bannerLoaded = false;

    int page = wantIcon ? 0 : 1;           // 0 = icon, 1 = banner
    int cursor = 0, scroll = 0;            // scroll = first visible candidate
    bool pickedIcon = false, pickedBanner = false;
    bool anyPicked = false;

    auto ensureCands = [&]() {
        if (page == 0 && !iconLoaded) {
            showLoading(target, {"Searching art...", query});
            buildIconCands(gameId, query, slug, coverPath, iconCands, &offline);
            iconLoaded = true;
        }
        if (page == 1 && !bannerLoaded) {
            showLoading(target, {"Searching art...", query});
            buildBannerCands(gameId, query, fsName, refineName, coverPath, slug, bannerCands, &offline);
            bannerLoaded = true;
        }
    };
    auto invalidate = [&]() {
        freeCands(iconCands);
        freeCands(bannerCands);
        iconLoaded = bannerLoaded = false;
        cursor = scroll = 0;
    };
    auto advance = [&]() -> bool {   // to the other page, or done
        if (page == 0 && wantBanner && !pickedBanner) { page = 1; cursor = scroll = 0; return true; }
        if (page == 1 && wantIcon && !pickedIcon)     { page = 0; cursor = scroll = 0; return true; }
        return false;   // nothing left to choose
    };

    ensureCands();
    aplog.info("loop enter");
    bool firstFrame = true;

    while (aptMainLoop()) {
        std::vector<Cand>& cands = (page == 0) ? iconCands : bannerCands;
        const int cols = (page == 0) ? 5 : 2;
        const int rows = (page == 0) ? 3 : 2;
        const int perPage = cols * rows;
        const int cellW = (page == 0) ? 48 : 128;
        const int cellH = (page == 0) ? 48 : 64;
        const int pitchX = (page == 0) ? 56 : 150;
        // tighter rows: the third icon row used to run under the footer info
        const int pitchY = (page == 0) ? 52 : 74;
        const int gridX = (320 - (cols - 1) * pitchX - cellW) / 2;
        const int gridY = 38;

        hidScanInput();
        u32 kDown = hidKeysDown();
        kDown |= gPendingKeys;   // replay presses that cut an in-flight fetch
        gPendingKeys = 0;
        int count = (int)cands.size();

        if (kDown & KEY_B) {                       // skip this page
            if (!advance()) break;
            ensureCands();
            continue;
        }
        if ((kDown & KEY_Y) && wantIcon && wantBanner) {
            page = 1 - page;
            cursor = scroll = 0;
            ensureCands();
            continue;
        }
        if (kDown & KEY_X) {                       // refine search
            std::string q = swkbdQuery(query);
            if (!q.empty()) {
                query = q;
                refineName = q;
                gameId = resolveGame(query, &gameName, &offline);
                invalidate();
                ensureCands();
            }
            continue;
        }
        if (count > 0) {
            if (kDown & KEY_RIGHT && cursor + 1 < count) cursor++;
            if (kDown & KEY_LEFT  && cursor > 0) cursor--;
            if (kDown & KEY_DOWN  && cursor + cols < count) cursor += cols;
            if (kDown & KEY_UP    && cursor - cols >= 0) cursor -= cols;
            if (kDown & KEY_R && scroll + perPage < count) { scroll += perPage; cursor = scroll; }
            if (kDown & KEY_L && scroll > 0) { scroll -= perPage; cursor = scroll; }
            if (cursor < scroll) scroll = (cursor / perPage) * perPage;
            if (cursor >= scroll + perPage) scroll = (cursor / perPage) * perPage;

            if (kDown & KEY_A) {                   // use the focused candidate
                Cand& c = cands[cursor];
                showLoading(target, {"Fetching art...", title});
                std::string piece;
                bool ok = false;
                if (c.source == "romm-cover") {
                    ArtPieces cov;
                    if (artFromRommCover(gSgdb, gRomm, coverPath, page == 0, page == 1, cov)) {
                        piece = (page == 0) ? cov.icon48 : cov.bannerTex;
                        ok = !piece.empty();
                    }
                } else {
                    piece = artFetchRender(gSgdb, gRomm, c.url, c.cacheKey, page == 0);
                    ok = !piece.empty();
                }
                if (!ok) {
                    Dialog(target, 0, 0, 320, 240, {"Couldn't fetch that one.",
                           offline ? "SteamGridDB unreachable." : "Try another."}, {"OK"}).handle();
                    continue;
                }
                entry.query = query;
                if (gameId) entry.sgdbGameId = gameId;
                if (page == 0) {
                    pieces.icon48 = piece;
                    entry.iconSource = c.source;   // "sgdb" | "iisu" | "romm-cover"
                    entry.iconId = c.id;
                    pickedIcon = true;
                    if (iconChosen) *iconChosen = true;
                } else {
                    pieces.bannerTex = piece;
                    entry.bannerSource = c.source;
                    entry.bannerId = c.id;
                    entry.bannerName = (c.source == "libretro") ? c.name : "";
                    pickedBanner = true;
                    if (bannerChosen) *bannerChosen = true;
                }
                anyPicked = true;
                if (!advance()) break;
                ensureCands();
                continue;
            }
        }

        // one lazy thumb fetch per frame (visible page only)
        loadOneThumb(cands, scroll, scroll + perPage, cellW, cellH);

        // ---- draw ----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(target, COL_BG);
        C2D_SceneBegin(target);
        char head[96];
        snprintf(head, sizeof(head), "%s %s  %d found  %d/%d",
                 (page == 0) ? "ICON" : "BANNER",
                 (wantIcon && wantBanner) ? ((page == 0) ? "(Y: banner)" : "(Y: icon)") : "",
                 count, count ? (scroll / perPage) + 1 : 1,
                 count ? (count + perPage - 1) / perPage : 1);
        drawText(160, 8, 0, 0.55f, COL_BG, COL_TEXT, head, C2D_AlignCenter);

        for (int i = 0; i < perPage && scroll + i < count; i++) {
            Cand& c = cands[scroll + i];
            int cx = gridX + (i % cols) * pitchX;
            int cy = gridY + (i / cols) * pitchY;
            C2D_DrawRectSolid(cx, cy, 0, cellW, cellH, COL_SURFACE);
            if (c.state == 1 && c.img.tex) {
                float iw = c.img.subtex->width, ih = c.img.subtex->height;
                C2D_DrawImageAt(c.img, cx + (cellW - iw) / 2, cy + (cellH - ih) / 2, 0);
            } else if (c.state == 2) {
                drawText(cx + cellW / 2, cy + cellH / 2 - 7, 0, 0.4f, COL_SURFACE, COL_TEXT_DIM, "x", C2D_AlignCenter);
            }
            if (scroll + i == cursor)
                C2DExtra_DrawRectHollow(cx - 2, cy - 2, 0.1f, cellW + 4, cellH + 4, 2, COL_ACCENT);
        }
        if (count == 0) {
            drawText(160, 100, 0, 0.5f, COL_BG, COL_TEXT_DIM,
                     offline ? "SteamGridDB unreachable" : "No results", C2D_AlignCenter);
            drawText(160, 120, 0, 0.45f, COL_BG, COL_TEXT_DIM, "X = search again", C2D_AlignCenter);
        }
        // footer: focused candidate info + controls
        if (count > 0 && cursor < count) {
            Cand& c = cands[cursor];
            char info[96];
            if (c.source == "sgdb" && c.url.size() > 4 &&
                c.url.compare(c.url.size() - 4, 4, ".ico") == 0)
                snprintf(info, sizeof(info), "SteamGridDB  ico (native sizes)");
            else if (c.source == "sgdb-grid")
                snprintf(info, sizeof(info), "SteamGridDB grid  %dx%d", c.w, c.h);
            else if (c.source == "iisu")
                snprintf(info, sizeof(info), "iiSU  %dx%d  %s", c.w, c.h, c.name.c_str());
            else if (c.source == "sgdb" && c.w)
                snprintf(info, sizeof(info), "SteamGridDB  %dx%d", c.w, c.h);
            else if (c.source == "libretro")
                snprintf(info, sizeof(info), "libretro logo");
            else
                snprintf(info, sizeof(info), "%s", c.name.empty() ? c.source.c_str() : c.name.c_str());
            drawText(160, 205, 0, 0.42f, COL_BG, COL_TEXT_DIM, info, C2D_AlignCenter);
        }
        if (!gameName.empty()) {
            // one clipped line between the heading and the grid — a long
            // (refined) name used to paint over the heading and the top row
            std::string gn = gameName.size() > 42 ? gameName.substr(0, 41) + "..." : gameName;
            drawText(160, 26, 0, 0.38f, COL_BG, COL_TEXT_DIM, gn.c_str(), C2D_AlignCenter);
        }
        drawText(160, 227, 0, 0.42f, COL_BG, COL_TEXT_DIM,
                 "A use   X search   B skip   L/R pages", C2D_AlignCenter);
        C3D_FrameEnd(0);
        if (firstFrame) { firstFrame = false; aplog.info("first frame drawn"); }
    }

    freeCands(iconCands);
    freeCands(bannerCands);
    return anyPicked;
}
