#include <3ds.h>
#include <citro2d.h>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <cctype>
#include <ctime>
#include "menu.hpp"
extern "C" {
#include "graphics.h"
#include "nds.h"
}
#include "builder.hpp"
#include "dialog.hpp"
#include "settings.hpp"
#include "config.hpp"
#include "helpers.hpp"
#include "lang.hpp"
#include "manage.hpp"
#include "zip.hpp"
#include "ctrbuilder.hpp"
#include "ciainstall.hpp"
#include "installed3ds.hpp"
#include "installedtitles.hpp"
#include "boxart.hpp"
#include "cwav.hpp"
#include "teximg.hpp"
#include "covercache.hpp"
#include "librefresh.hpp"
#include "logger.hpp"
#include "json.hpp"
#include "sgdb.hpp"
#include "artfetch.hpp"
#include "artpicker.hpp"

static Logger rlog("romm");

#define MAX_DSIWARE 40

// id 0 was the Random-title-ID toggle: now asked at install time when the
// title id is already taken (TWL forwarders), so the setting is gone
#define SETTING_CUSTOM_TITLE 1
#define SETTING_FORCE 2
#define SETTING_LANGUAGE 3
#define SETTING_TEMPLATE 4
#define SETTING_SERVER 5
#define SETTING_IMPORT 6
#define SETTING_SRV_HOST 10
#define SETTING_SRV_USER 11
#define SETTING_SRV_PASS 12
#define SETTING_SRV_TEST 13
// id 7 was "Show 3DS .3ds": removed — only decrypted .cia files install
#define SETTING_ART_NOTIFY 8
#define SETTING_SGDB_KEY 9
#define SETTING_GBA_SCREEN 20   // outside the server-row range (10-13)
#define SETTING_MANAGE_ART 21
#define SETTING_ART_CACHE 22
#define SETTING_DELETE_SRC 23

// downloaded covers + generated previews/icons — safe to wipe, all of it
// re-downloads or rebuilds on demand. art.json (the user's picks) is NOT here.
static const char* kArtCacheDirs[] = {"/cache/", "/banners-cache/", "/titleicons/"};
static void artCacheStats(int& files, u64& bytes) {
    files = 0; bytes = 0;
    std::error_code ec;
    for (const char* d : kArtCacheDirs) {
        for (auto& de : std::filesystem::directory_iterator(FORWARDER_DIR + d, ec)) {
            if (!de.is_regular_file(ec)) continue;
            files++;
            bytes += de.file_size(ec);
        }
    }
}
static int artCacheClear() {
    int gone = 0;
    std::error_code ec;
    for (const char* d : kArtCacheDirs) {
        std::vector<std::filesystem::path> victims;
        for (auto& de : std::filesystem::directory_iterator(FORWARDER_DIR + d, ec))
            if (de.is_regular_file(ec)) victims.push_back(de.path());
        for (auto& p : victims)
            if (std::filesystem::remove(p, ec)) gone++;
    }
    return gone;
}

static CtrBuilder gCtr;
static bool gCtrReady = false;

// vertical action menu (defined below) — used by the early install helpers
struct MenuOpt {
    std::string label;
    std::string desc;
};
static int actionMenu(C3D_RenderTarget* target, const std::string& title,
                      const std::string& subtitle,
                      const std::vector<MenuOpt>& opts, int def = 0,
                      const char* xLabel = nullptr, bool* xOut = nullptr);

SgdbClient gSgdb;
static bool gSgdbKeyTried = false;
// loads the SGDB key once per session; false = no key (icons fall back)
static bool ensureSgdb() {
    if (!gSgdbKeyTried) {
        gSgdbKeyTried = true;
        gSgdb.loadKey();
    }
    return gSgdb.hasKey();
}

static bool ensureCtrBuilder(C3D_RenderTarget* target) {
    if (gCtrReady) return true;
    ReturnResult* r = gCtr.initialize();
    gCtrReady = r->isSuccess();
    if (!gCtrReady)
        Dialog(target,0,0,320,240,{"CTR template error",r->message},{"OK"}).handle();
    delete r;
    return gCtrReady;
}

// builds + installs a CTR forwarder for an on-SD rom. Banner art tiers
// (ART-UX-SPEC): art.json reuse -> SD assets/YANBF/GameTDB chain -> SGDB
// logo (strong match) -> notify [Search]/[Use RomM cover] -> DS-icon stamp.
// The SMDH icon is always the ROM's own DS icon. Shows its own progress.
// interactive=false (batch phase 2) suppresses the missing-art notify and the
// per-item failure dialog so the unattended install never stops for input;
// the progress dialogs still show.
static bool buildForwarderFor(C3D_RenderTarget* target, Config* config,
                              const std::string& romPath, const std::string& title,
                              const std::string& coverPath, bool pickArt = false,
                              bool interactive = true) {
    if (!ensureCtrBuilder(target)) return false;
    CoverCachePause coverPause;   // own httpc + SD while fetching art/sound
    std::string romBase = std::filesystem::path(romPath).filename().generic_string();
    ensureSgdb();

    ArtEntry ae = artStoreGet(romBase);
    std::string boxart;
    std::string customIcon;   // "" = the ROM's own DS icon (the default)
    bool persist = false;
    if (pickArt) {                                // "+ choose art": picker first
        // same which-art menu as GBA — but the icon default here is the
        // ROM's own DS icon, so banner-only leads (cosmetics beyond that)
        bool pIcon = false, pBanner = true;
        if (interactive) {
            int w = actionMenu(target, "Choose which art?", title, {
                {"Banner only", "The big HOME banner. The icon stays the ROM's own DS icon - every NDS game ships one."},
                {"Icon + banner", "Pick a custom HOME icon too. Cosmetic only - the DS icon is the classic default."},
                {"Icon only", "Just the 48px HOME icon; the banner resolves automatically."}});
            if (w >= 0) {
                pIcon   = (w == 1 || w == 2);
                pBanner = (w == 0 || w == 1);
            }
        }
        ArtEntry pe = ae;
        if (pe.query.empty()) {
            std::vector<std::string> qs = artQueriesFor(romBase, title);
            pe.query = qs.empty() ? artSanitizeQuery(romBase) : qs[0];
        }
        ArtPieces picked;
        bool iCh = false, bCh = false;
        artPickerRun(target, romBase, title, coverPath, ROMM_SLUG_NDS,
                     pe, picked, pIcon, pBanner, &iCh, &bCh);
        if (bCh) boxart = picked.bannerTex;
        if (iCh) customIcon = picked.icon48;
        if (iCh || bCh) {
            pe.weak = false;
            ae = pe;
            persist = true;
        }
    }
    if (ae.valid && (boxart.empty() || (customIcon.empty() && !ae.iconSource.empty()))) {
        // F6: reuse silently — banner AND any previously chosen icon
        showLoading(target, {"Preparing art...", title});
        ArtPieces p;
        if (artBuildFromEntry(gSgdb, gRomm, romBase, coverPath, ae, p)) {
            if (boxart.empty() && !ae.bannerSource.empty()) boxart = p.bannerTex;
            if (customIcon.empty()) customIcon = p.icon48;
        }
    }
    if (boxart.empty()) {
        // Dialog word-wraps each message line — pass full titles, no shorten()
        Dialog(target,0,0,320,240,{"Preparing art...",title},{},0).handle();
        boxart = fetchBoxart(gRomm, romPath, coverPath);
    }
    if (boxart.empty()) {                          // SGDB logo tier (auto, strong only)
        ArtEntry ne;
        boxart = artSgdbLogoAuto(gSgdb, gRomm, artQueriesFor(romBase, title), ne);
        if (!boxart.empty()) { ae = ne; persist = true; }
    }
    if (boxart.empty()) {                          // iiSU logo, last of the logo tiers
        ArtEntry ne;
        for (const std::string& q : artQueriesFor(romBase, title)) {
            ne.query = q;
            boxart = artIisuBannerAuto(gSgdb, gRomm, q, ROMM_SLUG_NDS, ne);
            if (!boxart.empty()) { ae = ne; persist = true; break; }
        }
    }
    if (boxart.empty())                            // GameTDB box — just above the RomM cover
        boxart = gametdbBoxart(gRomm, romPath);
    if (boxart.empty() && config && config->artNotify && interactive) {   // S2, banner line only
        ArtEntry ne;
        std::vector<std::string> qs = artQueriesFor(romBase, title);
        ne.query = qs.empty() ? artSanitizeQuery(romBase) : qs[0];
        const char* coverBtn = coverPath.empty() ? "Use DS icon" : "Use cover";
        for (;;) {
            int c = Dialog(target,0,0,320,240,{"Art not found:","banner: not found for \""+ne.query+"\""},{"Search",coverBtn}).handle();
            if (c != 0) break;
            SwkbdState kb;
            char buf[128] = {0};
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 63);
            swkbdSetHintText(&kb, "Game name");
            swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
            swkbdSetInitialText(&kb, ne.query.c_str());
            if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM || !buf[0])
                continue;
            ne.query = buf;
            ne.sgdbGameId = 0;
            ArtPieces picked;
            if (artPickerRun(target, romBase, title, coverPath, ROMM_SLUG_NDS,
                             ne, picked, false, true)) {
                boxart = picked.bannerTex;
                ne.weak = false;
                ae = ne; persist = true;
            }
            break;
        }
    }
    if (boxart.empty() && !coverPath.empty()) {    // RomM cover fallback (⚠)
        ArtPieces cov;
        if (artFromRommCover(gSgdb, gRomm, coverPath, false, true, cov) && !cov.bannerTex.empty()) {
            boxart = cov.bannerTex;
            ArtEntry ne;
            ne.query = artSanitizeQuery(romBase);
            ne.bannerSource = "romm-cover";
            ne.weak = true;
            ae = ne; persist = true;
        }
    }
    if (boxart.empty())                            // last resort: DS-icon stamp
        boxart = dsIconBanner(romPath);

    Dialog(target,0,0,320,240,{"Fetching sound...",title},{},0).handle();
    std::string gameCwav = fetchGameSound(gRomm, romPath);
    Dialog(target,0,0,320,240,{gLang.getString("menu_installing"),title},{},0).handle();
    u64 ctid = gCtr.allocateTID(romBase);
    if (ctid == 0) {
        Dialog(target,0,0,320,240,{"No free install slots"},{"OK"}).handle();
        return false;
    }
    ReturnResult* r = gCtr.buildCIA(romPath, title, ctid, boxart, gameCwav, customIcon);
    bool ok = r->isSuccess();
    if (!ok) {
        if (interactive)
            Dialog(target,0,0,320,240,{"Install failed",r->message,gLang.parseString("format_hex",(u32)r->code)},{"OK"}).handle();
    }
    else if (persist)
        artStorePut(romBase, ae);
    delete r;
    return ok;
}

// GBA art (ART-UX-SPEC S1-S4): art.json reuse -> auto resolve -> optional
// missing-art notify with [Search]->picker / [Use RomM cover]. Never blocks
// the install; pieces left "" keep the template art. The caller persists
// entryOut with artStorePut() after a successful install/rebuild.
// forcePicker = the user asked to choose ("+ Art" / Manage "Change art"):
// skip the notify and open the picker directly, preloaded from art.json.
static void resolveGbaArtInteractive(C3D_RenderTarget* target, Config* config,
                                     const std::string& fsName, const std::string& title,
                                     const std::string& coverPath,
                                     ArtEntry& entryOut, ArtPieces& piecesOut,
                                     bool forcePicker = false,
                                     bool pickIcon = true, bool pickBanner = true) {
    // own the network while art resolves: the cover prefetch worker shares
    // httpc and the SD card with us
    CoverCachePause coverPause;
    ensureSgdb();
    ArtEntry ae = artStoreGet(fsName);
    if (ae.valid && !forcePicker) {   // F6: reinstall reuses silently, cache-first
        showLoading(target, {"Preparing art...", title});
        artBuildFromEntry(gSgdb, gRomm, fsName, coverPath, ae, piecesOut);
        // fill gaps from the cover even when the entry "rebuilt" fine: an
        // entry whose art came from the cover fallback has no sources, and
        // an empty rebuild must never bake the template over existing art
        if (piecesOut.icon48.empty() || piecesOut.bannerTex.empty())
            artFromRommCover(gSgdb, gRomm, coverPath,
                             piecesOut.icon48.empty(), piecesOut.bannerTex.empty(), piecesOut);
        entryOut = ae;
        return;
    }
    if (ae.valid) {   // Change art: picker preloaded from the persisted entry
        entryOut = ae;
        bool iCh = false, bCh = false;
        artPickerRun(target, fsName, title, coverPath, ROMM_SLUG_GBA,
                     entryOut, piecesOut, pickIcon, pickBanner, &iCh, &bCh);
        // pages the user skipped keep their stored art
        if (piecesOut.icon48.empty() || piecesOut.bannerTex.empty()) {
            ArtPieces re;
            artBuildFromEntry(gSgdb, gRomm, fsName, coverPath, entryOut, re);
            if (piecesOut.icon48.empty()) piecesOut.icon48 = re.icon48;
            if (piecesOut.bannerTex.empty()) piecesOut.bannerTex = re.bannerTex;
        }
        // a stored piece that can't be rebuilt must never bake the template:
        // fall to the cover rather than losing the installed art
        if (piecesOut.icon48.empty() || piecesOut.bannerTex.empty()) {
            ArtPieces cov;
            artFromRommCover(gSgdb, gRomm, coverPath,
                             piecesOut.icon48.empty(), piecesOut.bannerTex.empty(), cov);
            if (piecesOut.icon48.empty()) piecesOut.icon48 = cov.icon48;
            if (piecesOut.bannerTex.empty()) piecesOut.bannerTex = cov.bannerTex;
        }
        if (iCh || bCh) entryOut.weak = false;   // the user picked -> ⚠ cleared
        return;
    }
    artResolveGba(gSgdb, gRomm, fsName, title, entryOut, piecesOut,
                  [&](const std::string& msg) { showLoading(target, {msg, title}); });
    if (forcePicker) {
        artPickerRun(target, fsName, title, coverPath, ROMM_SLUG_GBA,
                     entryOut, piecesOut, pickIcon, pickBanner);
    }
    bool iconMiss = piecesOut.icon48.empty();
    bool bannerMiss = piecesOut.bannerTex.empty();
    if ((iconMiss || bannerMiss) && config->artNotify && !forcePicker) {
        const char* coverBtn = coverPath.empty() ? "Use tile" : "Use cover";
        for (;;) {
            std::string q = entryOut.query;
            int c;
            if (iconMiss && bannerMiss)
                c = Dialog(target,0,0,320,240,{"Art not found:","icon: no match for \""+q+"\"","banner: not found"},{"Search",coverBtn}).handle();
            else if (iconMiss)
                c = Dialog(target,0,0,320,240,{"Art not found:","icon: no match for \""+q+"\""},{"Search",coverBtn}).handle();
            else
                c = Dialog(target,0,0,320,240,{"Art not found:","banner: not found"},{"Search",coverBtn}).handle();
            if (c != 0) break;                      // cover/tile fallback below
            // S3: correct the name, then the picker for the missing pieces
            SwkbdState kb;
            char buf[128] = {0};
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 63);
            swkbdSetHintText(&kb, "Game name");
            swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
            swkbdSetInitialText(&kb, q.c_str());
            if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM || !buf[0])
                continue;                           // cancelled -> back to the notify
            entryOut.query = buf;
            entryOut.sgdbGameId = 0;                // re-resolve for the corrected name
            artPickerRun(target, fsName, title, coverPath, ROMM_SLUG_GBA,
                         entryOut, piecesOut, iconMiss, bannerMiss);
            break;
        }
    }
    // whatever is still missing falls back to the cover (or stays template)
    bool fellBack = false;
    if (piecesOut.icon48.empty() || piecesOut.bannerTex.empty()) {
        ArtPieces cov;
        artFromRommCover(gSgdb, gRomm, coverPath,
                         piecesOut.icon48.empty(), piecesOut.bannerTex.empty(), cov);
        if (piecesOut.icon48.empty() && !cov.icon48.empty()) {
            piecesOut.icon48 = cov.icon48;
            entryOut.iconSource = "romm-cover";
            fellBack = true;
        }
        if (piecesOut.bannerTex.empty() && !cov.bannerTex.empty()) {
            piecesOut.bannerTex = cov.bannerTex;
            entryOut.bannerSource = "romm-cover";
            fellBack = true;
        }
    }
    entryOut.weak = fellBack ||
                    piecesOut.icon48.empty() || piecesOut.bannerTex.empty();
}

// build + install a GBA VC inject for an on-SD rom with pre-resolved art
// (same allocate/build/progress/persist shape as the RomM GBA install path).
// the screen preset a (re)bake of this game should use: the one already baked
// into its install when known (art.json), else the Settings default
static int gbaScreenFor(const ArtEntry& ae, Config* config) {
    return (ae.screen >= 0) ? ae.screen % GBA_SCREEN_COUNT
                            : config->gbaScreen % GBA_SCREEN_COUNT;
}

// *cancelled (optional) reports a B-cancel so batch flows can stop early;
// quiet suppresses the per-item failure dialog (unattended batch phase).
static bool installGbaInject(C3D_RenderTarget* target, Config* config,
                             const std::string& romPath, const std::string& title,
                             const std::string& fsName, const ArtEntry& ae,
                             const ArtPieces& pieces, bool* cancelled = nullptr,
                             bool quiet = false) {
    if (cancelled) *cancelled = false;
    std::string romBase = std::filesystem::path(romPath).filename().generic_string();
    u64 gtid = gCtr.allocateGbaTID(romBase);
    if (gtid == 0) {
        if (!quiet) Dialog(target,0,0,320,240,{"No free install slots"},{"OK"}).handle();
        return false;
    }
    Dialog(target,0,0,320,240,{"Installing...",title},{},0).handle();
    u64 lastG = 0;
    int mode = gbaScreenFor(ae, config);   // reinstalls keep the game's preset
    ReturnResult* gr = gCtr.buildGbaCIA(romPath, title, gtid, pieces.icon48, pieces.bannerTex,
                                        mode,
        [&](u64 done, u64 total) -> bool {
            hidScanInput();
            if (hidKeysDown() & KEY_B) return false;
            if (done - lastG < (2<<20) && done != total) return true;
            lastG = done;
            int pct = (total>0)?(int)(done*100/total):0;
            Dialog(target,0,0,320,240,{"Installing... (B = cancel)",title,std::to_string(pct)+"%"},{},0).handle();
            return true;
        });
    bool ok = gr->isSuccess();
    if (ok) { ArtEntry e2 = ae; e2.screen = mode; artStorePut(fsName, e2); }
    else {
        if (cancelled) *cancelled = (gr->message == "cancelled");
        if (!quiet)
            Dialog(target,0,0,320,240,{(gr->message=="cancelled")?"Install cancelled":"Install failed",gr->message,gLang.parseString("format_hex",(u32)gr->code)},{"OK"}).handle();
    }
    delete gr;
    return ok;
}

static bool isZipName(const std::string& n) {
    std::string l = toLowerCase(n);
    return l.size() > 4 && l.rfind(".zip") == l.size()-4;
}
// expected local playable rom path for a server file name
// (zips extract to <stem>.<platform ext>; inner name may differ, best effort)
static std::string rommLocalPath(const std::string& fsName, const std::string& slug) {
    std::string dir = rommDirFor(slug);
    if (!isZipName(fsName)) return dir + fsName;
    const char* ext = (slug == ROMM_SLUG_GBA) ? ".gba" : ".nds";
    return dir + fsName.substr(0, fsName.size()-4) + ext;
}

// normalized names of NDS roms that have ANY forwarder installed (TWL/YANBF/romm3ds)
static std::set<std::string> gFwdNames;
static bool gFwdReady = false;
static void refreshNdsForwarders();
static bool ndsForwarderInstalled(const std::string& fsName);

// Browse SD: turn a local row's file into a playable rom path. A .zip is
// extracted next to itself (deterministic name = zip stem + platform ext) and
// zipConsumed is set so the caller deletes the archive after a good install.
// A plain rom is returned as-is. Returns "" on failure (err set; "cancelled"
// when the user pressed B during extraction).
static std::string resolveLocalRom(C3D_RenderTarget* target, const std::string& path,
                                   const std::string& slug, bool& zipConsumed,
                                   std::string& err) {
    zipConsumed = false;
    std::filesystem::path pp(path);
    std::string fname = pp.filename().generic_string();
    if (!isZipName(fname)) return path;
    std::string dir = pp.parent_path().generic_string() + "/";
    std::string forceName = pp.stem().generic_string() +
                            (slug == ROMM_SLUG_3DS ? ".cia" : slug == ROMM_SLUG_GBA ? ".gba" : ".nds");
    std::string extracted; u64 lastZ = 0;
    Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",fname},{},0).handle();
    bool zok = extractFirstRom(path, dir, zipRomExtsFor(slug), extracted, err,
        [&](unsigned long long done, unsigned long long total) -> bool {
            hidScanInput();
            if (hidKeysDown() & KEY_B) return false;
            if (done - lastZ < (2<<20) && done != total) return true;
            lastZ = done;
            int pct = (total>0)?(int)(done*100/total):0;
            Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",fname,std::to_string(pct)+"%"},{},0).handle();
            return true;
        }, forceName);
    if (!zok) return "";
    zipConsumed = true;
    return extracted;
}

// one RomM item: download (if needed) -> extract -> install/inject/build.
// Shared by the single RommInstall action and the batch installer. Art is
// resolved by the CALLER (GBA passes gbaArt/gbaArtEntry; NDS art resolves
// inside buildForwarderFor). interactive=false (batch phase 2) suppresses the
// per-item error/cancel dialogs and the NDS art notify; the progress dialogs
// always show. Returns {ok, cancelled}: cancelled means the user pressed B
// (batch: stop the rest of the queue). The caller shows "Installed!" and marks
// the row — installOneRomm never touches the menu.
struct InstallOutcome { bool ok=false; bool cancelled=false; };
static InstallOutcome installOneRomm(C3D_RenderTarget* target, Config* config,
                                     const MenuSelection& entry, bool needDownload,
                                     const ArtEntry& gbaArtEntry, const ArtPieces& gbaArt,
                                     bool pickArt, bool interactive,
                                     int screenOverride = -1) {
    InstallOutcome out;
    bool is3ds = (entry.platformSlug == ROMM_SLUG_3DS);
    bool isGba = (entry.platformSlug == ROMM_SLUG_GBA);
    std::string dir = rommDirFor(entry.platformSlug);
    std::string dest = dir + entry.fsName;                         // download target (nds may be .zip)
    std::string romPath = rommLocalPath(entry.fsName, entry.platformSlug); // playable file
    rlog.info("install: pre-download needDownload=" + std::string(needDownload?"1":"0") + " dest=" + dest);
    if (needDownload) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        showLoading(target, {"Downloading...", entry.fsName});
        // catch a mistaken "Install": B before the first byte cancels cleanly
        hidScanInput();
        if (hidKeysDown() & KEY_B) {
            out.cancelled = true;
            if (interactive) Dialog(target,0,0,320,240,{"Download cancelled"},{"OK"}).handle();
            return out;
        }
        u64 lastDrawn = 0;
        RommRom dlRom;
        dlRom.id = entry.rommId;
        dlRom.name = entry.title;
        dlRom.fsName = entry.fsName;
        dlRom.fileId = entry.fileId;
        dlRom.sizeBytes = entry.sizeBytes;
        dlRom.multiFile = false;
        bool ok = gRomm.download(dlRom, dest,
            [&](u64 done, u64 total) -> bool {
                hidScanInput();
                if (hidKeysDown() & KEY_B) return false; // cancel
                if (done - lastDrawn < (1<<20) && done != total) return true; // redraw each ~1MB
                lastDrawn = done;
                int pct = (total>0)?(int)(done*100/total):0;
                Dialog(target,0,0,320,240,{"Downloading... (B = cancel)",entry.fsName,humanSize(done)+" / "+humanSize(total)+" ("+std::to_string(pct)+"%)"},{},0).handle();
                return true;
            });
        if (!ok) {
            out.cancelled = (gRomm.lastError == "cancelled");
            if (interactive) {
                if (out.cancelled)
                    Dialog(target,0,0,320,240,{"Download cancelled"},{"OK"}).handle();
                else
                    Dialog(target,0,0,320,240,{"Download failed",gRomm.lastError},{"OK"}).handle();
            }
            return out;
        }
        if (!is3ds && isZipName(entry.fsName)) {   // nds/gba zip archives are extracted on-device
            Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",entry.fsName},{},0).handle();
            std::string extracted, zerr;
            u64 lastZDrawn = 0;
            // force the predicted name (fsName stem + platform ext) so
            // markers/covers/tids all agree — inner zip names are often
            // scene releases ("...(#GBA).gba") and would diverge
            std::string wantName = std::filesystem::path(romPath).filename().generic_string();
            bool zok = extractFirstRom(dest, dir, zipRomExtsFor(entry.platformSlug), extracted, zerr,
                [&](unsigned long long done, unsigned long long total) -> bool {
                    hidScanInput();
                    if (hidKeysDown() & KEY_B) return false;
                    if (done - lastZDrawn < (2<<20) && done != total) return true;
                    lastZDrawn = done;
                    int pct = (total>0)?(int)(done*100/total):0;
                    Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",entry.fsName,humanSize(done)+" / "+humanSize(total)+" ("+std::to_string(pct)+"%)"},{},0).handle();
                    return true;
                }, wantName);
            if (!zok) {
                remove(dest.c_str());
                out.cancelled = (zerr == "cancelled");
                if (interactive)
                    Dialog(target,0,0,320,240,{out.cancelled?"Extract cancelled":"Extract failed",zerr},{"OK"}).handle();
                return out;
            }
            remove(dest.c_str());
            romPath = extracted;
        }
    }
    bool installed = false;
    if (is3ds) {
        // stream-install the downloaded .cia into the title database
        std::string ierr;
        u64 lastI = 0;
        rlog.info("cia install start: " + dest);
        showLoading(target, {"Installing...", entry.fsName});
        installed = installCiaFromFile(dest, ierr, config->forceInstall,
            [&](unsigned long long done, unsigned long long total) -> bool {
                hidScanInput();
                if (hidKeysDown() & KEY_B) return false;
                if (done - lastI < (4<<20) && done != total) return true;
                lastI = done;
                int pct = (total>0)?(int)(done*100/total):0;
                Dialog(target,0,0,320,240,{"Installing... (B = cancel)",entry.fsName,std::to_string(pct)+"%"},{},0).handle();
                return true;
            });
        rlog.info(std::string("cia install ") + (installed?"OK":("FAILED: " + ierr)));
        if (installed) {
            u64 tid = ciaFileTitleId(dest);           // before we delete it
            if (tid) { installed3dsRecord(entry.rommId, tid);
                       rlog.info("recorded install tid=" + std::to_string(tid)); }
            remove(dest.c_str());                     // free the SD copy
        } else {
            out.cancelled = (ierr == "cancelled");
            if (interactive)
                Dialog(target,0,0,320,240,{out.cancelled?"Install cancelled":"Install failed",ierr},{"OK"}).handle();
        }
    } else if (isGba) {
        // VC inject: ROM baked into a native AGB_FIRM title
        std::string romBase = std::filesystem::path(romPath).filename().generic_string();
        u64 gtid = gCtr.allocateGbaTID(romBase);
        if (gtid == 0) {
            if (interactive) Dialog(target,0,0,320,240,{"No free install slots"},{"OK"}).handle();
            return out;
        }
        Dialog(target,0,0,320,240,{"Installing...",entry.title},{},0).handle();
        u64 lastG = 0;
        int mode = (screenOverride >= 0) ? screenOverride % GBA_SCREEN_COUNT
                                         : gbaScreenFor(gbaArtEntry, config);
        ReturnResult* gr = gCtr.buildGbaCIA(romPath, entry.title, gtid,
                                            gbaArt.icon48, gbaArt.bannerTex,
                                            mode,
            [&](u64 done, u64 total) -> bool {
                hidScanInput();
                if (hidKeysDown() & KEY_B) return false;
                if (done - lastG < (2<<20) && done != total) return true;
                lastG = done;
                int pct = (total>0)?(int)(done*100/total):0;
                Dialog(target,0,0,320,240,{"Installing... (B = cancel)",entry.title,std::to_string(pct)+"%"},{},0).handle();
                return true;
            });
        installed = gr->isSuccess();
        if (installed) {
            ArtEntry e2 = gbaArtEntry; e2.screen = mode;
            artStorePut(entry.fsName, e2);
        }
        else {
            out.cancelled = (gr->message == "cancelled");
            if (interactive)
                Dialog(target,0,0,320,240,{out.cancelled?"Install cancelled":"Install failed",gr->message,gLang.parseString("format_hex",(u32)gr->code)},{"OK"}).handle();
        }
        delete gr;
    } else {
        installed = buildForwarderFor(target, config, romPath, entry.title, entry.coverPath, pickArt, interactive);
        if (installed) { gFwdReady = false; invalidateManagedRoms(); }   // refresh forwarder detection
    }
    out.ok = installed;
    return out;
}

// ---- Manage helpers (single-item + batch share these) --------------------

// change art for one GBA inject in place (same TID keeps HOME slot + saves).
// Returns 1 = updated, 0 = skipped (B / nothing chosen), -1 = failed.
// interactive=true shows the per-item result dialog (single); batch passes
// false and reports in its own summary. The picker + progress always show.
// five-preset picker: vertical list with a plain-words explanation of the
// highlighted preset underneath. current = the preset baked into this game's
// install (art.json; -1 unknown/mixed) — tagged and preselected when known,
// else the Settings default. Returns the GBA_SCREEN_* index or -1 on B.
static float drawWrapped(float x, float y, float maxW, float lineH, float scale,
                         u32 color, const std::string& text, int maxLines);
// ---- vertical action menu -------------------------------------------------
// One coherent options screen for every per-item action list, on Manage and
// Browse alike: title + subtitle, options stacked vertically (the obvious
// default action FIRST), and the highlighted option explained under the
// list. A = pick (index), B = back (-1).
static int actionMenu(C3D_RenderTarget* target, const std::string& title,
                      const std::string& subtitle,
                      const std::vector<MenuOpt>& opts, int def,
                      const char* xLabel, bool* xOut) {
    int n = (int)opts.size();
    if (n <= 0) return -1;
    if (xOut) *xOut = false;
    int sel = (def >= 0 && def < n) ? def : 0;
    // squeeze rows when the list is long so the description always fits
    float pitch = (n > 5) ? 20.0f : 24.0f;
    float rowH  = pitch - 2.0f;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_UP)   sel = (sel + n - 1) % n;
        if (kd & KEY_DOWN) sel = (sel + 1) % n;
        if (kd & (KEY_A | KEY_START)) return sel;
        if (xLabel && (kd & KEY_X)) { if (xOut) *xOut = true; return sel; }
        if (kd & KEY_B) return -1;
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(target, COL_BG);
        C2D_SceneBegin(target);
        drawText(160, 16, 0.5f, 0.55f, 0, COL_TEXT, title.c_str(), C2D_AlignCenter);
        if (!subtitle.empty()) {
            std::string st = subtitle.size() > 46 ? subtitle.substr(0, 45) + "..." : subtitle;
            drawText(160, 34, 0.5f, 0.42f, 0, COL_TEXT_DIM, st.c_str(), C2D_AlignCenter);
        }
        float y = 50;
        for (int i = 0; i < n; i++, y += pitch) {
            bool hot = (i == sel);
            if (hot) C2D_DrawRectSolid(12, y, 0.4f, 296, rowH, COL_ACCENT);
            drawText(22, y + rowH / 2, 0.5f, 0.5f, 0,
                     hot ? HIGHLIGHT_FOREGROUND : COL_TEXT_DIM, opts[i].label.c_str(), 0);
        }
        C2D_DrawRectSolid(12, y + 4, 0.4f, 296, 1, COL_ELEV);
        if (!opts[sel].desc.empty()) {
            float descY = y + 12;
            int maxLines = (int)((222 - descY) / 13);
            if (maxLines < 1) maxLines = 1;
            if (maxLines > 3) maxLines = 3;
            drawWrapped(16, descY, 288, 13, 0.42f, COL_TEXT_DIM, opts[sel].desc, maxLines);
        }
        std::string hint = xLabel ? ("A select    X " + std::string(xLabel) + "    B back")
                                  : "A select    B back";
        drawText(160, 230, 0.5f, 0.4f, 0, COL_TEXT_DIM, hint.c_str(), C2D_AlignCenter);
        C3D_FrameEnd(0);
    }
    return -1;
}

static int pickGbaScreenPreset(C3D_RenderTarget* target, Config* config,
                               const std::string& title, int current = -1) {
    static const char* names[GBA_SCREEN_COUNT] = {
        "AGS-101 colors", "Original dark filter", "Unfiltered",
        "Brighter gamma", "Night (warm)"};
    static const char* descs[GBA_SCREEN_COUNT] = {
        "Gamma-corrected to match the backlit AGS-101 screen. Vivid colors without the dark cast.",
        "Nintendo's own Virtual Console filter. Authentic, but noticeably dark and muted.",
        "The raw palette, no filter at all. Brightest picture; colors look washed out.",
        "A gentler gamma correction - bright and punchy, pops like a 3DS game. Recommended.",
        "AGS-101 colors plus a warm 3400K tint - easier on the eyes in the dark."};
    if (current >= 0) current %= GBA_SCREEN_COUNT;
    int sel = -1;
    for (;;) {
        int def = config->gbaScreen % GBA_SCREEN_COUNT;
        std::vector<MenuOpt> opts;
        for (int i = 0; i < GBA_SCREEN_COUNT; i++) {
            std::string label = names[i];
            if (i == current)  label += "  (current)";
            if (i == def)      label += "  (default)";
            opts.push_back({label, descs[i]});
        }
        bool setDef = false;
        sel = actionMenu(target, "Filter", title, opts,
                         (sel >= 0) ? sel : (current >= 0) ? current : def,
                         "set default", &setDef);
        if (sel >= 0 && setDef) {
            // X: the highlighted preset becomes the Settings default for
            // every future install; stay in the menu with the tag moved
            config->gbaScreen = sel;
            config->save();
            continue;
        }
        return sel;
    }
}

// re-bakes an installed inject with the given screen preset, reusing the
// art already chosen for it (no art picker). 1 ok, -1 failed.
static int applyGbaScreenItem(C3D_RenderTarget* target, Config* config,
                              const std::string& romBase, const std::string& title,
                              const std::string& coverPath, const std::string& romPath,
                              bool interactive, int screenMode) {
    if (!ensureCtrBuilder(target)) return -1;
    ensureSgdb();
    ArtEntry ae = artStoreGet(romBase);
    ArtPieces pieces;
    if (ae.valid) artBuildFromEntry(gSgdb, gRomm, romBase, coverPath, ae, pieces);
    // ALWAYS fill gaps from the cover: a game whose art came from the cover
    // fallback has no recorded sources, and re-baking it with empty pieces
    // used to silently replace its icon/banner with the template art
    if (pieces.icon48.empty() || pieces.bannerTex.empty())
        artFromRommCover(gSgdb, gRomm, coverPath,
                         pieces.icon48.empty(), pieces.bannerTex.empty(), pieces);
    u64 gtid = gCtr.allocateGbaTID(romBase);
    if (gtid == 0) { if (interactive) Dialog(target,0,0,320,240,{"No free install slots"},{"OK"}).handle(); return -1; }
    Dialog(target,0,0,320,240,{"Applying filter...",title},{},0).handle();
    u64 lastG = 0;
    ReturnResult* gr = gCtr.buildGbaCIA(romPath, title, gtid,
                                        pieces.icon48, pieces.bannerTex,
                                        screenMode,
        [&](u64 done, u64 total) -> bool {
            hidScanInput();
            if (hidKeysDown() & KEY_B) return false;
            if (done - lastG < (2<<20) && done != total) return true;
            lastG = done;
            int pct = (total>0)?(int)(done*100/total):0;
            Dialog(target,0,0,320,240,{"Installing... (B = cancel)",title,std::to_string(pct)+"%"},{},0).handle();
            return true;
        });
    int rc = gr->isSuccess() ? 1 : -1;
    if (rc == 1) {
        ae.screen = screenMode % GBA_SCREEN_COUNT;   // remember per game
        artStorePut(romBase, ae);
    }
    rlog.info("screen apply '" + romBase + "' mode=" + std::to_string(screenMode) +
              " rc=" + std::to_string(rc) + (rc == 1 ? "" : " (" + gr->message + ")"));
    if (interactive) {
        if (rc == 1) Dialog(target,0,0,320,240,{"Filter applied!",title},{"OK"}).handle();
        else Dialog(target,0,0,320,240,{(gr->message=="cancelled")?"Cancelled":"Update failed",gr->message},{"OK"}).handle();
    }
    delete gr;
    return rc;
}

// screenOverride >= 0 bakes that preset in the same pass ("Art + screen
// filter": one re-bake instead of two); -1 keeps the game's preset.
static int changeArtGbaItem(C3D_RenderTarget* target, Config* config,
                            const std::string& romBase, const std::string& title,
                            const std::string& coverPath, const std::string& romPath,
                            bool interactive, int screenOverride = -1) {
    if (!ensureCtrBuilder(target)) return -1;
    // ask WHICH art up front — skipping an unwanted page with a well-timed B
    // was the only way before, and easy to fumble
    bool pIcon = true, pBanner = true;
    if (interactive) {
        int w = actionMenu(target, "Change which art?", title, {
            {"Both", "Pick the HOME icon, then the banner."},
            {"Icon only", "Just the 48px HOME icon; the banner stays."},
            {"Banner only", "Just the big HOME banner; the icon stays."}});
        if (w < 0) return 0;
        pIcon   = (w == 0 || w == 1);
        pBanner = (w == 0 || w == 2);
    }
    ArtEntry ae;
    ArtPieces pieces;
    resolveGbaArtInteractive(target, config, romBase, title, coverPath, ae, pieces, true,
                             pIcon, pBanner);
    if (pieces.icon48.empty() && pieces.bannerTex.empty()) return 0;   // picker cancelled
    u64 gtid = gCtr.allocateGbaTID(romBase);
    if (gtid == 0) { if (interactive) Dialog(target,0,0,320,240,{"No free install slots"},{"OK"}).handle(); return -1; }
    Dialog(target,0,0,320,240,{"Updating art...",title},{},0).handle();
    u64 lastG = 0;
    // art change keeps the game's preset unless the caller picked one
    int mode = (screenOverride >= 0) ? screenOverride % GBA_SCREEN_COUNT
                                     : gbaScreenFor(ae, config);
    ReturnResult* gr = gCtr.buildGbaCIA(romPath, title, gtid,
                                        pieces.icon48, pieces.bannerTex,
                                        mode,
        [&](u64 done, u64 total) -> bool {
            hidScanInput();
            if (hidKeysDown() & KEY_B) return false;
            if (done - lastG < (2<<20) && done != total) return true;
            lastG = done;
            int pct = (total>0)?(int)(done*100/total):0;
            Dialog(target,0,0,320,240,{"Installing... (B = cancel)",title,std::to_string(pct)+"%"},{},0).handle();
            return true;
        });
    int rc;
    if (gr->isSuccess()) {
        ae.screen = mode;
        artStorePut(romBase, ae);
        if (interactive) Dialog(target,0,0,320,240,{"Art updated!",title},{"OK"}).handle();
        rc = 1;
    } else {
        if (interactive) Dialog(target,0,0,320,240,{(gr->message=="cancelled")?"Cancelled":"Art update failed",gr->message},{"OK"}).handle();
        rc = -1;
    }
    delete gr;
    return rc;
}

// change art for one romm3ds NDS forwarder in place (same rtid). Banner page
// only — the SMDH icon stays the ROM's own DS icon. 1 / 0 / -1 like above.
static int changeArtNdsRommItem(C3D_RenderTarget* target, Config* config,
                                const std::string& name, const std::string& title,
                                const std::string& coverPath, const std::string& romPath,
                                u64 rtid, bool interactive) {
    (void)config;
    if (!ensureCtrBuilder(target)) return -1;
    ensureSgdb();
    // which art? banner is the star; the SMDH icon DEFAULTS to the ROM's own
    // DS icon (every NDS game ships one) — a custom icon is pure cosmetics
    bool pIcon = false, pBanner = true;
    if (interactive) {
        int w = actionMenu(target, "Change which art?", title, {
            {"Banner only", "The big HOME banner. The icon stays the ROM's own DS icon - every NDS game ships one."},
            {"Icon + banner", "Pick a custom HOME icon too. Cosmetic only - the DS icon is the classic default."},
            {"Icon only", "Just the 48px HOME icon; the banner stays."}});
        if (w < 0) return 0;
        pIcon   = (w == 1 || w == 2);
        pBanner = (w == 0 || w == 1);
    }
    ArtEntry ae = artStoreGet(name);
    if (ae.query.empty()) {
        std::vector<std::string> qs = artQueriesFor(name, title);
        ae.query = qs.empty() ? artSanitizeQuery(name) : qs[0];
    }
    ArtPieces pieces;
    bool iCh = false, bCh = false;
    artPickerRun(target, name, title, coverPath, ROMM_SLUG_NDS,
                 ae, pieces, pIcon, pBanner, &iCh, &bCh);
    if (!iCh && !bCh) return 0;                          // picker cancelled
    ae.weak = false;
    // pages not picked keep their stored art — never bake the template over
    // an existing banner, and a previously chosen icon survives a banner-only
    // change ("" icon = the DS icon, the default)
    if (pieces.bannerTex.empty() || pieces.icon48.empty()) {
        ArtPieces re;
        artBuildFromEntry(gSgdb, gRomm, name, coverPath, ae, re);
        if (pieces.bannerTex.empty()) pieces.bannerTex = re.bannerTex;
        if (pieces.icon48.empty())    pieces.icon48    = re.icon48;
    }
    Dialog(target,0,0,320,240,{"Fetching sound...",title},{},0).handle();
    std::string gameCwav = fetchGameSound(gRomm, romPath);
    Dialog(target,0,0,320,240,{"Updating art...",title},{},0).handle();
    ReturnResult* r = gCtr.buildCIA(romPath, title, rtid, pieces.bannerTex, gameCwav, pieces.icon48);
    int rc;
    if (r->isSuccess()) {
        artStorePut(name, ae);
        if (interactive) Dialog(target,0,0,320,240,{"Art updated!",title},{"OK"}).handle();
        rc = 1;
    } else {
        if (interactive) Dialog(target,0,0,320,240,{"Art update failed",r->message},{"OK"}).handle();
        rc = -1;
    }
    delete r;
    return rc;
}

// uninstall one Manage item (mirrors the single-item deletion paths). Returns
// true on success; decrements config->dsiwareCount for a deleted TWL forwarder.
// a manage row counts as installed when its system's title/forwarder exists
static bool manageItemInstalled(const std::string& slug, const MenuSelection& it) {
    if (slug == ROMM_SLUG_GBA) return it.installed;
    if (slug == ROMM_SLUG_NDS) return it.rtid || it.installed || it.ytid;
    return true;   // the 3DS tab only lists installed titles
}

// One installed-GBA menu, shared by RomM / Browse / Manage so they can't
// drift. Top level is [Uninstall, Art & filter >, Reinstall] (destructive
// first, art/filter grouped behind a submenu, reinstall last); the submenu
// returns the specific art/filter action. The caller dispatches the returned
// action its own way (uninstall/reinstall differ per source).
enum GbaChoice { GBA_NONE=-1, GBA_UNINSTALL, GBA_CHG_ART, GBA_FILTER, GBA_ART_FILTER, GBA_REINSTALL };
static GbaChoice gbaInstalledMenu(C3D_RenderTarget* target, const std::string& title,
                                  bool hasReinstall) {
    std::vector<MenuOpt> mo = {
        {"Uninstall", "Uninstall and delete the game file."},
        {"Art & filter", "Change the icon, banner, or the color filter."}};
    if (hasReinstall) mo.push_back({"Reinstall", "Install again with the art and filter it already uses."});
    int c = actionMenu(target, title, "Installed", mo);
    if (c < 0) return GBA_NONE;
    if (c == 0) return GBA_UNINSTALL;
    if (c == 2) return GBA_REINSTALL;   // only present when hasReinstall
    int s = actionMenu(target, title, "Art & filter", {
        {"Change art", "Pick a new HOME icon and/or banner; the save is kept."},
        {"Filter", "Change the color filter; art and save kept."},
        {"Art + filter", "Pick the filter, then the art."}});
    if (s < 0) return GBA_NONE;
    return (s==0) ? GBA_CHG_ART : (s==1) ? GBA_FILTER : GBA_ART_FILTER;
}

static bool uninstallManageItem(Config* config, const MenuSelection& it) {
    if (it.platformSlug == ROMM_SLUG_3DS) {
        if (it.protectedTitle) return false;   // this app / a system title
        // its updates/DLC would stay behind as orphans — take them too.
        // allowEnumerate: in a batch the previous item invalidated the cache
        TitleExtras ex = findTitleExtras(it.tid, true);
        Result dr = AM_DeleteTitle(MEDIATYPE_SD, it.tid);
        AM_DeleteTicket(it.tid);
        if (R_SUCCEEDED(dr)) {
            for (u64 xt : ex.tids) {
                AM_DeleteTitle(MEDIATYPE_SD, xt);
                AM_DeleteTicket(xt);
            }
            installedTitlesInvalidate();
        }
        return R_SUCCEEDED(dr);
    }
    if (it.platformSlug == ROMM_SLUG_GBA) {
        if (it.installed) {   // "Delete ROMs" rows have no inject to remove
            Result dr = AM_DeleteTitle(MEDIATYPE_SD, it.tid);
            AM_DeleteTicket(it.tid);
            if (R_FAILED(dr)) return false;
        }
        std::error_code ec;
        std::filesystem::remove(it.path, ec);          // single-pass: inject + ROM
        return true;
    }
    // NDS: remove every forwarder type present, then the ROM file
    bool err = false;
    if (it.installed && it.tid != 0) {
        if (R_FAILED(deleteForwarder(it.tid))) err = true;
        else if (config->dsiwareCount > 0) config->dsiwareCount--;
    }
    if (it.ytid != 0 && R_FAILED(deleteYanbfForwarder(it.ytid))) err = true;
    if (it.rtid != 0 && R_FAILED(deleteRommCtrForwarder(it.rtid))) err = true;
    if (!err) {
        std::error_code ec;
        if (!std::filesystem::remove(it.path, ec)) err = true;
    }
    return !err;
}

// granular uninstall rows for an installed 3DS title — shared by Manage and
// the library browse (an installed game's A-menu offers the same actions in
// both places). what codes: 0 = game + extras, 1 = update + DLC only,
// 2 = update only, 3 = DLC only.
static void addUninstall3dsOpts(std::vector<MenuOpt>& opts, std::vector<int>& what,
                                u64 gameBytes, const TitleExtras& ex) {
    if (ex.empty()) {
        opts.push_back({"Uninstall", "Remove the game. Frees " + humanSize(gameBytes) + "."});
        what.push_back(0);
        return;
    }
    opts.push_back({"Uninstall", "Remove the game AND its " + std::to_string(ex.updates + ex.dlc) +
                    " update/DLC. Frees " + humanSize(gameBytes + ex.bytes) + "."});
    what.push_back(0);
    if (ex.updates && ex.dlc) {
        opts.push_back({"Remove update + DLC", "Keep the game; delete its update and DLC. Frees " +
                        humanSize(ex.bytes) + "."});
        what.push_back(1);
    }
    if (ex.updates) {
        opts.push_back({ex.dlc ? "Remove update only" : "Remove update",
                        std::string("Keep the game") + (ex.dlc ? " and DLC" : "") +
                        "; delete the update (back to v1.0). Frees " + humanSize(ex.updateBytes) + "."});
        what.push_back(2);
    }
    if (ex.dlc) {
        opts.push_back({ex.updates ? "Remove DLC only" : "Remove DLC",
                        std::string("Keep the game") + (ex.updates ? " and update" : "") +
                        "; delete the DLC. Frees " + humanSize(ex.dlcBytes) + "."});
        what.push_back(3);
    }
}

// confirm + delete for a what code from addUninstall3dsOpts. Returns true
// when anything was deleted, so the caller refreshes its list/markers.
static bool execUninstall3ds(C3D_RenderTarget* target, const std::string& n3,
                             u64 tid, u64 gameBytes, const TitleExtras& ex, int what) {
    bool delGame = (what == 0);
    std::vector<u64> extraDel;
    u64 freed = gameBytes;
    std::string confirmTitle = "Uninstall game?";
    switch (what) {
        case 0: extraDel = ex.tids;       freed = gameBytes + ex.bytes; break;
        case 1: extraDel = ex.tids;       freed = ex.bytes;       confirmTitle = "Remove update + DLC?"; break;
        case 2: extraDel = ex.updateTids; freed = ex.updateBytes; confirmTitle = "Remove the update?"; break;
        case 3: extraDel = ex.dlcTids;    freed = ex.dlcBytes;    confirmTitle = "Remove the DLC?"; break;
    }
    if (Dialog(target,0,0,320,240,
               {confirmTitle, n3, "Frees " + humanSize(freed) +
                (delGame ? "" : " - the game stays installed")},
               {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0) return false;
    showLoading(target, {delGame ? "Uninstalling..." : "Removing...", n3});
    Result dr = 0;
    if (delGame) {
        dr = AM_DeleteTitle(MEDIATYPE_SD, tid);
        AM_DeleteTicket(tid);
    }
    int extrasGone = 0;
    if (R_SUCCEEDED(dr)) {
        for (u64 xt : extraDel) {
            if (R_SUCCEEDED(AM_DeleteTitle(MEDIATYPE_SD, xt))) extrasGone++;
            AM_DeleteTicket(xt);
        }
    }
    installedTitlesInvalidate();
    installed3dsRefresh();   // library markers read the AM set
    if (R_FAILED(dr)) {
        Dialog(target,0,0,320,240,{"Uninstall failed",n3},{"OK"}).handle();
        return false;
    }
    if (!delGame)
        Dialog(target,0,0,320,240,{"Removed.",n3,
               humanSize(freed)+" freed - the game stays installed."},{"OK"}).handle();
    else
        Dialog(target,0,0,320,240,{"Uninstalled.",n3,
               extrasGone > 0 ? "Update/DLC went with it." : ""},{"OK"}).handle();
    return true;
}

RommClient gRomm;

// selected-game cover (shared by top rail + tick loader). gCover BORROWS the
// texture from gCoverLru below — only eviction frees it.
static C2D_Image gCover = {nullptr, nullptr};
// small texture LRU: revisiting a cover while scrolling must cost nothing
// (RESPONSIVENESS-PLAN: cap resident art, never re-read the SD for it)
struct CoverSlot { int id; C2D_Image img; };
static std::vector<CoverSlot> gCoverLru;   // front = most recent, owns textures
#define COVER_LRU_CAP 16
// the selected 3DS title's own HOME icon (from its SMDH) — art for games that
// aren't in the RomM library
static C2D_Image gTitleIcon = {nullptr, nullptr};
static u64 gTitleIconTid = 0;
static u64 gIconWantTid = 0;
static int gIconDebounce = 0;
static int gCoverForId = -1;    // rommId currently in gCover
static int gCoverFailedId = -1; // rommId that failed to load (don't retry)
static int gCoverWantId = -1;
static int gCoverDebounce = 0;
// which installed title's own icon/banner represents a Manage row (0 = none).
// NDS rows prefer our CTR forwarder, then YANBF (both SD titles with an
// SMDH); a bare TWL forwarder has no SMDH to read.
static u64 manageIconTid(const MenuSelection* s) {
    if (s->platformSlug == ROMM_SLUG_3DS) return s->tid;
    if (s->platformSlug == ROMM_SLUG_GBA) return s->installed ? s->tid : 0;
    if (s->rtid) return s->rtid;
    if (s->ytid) return s->ytid;
    return 0;
}
// baked-banner preview slot (GBA injects write banners-cache/<tid>.raw)
static C2D_Image gBannerPrev = {nullptr, nullptr};
static u64 gBannerPrevTid = 0;   // tid the texture belongs to (0 = none)
static u64 gBannerWantTid = 0;

// frames a selection must sit still before any art touches the SD card:
// stepping through a list stays pure text (the NDS-manage feel everywhere).
// 15 frames (250ms) beats a fast press cadence (~6-7 presses/sec) so even
// repeated single steps never fire a load between presses.
#define ART_SETTLE_FRAMES 15
// a cover that isn't in the SD cache yet (worker still fetching): back off
// this many frames before the next stat instead of re-trying every frame
#define ART_MISS_BACKOFF 30
static u32 gTick = 0;           // global frame counter for marquees

// ---- header status cluster: clock + battery ------------------------------
// the battery is the hbmenu/ftpd sprite set (romfs:/ui/battery*.png, from
// devkitPro/3ds-hbmenu) — the familiar HOME-style glyph, charge sprite while
// charging. Level comes from ptm:u (0-5, same mapping as hbmenu), cached and
// refreshed every ~5s — never a service call per frame.
static u8 gBattLevel = 5;        // ptm:u 0-5
static bool gBattCharging = false;
static u32 gBattLastTick = 0;
static bool gBattEver = false;
static C2D_Image gBattImg[6];    // levels 0-4 + [5] charging
static C2D_SpriteSheet gBattSheet = nullptr;
static bool gBattImgTried = false;
static void ensureBatterySprites() {
    if (gBattImgTried) return;
    gBattImgTried = true;
    // hbmenu's way: the PNGs are compiled at build time (tex3ds) into a
    // pre-tiled t3x atlas, imported here with a plain memcpy — no GX
    // display transfer, so this is safe even before the first frame.
    // (runtime stb decode + SyncDisplayTransfer hung the GPU at boot.)
    gBattSheet = C2D_SpriteSheetLoad("romfs:/gfx/battery.t3x");
    if (!gBattSheet) return;
    size_t n = C2D_SpriteSheetCount(gBattSheet);
    for (size_t i = 0; i < 6 && i < n; i++)
        gBattImg[i] = C2D_SpriteSheetGetImage(gBattSheet, i);
}
static void drawHeaderStatus() {
    // sprites are loaded in tickBottom — texture uploads (GX transfer +
    // gspWaitForPPF) hang the GPU when done inside a C3D frame
    if (!gBattEver || gTick - gBattLastTick >= 300) {
        gBattEver = true;
        gBattLastTick = gTick;
        u8 lvl;
        if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl))) gBattLevel = (lvl > 5) ? 5 : lvl;
        u8 chg = 0;
        gBattCharging = R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg)) && chg;
    }
    // hbmenu's level -> sprite mapping (0 and 1 share the empty glyph)
    static const int lvlImg[6] = {0, 0, 1, 2, 3, 4};
    C2D_Image bat = gBattCharging ? gBattImg[5] : gBattImg[lvlImg[gBattLevel]];
    // right cluster, all centered on the header's midline (y=13.5):
    // [time]  [battery] with real gaps — the counter sits further left
    float timeRight = 392;
    if (bat.tex) {
        C2D_DrawImageAt(bat, 400 - 27 - 4, 5, 0.35f);   // 27x18 sprite, 4px margin
        timeRight = 400 - 27 - 4 - 6;
    }
    time_t now = time(NULL);
    struct tm* lt = localtime(&now);
    char clk[8] = {0};
    if (lt) snprintf(clk, sizeof(clk), "%02d:%02d", lt->tm_hour, lt->tm_min);
    if (clk[0])
        drawText(timeRight, 13, 0.5f, 0.5f, COL_BG, COL_TEXT, clk, C2D_AlignRight);
}

// cached libraries per platform slug, for instant search filtering
static std::map<std::string, std::vector<RommRom>> gCache;   // slug -> roms
static std::map<std::string, bool> gCacheOk;                 // slug -> loaded?
static std::vector<RommRom> gCombined;                       // cross-system search source

static const char* systemName(const std::string& slug) {
    if (slug == ROMM_SLUG_3DS) return "Nintendo 3DS";
    if (slug == ROMM_SLUG_GBA) return "Game Boy Advance";
    return "Nintendo DS";
}
// --- on-SD library cache (fast open + offline search) -----------------------
static std::string libCachePath(const std::string& slug) {
    return FORWARDER_DIR + "/lib_" + slug + ".json";
}
static void saveLibCache(const std::string& slug, const std::vector<RommRom>& roms) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& r : roms)
        arr.push_back({{"id",r.id},{"name",r.name},{"fsName",r.fsName},{"fileId",r.fileId},
                       {"slug",r.platformSlug},{"inst",r.installable},{"cov",r.coverPath},
                       {"covS",r.coverSmallPath},{"sum",r.summary},{"gen",r.genres},
                       {"yr",r.year},{"rt",r.rating},{"sz",(uint64_t)r.sizeBytes},{"multi",r.multiFile},
                       {"tid",(uint64_t)r.titleId}});
    std::ofstream o(libCachePath(slug));
    o << arr.dump();
}
static bool loadLibCache(const std::string& slug, std::vector<RommRom>& out) {
    std::ifstream in(libCachePath(slug));
    if (!in.good()) return false;
    try {
        nlohmann::json arr; in >> arr;
        if (!arr.is_array() || arr.empty()) return false;
        out.clear();
        for (auto& j : arr) {
            RommRom r;
            r.id = j.value("id",0); r.name = j.value("name",std::string());
            r.fsName = j.value("fsName",std::string()); r.fileId = j.value("fileId",0);
            r.platformSlug = j.value("slug",slug); r.installable = j.value("inst",true);
            r.coverPath = j.value("cov",std::string()); r.coverSmallPath = j.value("covS",std::string());
            r.summary = j.value("sum",std::string()); r.genres = j.value("gen",std::string());
            r.year = j.value("yr",0); r.rating = j.value("rt",0.0f);
            r.sizeBytes = j.value("sz",(uint64_t)0); r.multiFile = j.value("multi",false);
            r.titleId = j.value("tid",(uint64_t)0);
            out.push_back(r);
        }
        return true;
    } catch (...) { return false; }
}

static void invalidateAllCaches() {
    gCacheOk.clear(); gCombined.clear();
    remove(libCachePath(ROMM_SLUG_NDS).c_str());
    remove(libCachePath(ROMM_SLUG_3DS).c_str());
}


// resolve 3ds title ids (for install detection) via a small header fetch, cached in the lib json.
// runs BEFORE the cover worker starts, so only the main thread touches httpc (no concurrency).
// Also settles fileId==-1 roms (RomM >= 4.9.2 list response has no file lists)
// with a detail fetch each, so the picked .cia lands in the lib json too.
static void resolveTitleIds(const std::string& slug, C3D_RenderTarget* target) {
    if (slug != ROMM_SLUG_3DS) return;
    auto& roms = gCache[slug];
    int need = 0;
    for (auto& r : roms) if (r.installable && (r.titleId == 0 || r.fileId == -1)) need++;
    if (need == 0) return;
    int done = 0, ok = 0;
    for (auto& r : roms) {
        if (!r.installable || (r.titleId != 0 && r.fileId != -1)) continue;
        done++;
        if (target) showLoading(target, {"Reading title ids...", std::to_string(done)+"/"+std::to_string(need)});
        if (r.fileId == -1 && !gRomm.resolveRomFile(r)) continue;   // network miss: retry next open
        if (!r.installable || r.titleId != 0) { ok++; continue; }   // resolved to "no .cia"
        std::string hdr;
        if (gRomm.fetchCiaHeader(r, hdr)) { r.titleId = ciaBufferTitleId(hdr); if (r.titleId) ok++; }
    }
    rlog.info(" resolved title ids " + std::to_string(ok) + "/" + std::to_string(need));
    if (ok) saveLibCache(slug, roms);
}

// settle a fileId==-1 3DS row at install time (library was opened offline):
// detail-fetch the .cia pick, update the row + the lib cache. Returns false
// on network error (row untouched); on success e.installable says whether
// the folder actually holds a .cia.
static bool settle3dsFilePick(MenuSelection& e) {
    if (e.platformSlug != ROMM_SLUG_3DS || e.fileId != -1) return true;
    RommRom rr;
    rr.id = e.rommId;
    rr.fileId = -1;
    rr.sizeBytes = 0;
    if (!gRomm.resolveRomFile(rr)) return false;
    e.fsName = rr.fsName.empty() ? e.fsName : rr.fsName;
    e.fileId = rr.fileId;
    e.sizeBytes = rr.sizeBytes ? rr.sizeBytes : e.sizeBytes;
    e.installable = rr.installable;
    for (auto& cr : gCache[ROMM_SLUG_3DS]) {   // persist the pick
        if (cr.id != e.rommId) continue;
        cr.fsName = rr.fsName.empty() ? cr.fsName : rr.fsName;
        cr.fileId = rr.fileId; cr.installable = rr.installable;
        if (rr.sizeBytes) cr.sizeBytes = rr.sizeBytes;
        saveLibCache(ROMM_SLUG_3DS, gCache[ROMM_SLUG_3DS]);
        break;
    }
    return true;
}

// loads a platform's library into gCache[slug] once; returns false on error (sets gRomm.lastError)
// quiet = a screen only wants the cached names/metadata (Manage -> 3DS): no
// background refresh, no cover prefetch — those fight the SMDH/tally SD reads.
static bool ensurePlatformLoaded(const std::string& slug, C3D_RenderTarget* target = nullptr,
                                 bool quiet = false) {
    if (gCacheOk[slug]) { rlog.info(" cache hit " + slug); return true; }
    // fast path: on-SD json cache (instant, no network), then refresh the
    // list from the server in the background ("updating..." in the heading)
    if (loadLibCache(slug, gCache[slug])) {
        gCacheOk[slug] = true;
        rlog.info(" loaded " + std::to_string(gCache[slug].size()) + " roms from SD cache " + slug);
        if (quiet) return true;
        resolveTitleIds(slug, target);          // fill in any missing tids (older cache)
        libRefreshStart(gRomm, slug, gCache[slug], saveLibCache);
        // covers wait for the refresh (single httpc user); resumed on take
        if (!libRefreshRunning("")) coverCacheStart(gRomm, gCache[slug]);
        return true;
    }
    rlog.info(" findPlatform " + slug);
    int pid = gRomm.findPlatform(slug);
    rlog.info(" platform id=" + std::to_string(pid));
    if (pid < 0) { rlog.error(" findPlatform failed: " + gRomm.lastError); return false; }
    rlog.info(" listRoms...");
    if (!gRomm.listRoms(pid, gCache[slug], slug)) { rlog.error(" listRoms failed: " + gRomm.lastError); return false; }
    gCacheOk[slug] = true;
    rlog.info(" listRoms ok: " + std::to_string(gCache[slug].size()) + " roms");
    resolveTitleIds(slug, target);              // resolve tids BEFORE covers (no concurrent httpc)
    saveLibCache(slug, gCache[slug]);
    if (quiet) return true;
    coverCacheStart(gRomm, gCache[slug]);       // background art prefetch (async, cached)
    rlog.info(" cover prefetch started");
    return true;
}

// measures text width at (font-adjusted) scale
static float measureText(const std::string& s, float fscale) {
    C2D_TextBuf buf = C2D_TextBufNew(1024);
    C2D_Text t;
    C2D_Font font = getFont();
    if (font) C2D_TextFontParse(&t, font, buf, s.c_str());
    else C2D_TextParse(&t, buf, s.c_str());
    float w = 0;
    C2D_TextGetDimensions(&t, fscale, fscale, &w, NULL);
    C2D_TextBufDelete(buf);
    return w;
}

// rotating char-window for text wider than maxW ("marquee").
// phase: frames since the marquee (re)started; dwells at the start,
// scrolls one char per 8 frames, then wraps with a gap.
static std::string tickerWindow(const std::string& s, float maxW, float fscale, u32 phase) {
    float w = measureText(s, fscale);
    if (w <= maxW || s.size() < 4) return s;
    size_t vis = (size_t)(s.size() * maxW / w);
    if (vis < 4) vis = 4;
    const u32 dwell = 120; // ~2s at 60fps before scrolling starts
    std::string loop = s + "     ";
    size_t off = (phase < dwell) ? 0 : (((phase - dwell) / 8) % loop.size());
    std::string window;
    for (size_t i = 0; i < vis; i++)
        window += loop[(off + i) % loop.size()];
    return window;
}

// truncate with ellipsis to fit maxW (single measure approximation)
static std::string fitEllipsis(const std::string& s, float maxW, float fscale) {
    float w = measureText(s, fscale);
    if (w <= maxW) return s;
    size_t vis = (size_t)(s.size() * maxW / w);
    if (vis < 4) vis = 4;
    return s.substr(0, vis - 3) + "...";
}

//class MenuSelection {

    MenuSelection::MenuSelection(std::string s,std::filesystem::path p) {
        this->display=s;
        this->path=p;
    }
    MenuSelection* MenuSelection::setPath(std::filesystem::path p) {
        this->path=p;
        return this;
    }
    MenuSelection* MenuSelection::setDisplay(std::string s) {
        this->display=s;
        return this;
    }


    Menu::~Menu() {
        for ( auto item : this->entries ) delete item;
        this->entries.clear();
    }
    Menu::Menu() {

    }
    Menu::Menu(std::vector<MenuSelection*> entries) {
        this->entries=entries;
    }
    Menu* Menu::addEntry(MenuSelection* s) {
        this->entries.push_back(s);
        return this;
    }
    // Browse SD Card walks the SD folders, which also hold everything
    // downloaded from RomM — so installed files are hidden by default and X
    // reveals them (needed for Reinstall / Change art).
    static bool gLocalShowInstalled = false;
    static int  gLocalHidden = 0;        // installed files the filter left out
    // Browse SD Card: a real folder browser. Starts at the roms folder and
    // walks up to the SD root (and into any sibling like sdmc:/cias). The
    // current folder persists across list rebuilds (X toggle / post-install).
    static const std::string BROWSE_ROOT = "sdmc:/roms";
    static std::string gBrowseDir;       // "" until the browser is first opened
    static std::string browseParent(std::string p);   // defined with generateLocalMenu

    void Menu::drawMenu() {
            gTick++;
            // restart the selected-row marquee whenever selection changes
            static const void* lastSel = nullptr;
            static u32 selTick = 0;
            const void* selPtr = this->entries.empty() ? nullptr : (const void*)*this->selection;
            if (selPtr != lastSel) { lastSel = selPtr; selTick = gTick; }

            std::string title = this->heading.empty() ? shorten(this->currentDirectory.generic_string(),30) : shorten(this->heading,34);
            if (this->type == MENU_ROMM && libRefreshRunning(this->crossSystem ? "" : this->platformSlug))
                title += "  ~ updating...";
            // the marked-count lives in the right-aligned header counter only
            // (same as RomM / Manage) — never appended to the title, which then
            // collided with that counter on the Browse screen
            // flat background + header. Everything in the bar sits on its
            // vertical midline: title left, position counter, then the
            // clock + battery cluster at the right edge (with real gaps).
            C2D_DrawRectSolid(0, 0, 0, 400, 240, COL_BG);
            drawText(12, 13, 0.5f, 0.5f, COL_BG, COL_TEXT_DIM, title.c_str(), 0);
            drawHeaderStatus();
            if (!this->entries.empty() && this->type != MENU_MAIN && this->type != MENU_SETTINGS && this->type != MENU_SERVER) {
                int nsel = this->selectedCount();
                char pos[24];
                if (nsel > 0) {          // multiselect: show the count in the accent color
                    snprintf(pos, sizeof(pos), "%d selected", nsel);
                    drawText(312, 13, 0.5f, 0.5f, COL_BG, COL_ACCENT, pos, C2D_AlignRight);
                } else {
                    snprintf(pos, sizeof(pos), "%d/%d",
                             (int)(this->selection - this->entries.begin()) + 1,
                             (int)this->entries.size());
                    drawText(312, 13, 0.5f, 0.5f, COL_BG, COL_TEXT_DIM, pos, C2D_AlignRight);
                }
            }
            C2D_DrawRectSolid(0, MENU_HEADING_HEIGHT-1, 0.1f, 400, 1, COL_ELEV);

            // library + manage both use the box-art rail (same design language)
            bool railed = (this->type == MENU_ROMM) || (this->type == MENU_MANAGE);
            float rowW = railed ? 282 : 400;
            float textX = railed ? 30 : 16;
            float textMax = rowW - textX - 14;
            u16 offset = 0;
            u8 counter=0;
            for (std::vector<MenuSelection*>::iterator entry=this->top;entry!=this->entries.end() && counter < MAX_ENTRY_COUNT;entry++) {
                bool isSel = (entry == this->selection);
                float ry = MENU_HEADING_HEIGHT + offset;
                if (isSel) {
                    C2D_DrawRectSolid(0, ry, 0.2f, rowW, ENTRY_HEIGHT, COL_SURFACE);
                    C2D_DrawRectSolid(0, ry, 0.3f, 4, ENTRY_HEIGHT, COL_ACCENT);
                }
                float scale = getFontScale(0.55);
                std::string body = (*entry)->display;
                // marked rows show ONLY the accent checkbox glyph (drawn below),
                // like RomM/Manage — no redundant "[x] " text prefix
                // RomM rows: fixed on-SD dot outside the scrolling text
                if (railed && body.size() >= 2) {
                    if (body[0] == '*')
                        C2D_DrawRectSolid(15, ry + ENTRY_HEIGHT/2 - 3, 0.4f, 6, 6, COL_ACCENT);
                    body = body.substr(2);
                }
                // batch multiselect mark: bright-bordered accent checkbox in the
                // left gutter (sits left of the on-SD dot; only when selected)
                if ((*entry)->selected) {
                    float bw = 10, bx = 4, by = ry + ENTRY_HEIGHT/2 - bw/2;
                    C2DExtra_DrawRectHollow(bx, by, 0.4f, bw, bw, 2, COL_TEXT);
                    C2D_DrawRectSolid(bx+3, by+3, 0.4f, bw-6, bw-6, COL_ACCENT);
                }
                std::string rowText = isSel
                    ? tickerWindow(body, textMax, scale, gTick - selTick)
                    : fitEllipsis(body, textMax, scale);
                C2D_TextBuf buf = C2D_TextBufNew(4096);
                C2D_Text text;
                C2D_Font font = getFont();
                if (font) {
                    C2D_TextFontParse(&text, font, buf, rowText.c_str());
                } else {
                    C2D_TextParse(&text,buf,rowText.c_str());
                }
                float textheight=0;
                C2D_TextGetDimensions(&text,scale,scale,NULL,&textheight);
                C2D_TextOptimize(&text);
                // the duplicates row is a cleanup action, not a game: tint it
                u32 rowCol = ((*entry)->action == CleanupCias) ? COL_ACCENT
                                                               : (isSel ? COL_TEXT : COL_TEXT_DIM);
                C2D_DrawText(&text, C2D_WithColor,textX,ry+(ENTRY_HEIGHT/2)-(textheight/2),0.4f,scale,scale,rowCol);
                C2D_TextBufDelete(buf);
                offset+=ENTRY_HEIGHT;
                counter++;
            }
            // scrollbar when the list overflows: minimal accent thumb
            if (this->entries.size() > MAX_ENTRY_COUNT) {
                float barX = rowW - 3;
                float trackY = MENU_HEADING_HEIGHT + 4, trackH = 240 - trackY - 4;
                float frac = (float)MAX_ENTRY_COUNT / this->entries.size();
                float thumbH = trackH * frac;
                if (thumbH < 16) thumbH = 16;
                float topIdx = (float)(this->top - this->entries.begin());
                float maxTop = (float)(this->entries.size() - MAX_ENTRY_COUNT);
                float thumbY = trackY + (maxTop > 0 ? (topIdx / maxTop) * (trackH - thumbH) : 0);
                C2D_DrawRectSolid(barX, thumbY, 0.61f, 3, thumbH, COL_ACCENT);
            }
            // box art rail: flat surface, sharp art, info stack
            if (railed && !this->entries.empty()) {
                MenuSelection* sel = *this->selection;
                float railX = rowW, railW = 400 - rowW;
                C2D_DrawRectSolid(railX, MENU_HEADING_HEIGHT, 0, railW, 240-MENU_HEADING_HEIGHT, COL_SURFACE);
                C2D_DrawRectSolid(railX, MENU_HEADING_HEIGHT, 0.55f, 1, 240-MENU_HEADING_HEIGHT, COL_ELEV);
                float cx = railX + railW / 2;
                float iy = MENU_HEADING_HEIGHT + 120; // fallback when no art
                if (gCoverForId == sel->rommId && gCover.tex) {
                    // full-bleed: cover spans the rail edge-to-edge
                    float cw = gCover.subtex->width, ch = gCover.subtex->height;
                    C2D_DrawImageAt(gCover, railX + (railW - cw) / 2, MENU_HEADING_HEIGHT, 0.56f, NULL, 1.0f, 1.0f);
                    iy = MENU_HEADING_HEIGHT + ch + 6;
                } else if (gTitleIconTid && this->type == MENU_MANAGE &&
                           gTitleIconTid == manageIconTid(sel) && gTitleIcon.tex) {
                    // no RomM cover: the title's own icon on a HOME-style plate
                    float d = 96, pad = 8, r = 12;
                    float px = cx - d/2 - pad, py = MENU_HEADING_HEIGHT + 24 - pad;
                    float pw = d + pad*2, ph = d + pad*2;
                    u32 plate = C2D_Color32(0xF4, 0xF6, 0xFA, 0xFF);
                    C2D_DrawRectSolid(px + r, py, 0.56f, pw - r*2, ph, plate);
                    C2D_DrawRectSolid(px, py + r, 0.56f, pw, ph - r*2, plate);
                    C2D_DrawCircleSolid(px + r, py + r, 0.56f, r, plate);
                    C2D_DrawCircleSolid(px + pw - r, py + r, 0.56f, r, plate);
                    C2D_DrawCircleSolid(px + r, py + ph - r, 0.56f, r, plate);
                    C2D_DrawCircleSolid(px + pw - r, py + ph - r, 0.56f, r, plate);
                    C2D_DrawImageAt(gTitleIcon, cx - d/2, MENU_HEADING_HEIGHT + 24, 0.57f, NULL, 2.0f, 2.0f);
                    iy = py + ph + 8;
                } else {
                    const char* ph;
                    u64 mtid = (this->type == MENU_MANAGE) ? manageIconTid(sel) : 0;
                    if (mtid) {
                        // icon rows: "..." until the debounced load actually
                        // ran and came back empty (matches the other systems)
                        ph = (gTitleIconTid == mtid && !gTitleIcon.tex) ? "no art" : "...";
                    } else {
                        ph = (gCoverFailedId == sel->rommId ||
                              (sel->coverPath.empty() && sel->coverSmallPath.empty())) ? "no art" : "...";
                    }
                    drawText(cx, MENU_HEADING_HEIGHT + 72, 0.56f, 0.45f, COL_SURFACE, COL_TEXT_DIM, ph, C2D_AlignCenter);
                }
                // condensed info under the art
                char line[48];
                if (sel->year > 0)
                    snprintf(line, sizeof(line), "%d - %s", sel->year, humanSize(sel->sizeBytes).c_str());
                else
                    snprintf(line, sizeof(line), "%s", humanSize(sel->sizeBytes).c_str());
                drawText(cx, iy, 0.56f, 0.42f, COL_SURFACE, COL_TEXT_DIM, line, C2D_AlignCenter);
                if (sel->display.size() && sel->display[0] == '*')
                    drawText(cx, iy + 13, 0.56f, 0.42f, COL_SURFACE, COL_ACCENT,
                             (this->type == MENU_MANAGE) ? "installed" : "on SD", C2D_AlignCenter);
            }
    }
    // ---- bottom-screen details panel ------------------------------------

    #define COVER_CACHE_DIR (FORWARDER_DIR + std::string("/cache/"))

    // storage breakdown for the Manage system picker: AM enumeration is too
    // slow to redo every frame, so it's computed once in generateManageSystemMenu
    static StorageTally gManageTally;
    static bool gManageTallyStale = false;   // async recompute in flight
    static bool gManageTallyEver = false;    // any numbers shown yet this run

    // description scroll state
    static int gDescForId = -1;
    static std::vector<std::string> gDescLines;
    static int gDescScroll = 0;
    static u32 gGenreTick = 0; // when the genre marquee last (re)started

    // draws word-wrapped text with y as the TOP of the first line; returns
    // the y just below the last line. (drawText centers on y, so we offset.)
    static float drawWrapped(float x, float y, float maxW, float lineH, float scale,
                             u32 color, const std::string& text, int maxLines) {
        float fscale = getFontScale(scale);
        float half = lineH * 0.5f;
        std::string word, line;
        std::stringstream ss(text);
        int lines = 0;
        while (ss >> word && lines < maxLines) {
            std::string test = line.empty() ? word : line + " " + word;
            if (measureText(test, fscale) > maxW && !line.empty()) {
                bool last = (lines == maxLines - 1);
                drawText(x, y + half, 0.55f, scale, 0, color, (last ? line + "..." : line).c_str(), 0);
                y += lineH;
                lines++;
                line = word;
            } else {
                line = test;
            }
        }
        if (!line.empty() && lines < maxLines) {
            drawText(x, y + half, 0.55f, scale, 0, color, line.c_str(), 0);
            y += lineH;
        }
        return y;
    }
    // single line of text with y as its TOP
    static void drawLineTop(float x, float y, float lineH, float scale, u32 color, const char* s) {
        drawText(x, y + lineH * 0.5f, 0.55f, scale, 0, color, s, 0);
    }

    void Menu::tickBottom() {
        ensureBatterySprites();   // texture upload — must run OUTSIDE the C3D frame
        if ((this->type != MENU_ROMM && this->type != MENU_MANAGE) || this->entries.empty()) return;
        MenuSelection* sel = *this->selection;
        if (sel->action != RommInstall && sel->action != ManageRom &&
            sel->action != ManageZip) return;
        // Manage rows showing the installed title's own art: HOME icon on the
        // rail + baked-banner preview in the details card. Debounced like
        // covers — an SD read on every step is what made lists crawl.
        u64 mtid = (this->type == MENU_MANAGE) ? manageIconTid(sel) : 0;
        if (mtid && sel->rommId <= 0) {
            if (mtid != gIconWantTid) { gIconWantTid = mtid; gIconDebounce = 0; return; }
            if (++gIconDebounce < ART_SETTLE_FRAMES) return;
            if (gTitleIconTid != mtid) {
                if (gTitleIcon.tex) freeTexImage(&gTitleIcon);
                gTitleIconTid = mtid;
                std::string rgba = titleIconRGBA(mtid);   // RAM-cached after first read
                if (rgba.size() == 48*48*4 &&
                    texFromRGBA((const unsigned char*)rgba.data(), 48, 48, &gTitleIcon))
                    C3D_TexSetFilter(gTitleIcon.tex, GPU_NEAREST, GPU_NEAREST);   // pixel art
                return;   // banner next settle frame: one SD read per frame max
            }
            if (gBannerPrevTid != mtid && gBannerWantTid != mtid) {
                gBannerWantTid = mtid;   // one attempt per selection
                char bp[96];
                snprintf(bp, sizeof(bp), "%s/banners-cache/%016llX.raw",
                         FORWARDER_DIR.c_str(), (unsigned long long)mtid);
                // no cached preview yet: pull it from the installed title's
                // own banner (works for every existing inject/forwarder)
                std::string raw = ensureBannerPreview(mtid) ? readEntireFile(bp) : std::string();
                if (raw.size() == 256*128*4) {
                    if (gBannerPrev.tex) freeTexImage(&gBannerPrev);
                    if (texFromRGBA((const unsigned char*)raw.data(), 256, 128, &gBannerPrev))
                        gBannerPrevTid = mtid;
                }
            }
            return;
        }
        if (sel->rommId <= 0) return;
        if (sel->rommId != gCoverWantId) {
            gCoverWantId = sel->rommId;
            gCoverDebounce = 0;
            return;
        }
        if (gCoverForId == gCoverWantId || gCoverFailedId == gCoverWantId) return;
        // RAM LRU first: scrolling back to a seen cover never re-reads the SD
        for (size_t i = 0; i < gCoverLru.size(); i++) {
            if (gCoverLru[i].id != gCoverWantId) continue;
            CoverSlot s = gCoverLru[i];
            gCoverLru.erase(gCoverLru.begin() + i);
            gCoverLru.insert(gCoverLru.begin(), s);
            gCover = s.img;
            gCoverForId = s.id;
            return;
        }
        if (++gCoverDebounce < ART_SETTLE_FRAMES) return; // rapid steps: no SD at all
        // never fetch/decode here: the worker prefetches, we just upload
        coverCacheWant(gCoverWantId);
        C2D_Image img = {nullptr, nullptr};
        if (coverCacheLoad(gCoverWantId, &img)) {
            gCoverLru.insert(gCoverLru.begin(), {gCoverWantId, img});
            while (gCoverLru.size() > COVER_LRU_CAP) {
                freeTexImage(&gCoverLru.back().img);
                gCoverLru.pop_back();
            }
            gCover = img;
            gCoverForId = gCoverWantId;
        } else if (coverCacheUnavailable(gCoverWantId)) {
            gCoverFailedId = gCoverWantId;
        } else {
            // still downloading: don't stat the SD again every frame
            gCoverDebounce = ART_SETTLE_FRAMES - ART_MISS_BACKOFF;
        }
    }

    // splits text into wrapped lines for the given width
    static void wrapLines(const std::string& text, float maxW, float scale,
                          std::vector<std::string>& out) {
        out.clear();
        float fscale = getFontScale(scale);
        std::string word, line;
        std::stringstream ss(text);
        while (ss >> word) {
            std::string test = line.empty() ? word : line + " " + word;
            if (measureText(test, fscale) > maxW && !line.empty()) {
                out.push_back(line);
                line = word;
            } else {
                line = test;
            }
        }
        if (!line.empty()) out.push_back(line);
    }

    void Menu::scrollDesc(int dir) {
        gDescScroll += dir;
        if (gDescScroll < 0) gDescScroll = 0;
        // upper clamp happens in drawBottom (needs line count)
    }

    void Menu::toggleMark() {
        // Y marks/unmarks a game for a batch on the Install-from-SD screen;
        // everywhere else Y still scrolls the details panel
        if (this->type != MENU_LOCAL) { this->scrollDesc(-1); return; }
        if (this->entries.empty()) return;
        MenuSelection* sel = *this->selection;
        if (sel->action == LocalInstall) sel->selected = !sel->selected;
    }

    // small flat metadata chip with y as its TOP; returns x after the chip
    #define CHIP_H 16.0f
    // short region tag from the SMDH region lockout (nullptr = unknown)
    static const char* regionChipText(u32 r) {
        if (!r) return nullptr;
        if (r == 0x7FFFFFFF) return "World";
        int n = 0;
        for (u32 b = r & 0x7F; b; b >>= 1) n += (int)(b & 1);
        if (n >= 3) return "World";
        if ((r & 2) && (r & 4)) return "USA/EUR";
        if (r & 2)  return "USA";
        if (r & 4)  return "EUR";
        if (r & 1)  return "JPN";
        if (r & 8)  return "AUS";
        if (r & 16) return "CHN";
        if (r & 32) return "KOR";
        if (r & 64) return "TWN";
        return nullptr;
    }

    static float drawChip(float x, float y, const std::string& label, u32 fg) {
        float fscale = getFontScale(0.42f);
        float w = measureText(label, fscale) + 14;
        C2D_DrawRectSolid(x, y, 0.5f, w, CHIP_H, COL_ELEV);
        drawText(x + w/2, y + CHIP_H/2, 0.55f, 0.42f, 0, fg, label.c_str(), C2D_AlignCenter);
        return x + w + 6;
    }

    // one main content card for every bottom screen: bg, card, action bar.
    // ALL text lives inside the card between CARD_X..CARD_X+CARD_W.
    #define CARD_X 8.0f
    #define CARD_Y 8.0f
    #define CARD_W 304.0f
    #define PAD    8.0f
    #define CTX    (CARD_X + PAD)   // content x
    #define CTW    (CARD_W - 2*PAD) // content width

    #define BAR_Y 218.0f            // taller action bar so text clears the edge
    static void drawBottomFrame(const char* hint) {
        C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_BG);
        C2D_DrawRectSolid(CARD_X, CARD_Y, 0.2f, CARD_W, BAR_Y - CARD_Y, COL_SURFACE);
        C2D_DrawRectSolid(0, BAR_Y, 0.5f, 320, 240 - BAR_Y, COL_ELEV);
        drawText(160, BAR_Y + (240 - BAR_Y) / 2, 0.56f, 0.42f, 0, COL_TEXT_DIM, hint, C2D_AlignCenter);
    }

    static float cardDivider(float y) {
        C2D_DrawRectSolid(CARD_X, y, 0.4f, CARD_W, 1, COL_ELEV);
        return y + 1;
    }

    void Menu::drawBottom(Config* config) {
        if (this->type == MENU_ROMM) {
            if (this->entries.empty()) {
                drawBottomFrame("B Back");
                drawText(160, 110, 0.55f, 0.5f, COL_SURFACE, COL_TEXT_DIM, "No games match.", C2D_AlignCenter);
                return;
            }
            MenuSelection* sel = *this->selection;
            bool is3ds = (sel->platformSlug == ROMM_SLUG_3DS);
            // installed state per SELECTION, not per frame: the GBA lookup
            // walks paths + the tid-owner map and 60Hz of that dragged browse
            static bool gOnSd = false;
            if (gDescForId != sel->rommId) {
                gOnSd = is3ds ? installed3dsHasTitle(sel->titleId)
                      : sel->platformSlug == ROMM_SLUG_GBA
                          ? installed3dsHasTitle(gbaTidForRom(std::filesystem::path(rommLocalPath(sel->fsName, sel->platformSlug)).filename().generic_string()))
                          : ndsForwarderInstalled(sel->fsName);
                wrapLines(sel->summary, CTW, 0.45f, gDescLines);
                gDescForId = sel->rommId;
                gDescScroll = 0;
                gGenreTick = gTick; // restart genre marquee for the new game
            }
            bool onSD = gOnSd;
            drawBottomFrame(""); // hint drawn last
            float y = CARD_Y + PAD;
            // title
            y = drawWrapped(CTX, y, CTW, 16, 0.58f, COL_TEXT, sel->title, 2);
            y += 6;
            // chips
            float cxp = CTX;
            char chip[24];
            if (sel->year > 0) { snprintf(chip, sizeof(chip), "%d", sel->year); cxp = drawChip(cxp, y, chip, COL_TEXT_DIM); }
            cxp = drawChip(cxp, y, humanSize(sel->sizeBytes), COL_TEXT_DIM);
            if (sel->rating > 0) { snprintf(chip, sizeof(chip), "%.0f/100", sel->rating); cxp = drawChip(cxp, y, chip, COL_TEXT_DIM); }
            if (onSD) drawChip(cxp, y, "INSTALLED", COL_ACCENT);
            y += CHIP_H + 6;
            y = cardDivider(y);
            // genres: 4px pad, dwell then scroll after ~3s
            if (!sel->genres.empty()) {
                y += 4;
                float fscale = getFontScale(0.42f);
                std::string g = tickerWindow(sel->genres, CTW, fscale, gTick - gGenreTick);
                drawLineTop(CTX, y, 13, 0.42f, COL_TEXT_DIM, g.c_str());
                y += 13 + 4;
                y = cardDivider(y);
            }
            // description: 4px pad
            float textTop = y + 4;
            int visible = (int)((BAR_Y - 4 - textTop) / 13);
            if (visible < 1) visible = 1;
            int maxScroll = (int)gDescLines.size() - visible;
            if (maxScroll < 0) maxScroll = 0;
            if (gDescScroll > maxScroll) gDescScroll = maxScroll;
            if (gDescLines.empty()) {
                drawLineTop(CTX, textTop, 13, 0.45f, COL_TEXT_DIM, "No description.");
            } else {
                for (int i = 0; i < visible && gDescScroll + i < (int)gDescLines.size(); i++)
                    drawLineTop(CTX, textTop + i * 13, 13, 0.45f, C2D_Color32(0xC6, 0xCF, 0xE2, 255),
                                gDescLines[gDescScroll + i].c_str());
                if (gDescScroll > 0)
                    drawArrow(CARD_X + CARD_W - 12, textTop + 2, 0.56f, 7, 7, COL_ACCENT, false);
                if (gDescScroll < maxScroll)
                    drawArrow(CARD_X + CARD_W - 12, BAR_Y - 12, 0.56f, 7, 7, COL_ACCENT, true);
            }
            int nsel = this->selectedCount();
            // one bar, bottom only: verb (with the batch count when rows are
            // marked), marks, find, back. START quits silently, as homebrew does.
            // marking mode drops the find hint so "R All/None" reads clearly.
            std::string hint = (nsel > 0)
                ? "A Install " + std::to_string(nsel) + "   Y Mark   R All/None   B Back"
                : std::string(onSD ? "A Manage" : "A Install") +
                  "   Y Mark   SEL Find   B Back";
            if (maxScroll > 0) hint += "   X/L Scroll";
            drawText(160, BAR_Y + (240 - BAR_Y) / 2, 0.56f, 0.42f, 0, COL_TEXT_DIM, hint.c_str(), C2D_AlignCenter);
            return;
        }
        if (this->type == MENU_MANAGE) {
            int nsel = this->selectedCount();
            std::string mhint = this->entries.empty()
                ? std::string("B Back")
                : (nsel > 0 ? "A Selected " + std::to_string(nsel) + "   Y Mark   R All/None   B Back"
                            : std::string("A Manage   Y Mark   SEL Find   B Back"));
            drawBottomFrame(mhint.c_str());
            if (this->entries.empty()) {
                const char* empty = (this->platformSlug == ROMM_SLUG_3DS) ? "No installed 3DS titles."
                                  : (this->platformSlug == ROMM_SLUG_GBA) ? "No roms in sd:/roms/gba"
                                  :                                          "No roms in sd:/roms/nds";
                drawText(160, 110, 0.55f, 0.5f, COL_SURFACE, COL_TEXT_DIM, empty, C2D_AlignCenter);
                return;
            }
            MenuSelection* sel = *this->selection;
            if (sel->action == CleanupCias) {   // the duplicates row explains itself here
                float dy = CARD_Y + PAD;
                dy = drawWrapped(CTX, dy, CTW, 17, 0.58f, COL_ACCENT, "Duplicate files", 1);
                dy += 5;
                drawChip(CTX, dy, humanSize(sel->sizeBytes) + " to free", COL_ACCENT);
                dy += 21;
                dy = cardDivider(dy) + 5;
                drawWrapped(CTX, dy, CTW, 14, 0.45f, C2D_Color32(0xC6,0xCF,0xE2,255),
                            std::to_string(sel->rommId) + " installer (.cia) files on the SD card are for "
                            "games that are already installed, so the card stores each of them twice. "
                            "Press A to delete just those files - the games stay installed.", 6);
                return;
            }
            if (sel->action == ManageZip) {   // interrupted download: zip never extracted
                float zy = CARD_Y + PAD;
                zy = drawWrapped(CTX, zy, CTW, 17, 0.58f, COL_TEXT, sel->title, 2);
                zy += 5;
                float zx = drawChip(CTX, zy, humanSize(sel->sizeBytes), COL_TEXT_DIM);
                drawChip(zx, zy, "not extracted", COL_ACCENT);
                zy += 21;
                zy = cardDivider(zy) + 5;
                drawWrapped(CTX, zy, CTW, 14, 0.45f, C2D_Color32(0xC6,0xCF,0xE2,255),
                            "Downloaded archive whose ROM was never extracted - the install "
                            "was interrupted. Press A to finish it (extract + install) or to "
                            "delete the file.", 5);
                return;
            }
            bool m3ds = (sel->platformSlug == ROMM_SLUG_3DS);
            float y = CARD_Y + PAD;
            y = drawWrapped(CTX, y, CTW, 17, 0.58f, COL_TEXT, sel->title, 2);
            y += 5;
            // 3DS: a game's updates/DLC are separate titles — show the real
            // total for the row, with the breakdown underneath
            static u64 gExtrasTid = 0;
            static TitleExtras gExtras;
            if (m3ds && sel->tid && sel->tid != gExtrasTid) {
                gExtrasTid = sel->tid;
                gExtras = findTitleExtras(sel->tid);
            }
            bool hasExtras = m3ds && sel->tid == gExtrasTid && !gExtras.empty();
            u64 totalBytes = sel->sizeBytes + (hasExtras ? gExtras.bytes : 0);
            float cxp = drawChip(CTX, y, humanSize(totalBytes), COL_TEXT_DIM);
            if (m3ds) {
                // the tab already says 3DS; the slot goes to the region.
                // extras collapse into one short chip — "update + DLC" plus
                // the rest overflowed the row; the breakdown lives below.
                if (const char* rg = regionChipText(sel->region))
                    cxp = drawChip(cxp, y, rg, COL_TEXT_DIM);
                cxp = drawChip(cxp, y, "INSTALLED", COL_ACCENT);
                if (sel->rommId > 0) cxp = drawChip(cxp, y, "on RomM", COL_TEXT_DIM);
                if (hasExtras) drawChip(cxp, y, "extras", COL_ACCENT);
            } else if (sel->platformSlug == ROMM_SLUG_GBA) {
                if (sel->installed) {
                    cxp = drawChip(cxp, y, "INSTALLED", COL_ACCENT);
                    // the screen preset baked into THIS game's install
                    static const char* scr[GBA_SCREEN_COUNT] =
                        {"AGS-101", "Original", "Raw", "Bright", "Night"};
                    if (sel->gbaScreen >= 0)
                        drawChip(cxp, y, scr[sel->gbaScreen % GBA_SCREEN_COUNT], COL_TEXT_DIM);
                } else {
                    drawChip(cxp, y, "not installed", COL_TEXT_DIM);
                }
            } else {
                // one "installed" chip - the user doesn't need the engine type
                // (romm3ds / TWiLight / YANBF) behind an installed DS game
                bool anyFwd = sel->rtid || sel->installed || sel->ytid;
                if (anyFwd) drawChip(cxp, y, "installed", COL_ACCENT);
                else drawChip(cxp, y, sel->fwdCia.empty() ? "not installed" : ".cia on SD", COL_TEXT_DIM);
            }
            y += 21;
            y = cardDivider(y) + 5;
            if (m3ds) {
                // no raw title-id hex here: it means nothing to the user
                if (hasExtras) {
                    char bd[96];
                    snprintf(bd, sizeof(bd), "Game %s  +  %u update/DLC %s",
                             humanSize(sel->sizeBytes).c_str(),
                             (unsigned)(gExtras.updates + gExtras.dlc),
                             humanSize(gExtras.bytes).c_str());
                    drawWrapped(CTX, y, CTW, 14, 0.45f, COL_TEXT_DIM, bd, 2);
                    drawWrapped(CTX, y + 30, CTW, 14, 0.45f, COL_TEXT_DIM,
                                "Press A to manage: uninstall everything, or remove just the update/DLC.", 2);
                } else {
                    drawWrapped(CTX, y, CTW, 14, 0.45f, COL_TEXT_DIM, "Installed. Press A to uninstall.", 2);
                }
                return;
            }
            if (!sel->installed && !sel->rtid && !sel->ytid) {
                drawWrapped(CTX, y, CTW, 14, 0.45f, COL_TEXT_DIM,
                            sel->fwdCia.empty()
                                ? "Not installed — the ROM is on your SD card. Press A to install."
                                : "Not installed — a ready .cia is on your SD card. Press A to install.", 3);
                return;
            }
            // installed: the baked banner, big and centered — exactly what
            // HOME shows. Size already lives in the chip row; no repeat here.
            if (gBannerPrev.tex && gBannerPrevTid && gBannerPrevTid == manageIconTid(sel))
                C2D_DrawImageAt(gBannerPrev, CARD_X + (CARD_W - 192) / 2, y,
                                0.57f, NULL, 0.75f, 0.75f);
            return;
        }
        if (this->type == MENU_SETTINGS || this->type == MENU_SERVER) {
            drawBottomFrame("A Change    B Back");
            static const char* descs[] = {
                "", // random title id (removed: asked on install when a duplicate is detected)
                "Ask for a custom HOME menu name on every install.",
                "Overwrite existing installs without asking first.",
                "", // language (removed)
                "Template used by SD card installs.",
                "RomM server address and account used by the library."
            };
            static const char* srvDescs[] = {
                "Address of your RomM instance, e.g. http://192.168.0.17 or http://host:8080.",
                "RomM account used to browse and download.",
                "Password for the account. Stored on the SD card.",
                "Checks the server connection and that RomM is reachable."
            };
            if (!this->entries.empty()) {
                MenuSelection* sel = *this->selection;
                float y = CARD_Y + PAD;
                y = drawWrapped(CTX, y, CTW, 17, 0.58f, COL_TEXT, sel->display, 2);
                y += 5;
                y = cardDivider(y) + 5;
                int id = sel->rommId;
                const char* d = nullptr;
                if (id >= 0 && id <= 5) d = descs[id];
                else if (id == SETTING_DELETE_SRC) d = "After installing from the SD card: delete the .cia (a duplicate of the installed game) or keep all files. DS/GBA game files are always kept - the game needs them to run and to change art.";
                else if (id == SETTING_ART_NOTIFY) d = "What happens when icon/banner art isn't found at install. Press A to choose - each choice is explained there.";
                else if (id == SETTING_SGDB_KEY) d = "HOME icons come from SteamGridDB. Press A to type the key, or create sd:/3ds/romm3ds/sgdb.env yourself - a text file with one line: STEAMGRIDDB_API_KEY=e51f8a33...";
                else if (id == SETTING_GBA_SCREEN) d = "Default color filter for new GBA installs. Press A to pick from the presets. Per game: Manage -> game -> Filter.";
                else if (id == SETTING_MANAGE_ART) d = "Art shown for installed games in Manage: each game's own HOME icon, or its RomM cover. Press A to choose.";
                else if (id == SETTING_ART_CACHE) d = "Downloaded covers, banner previews and title icons. Safe to clear - everything re-downloads or rebuilds on demand. The art you picked per game (art.json) is kept.";
                else if (id >= SETTING_SRV_HOST && id <= SETTING_SRV_TEST) d = srvDescs[id - SETTING_SRV_HOST];
                if (d)
                    drawWrapped(CTX, y, CTW, 14, 0.45f, C2D_Color32(0xC6,0xCF,0xE2,255), d, 4);
            }
            return;
        }
        if (this->type == MENU_LOCAL) {
            bool atRoot = browseParent(this->currentDirectory.generic_string()).empty();
            const char* bTxt = atRoot ? "B Exit" : "B Up";
            if (this->entries.empty()) {
                drawBottomFrame(gLocalHidden ? (std::string("X Show installed    ") + bTxt).c_str() : bTxt);
                if (gLocalHidden && !gLocalShowInstalled) {
                    drawText(160, 88, 0.55f, 0.45f, COL_SURFACE, COL_TEXT_DIM, "Nothing new here.", C2D_AlignCenter);
                    drawWrapped(48, 112, 224, 14, 0.42f, COL_TEXT_DIM,
                                "All " + std::to_string(gLocalHidden) + " games in this folder are already "
                                "installed. Press X to show them and reinstall or change art.", 4);
                } else {
                    drawText(160, 92, 0.55f, 0.45f, COL_SURFACE, COL_TEXT_DIM, "Empty folder.", C2D_AlignCenter);
                    drawWrapped(48, 116, 224, 14, 0.42f, COL_TEXT_DIM,
                                "Drop .cia / .nds / .gba (or a .zip) files here or in any folder, then browse to them.", 4);
                }
                return;
            }
            int nSel = 0;
            for (auto e : this->entries) if (e->selected) nSel++;
            drawBottomFrame("");
            MenuSelection* sel = *this->selection;
            u32 lineCol = C2D_Color32(0xC6,0xCF,0xE2,255);
            float y = CARD_Y + PAD;
            if (sel->action == LocalInstall) {
                y = drawWrapped(CTX, y, CTW, 17, 0.58f, COL_TEXT, sel->title, 2);
                y += 5;
                bool isZip = isZipName(sel->path.filename().generic_string());
                std::string tag = std::string((sel->platformSlug==ROMM_SLUG_3DS)?"3DS":
                                  (sel->platformSlug==ROMM_SLUG_GBA)?"GBA":"NDS") + (isZip?" zip":"");
                float cxp = drawChip(CTX, y, tag.c_str(), COL_TEXT_DIM);
                cxp = drawChip(cxp, y, humanSize(sel->sizeBytes), COL_TEXT_DIM);
                if (sel->installed) cxp = drawChip(cxp, y, "INSTALLED", COL_ACCENT);
                if (sel->selected)  drawChip(cxp, y, "SELECTED", COL_ACCENT);
                y += 21;
                y = cardDivider(y) + 5;
                // per platform: 3DS has no art/filter (its .cia carries the
                // icon); NDS has art but no filter; GBA has both
                bool is3 = (sel->platformSlug == ROMM_SLUG_3DS);
                bool isG = (sel->platformSlug == ROMM_SLUG_GBA);
                const char* desc =
                    sel->installed
                      ? (is3 ? "Installed. A reinstalls. Y marks several to install."
                         : isG ? "Installed. A: reinstall, change art or filter. Y marks several."
                               : "Installed. A: reinstall or change art. Y marks several.")
                      : (is3 ? "A installs it to the HOME menu. Y marks several to install."
                         : isG ? "A: install, with art / filter options. Y marks several."
                               : "A: install, with an art option. Y marks several to install.");
                drawWrapped(CTX, y, CTW, 14, 0.45f, lineCol, desc, 3);
            } else {   // folder or ".." row
                bool up = (sel->display.find("..") != std::string::npos);
                y = drawWrapped(CTX, y, CTW, 17, 0.58f, COL_TEXT,
                                up ? "Parent folder" : sel->path.filename().generic_string(), 2);
                y += 5;
                y = cardDivider(y) + 5;
                drawWrapped(CTX, y, CTW, 14, 0.45f, lineCol,
                            up ? "Press A (or B) to go up one folder."
                               : "Folder. Press A to open it.", 2);
            }
            std::string hint;
            if (nSel > 0)
                hint = "START Install " + std::to_string(nSel) + "   R All/None   " + bTxt;
            else if (sel->action == OpenFolder)
                hint = std::string("A Open   Y Mark   ") + bTxt;
            else
                hint = std::string("A Install   Y Mark   X ") +
                       (gLocalShowInstalled ? "Hide done   " : "Show all   ") + bTxt;
            drawText(160, BAR_Y + (240 - BAR_Y) / 2, 0.56f, 0.42f, 0, COL_TEXT_DIM,
                     hint.c_str(), C2D_AlignCenter);
            return;
        }
        // main menu / systems / SD browser
        drawBottomFrame("A Select    B Back");
        if (this->type == MENU_MAIN) {
            drawText(160, 70, 0.5f, 0.9f, COL_SURFACE, COL_TEXT, "romm3ds", C2D_AlignCenter);
            drawText(160, 96, 0.5f, 0.45f, COL_SURFACE, COL_TEXT_DIM, VERSION, C2D_AlignCenter);
            if (!gRomm.host.empty())
                drawText(160, 124, 0.5f, 0.45f, COL_SURFACE, COL_TEXT_DIM, gRomm.host.c_str(), C2D_AlignCenter);
        } else if (this->type == MENU_SYSTEMS) {
            bool manage = (this->heading.rfind("Manage", 0) == 0);
            if (manage) {
                // what the installed titles actually cost, per system.
                // gManageTally is filled once, when this menu is generated;
                // a stale one refreshes here as soon as the worker finishes
                if (gManageTallyStale && storageTallyCached()) {
                    gManageTally = computeStorageTally();
                    gManageTallyStale = false;
                    gManageTallyEver = true;
                }
                const StorageTally& s = gManageTally;
                float y = CARD_Y + PAD;
                drawLineTop(CTX, y, 17, 0.58f, COL_TEXT, "Installed");
                y += 17 + 4;
                y = cardDivider(y) + 6;
                if (gManageTallyStale && !gManageTallyEver) {
                    // first run: no old numbers to show — say so instead of zeros
                    drawWrapped(CTX, y + 8, CTW, 15, 0.45f, COL_TEXT_DIM,
                                "Calculating space usage...", 2);
                    drawWrapped(CTX, y + 40, CTW, 14, 0.42f, COL_TEXT_DIM,
                                "Pick a system to see what's installed, and uninstall or change art.", 3);
                    return;
                }
                auto row = [&](const char* label, u32 count, u64 bytes) {
                    drawLineTop(CTX, y, 15, 0.45f, COL_TEXT, label);
                    char v[64];
                    snprintf(v, sizeof(v), "%lu - %s", (unsigned long)count, humanSize(bytes).c_str());
                    drawText(CARD_X + CARD_W - PAD, y + 7.5f, 0.55f, 0.45f, 0, COL_TEXT_DIM, v, C2D_AlignRight);
                    y += 18;
                };
                // DS/GBA sizes include the rom files on the card — the
                // forwarder/inject titles alone say nothing about the space used
                row("Nintendo 3DS", s.appCount, s.appBytes);
                row("Nintendo DS", s.dsCount(), s.dsBytes());
                row("Game Boy Advance", s.gbaCount, s.gbaTotalBytes());
                if (s.extraCount > 0) row("Updates and DLC", s.extraCount, s.extraBytes);
                // plain installer files aren't the user's problem; only the
                // duplicates of already-installed games are reclaimable
                if (s.ciaDoneCount > 0) row("Duplicates - can be freed", s.ciaDoneCount, s.ciaDoneBytes);
                y += 4;
                y = cardDivider(y) + 8;
                drawChip(CTX, y, "SD free  " + humanSize(s.sdFreeBytes), COL_ACCENT);
                if (gManageTallyStale)
                    drawText(CARD_X + CARD_W - PAD, y + CHIP_H/2, 0.55f, 0.4f, 0,
                             COL_TEXT_DIM, "updating sizes...", C2D_AlignRight);
                y += CHIP_H + 8;
                drawWrapped(CTX, y, CTW, 14, 0.42f, COL_TEXT_DIM,
                            "Pick a system to see what's installed, and uninstall or change art.", 3);
                return;
            }
            drawText(160, 84, 0.5f, 0.5f, COL_SURFACE, COL_TEXT, "RomM Library", C2D_AlignCenter);
            drawWrapped(48, 108, 224, 14, 0.45f, COL_TEXT_DIM,
                        "Pick a system to browse and install games. "
                        "Search all systems looks across all three.", 4);
            if (!gRomm.host.empty())
                drawText(160, 172, 0.5f, 0.42f, COL_SURFACE, COL_TEXT_DIM, gRomm.host.c_str(), C2D_AlignCenter);
        } else {
            drawText(160, 84, 0.5f, 0.5f, COL_SURFACE, COL_TEXT, "SD card install", C2D_AlignCenter);
            drawWrapped(48, 108, 224, 14, 0.45f, COL_TEXT_DIM,
                        "Pick a .nds file to install it. Options live in Settings.", 3);
        }
    }

    // "Browse SD Card": a real folder browser, offline-first. Starts at the
    // roms folder (sdmc:/roms) and walks up to the SD root and into any
    // sibling folder — so a .cia the user keeps in sdmc:/cias, or roms parked
    // anywhere, just work. Rows are subfolders (A enters, B goes up) plus the
    // rom files in THIS folder, auto-typed by extension and marked "* " when
    // installed. Per-folder: install one (A), art/screen pickers, Y multiselect,
    // R all, "Install all here". No RomM needed; a cached library, if present,
    // lends its title/cover to the art pipeline.

    Menu* generateLocalMenu(Menu* prev, std::filesystem::path dir);   // fwd: rebuilds keep the folder

    // parent of a browse dir as a plain string. Floors at the SD root:
    // "sdmc:/roms/nds" -> "sdmc:/roms", "sdmc:/roms" -> "sdmc:/", root -> "".
    static std::string browseParent(std::string p) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();   // strip trailing slash
        size_t slash = p.find_last_of('/');
        if (slash == std::string::npos) return "";              // at/above "sdmc:" -> main menu
        if (slash <= 5) return "sdmc:/";                        // "sdmc:/xxx" -> SD root
        return p.substr(0, slash);
    }

    Menu* Menu::toggleShowInstalled() {
        if (this->type != MENU_LOCAL) return this;
        gLocalShowInstalled = !gLocalShowInstalled;
        return generateLocalMenu(this, this->currentDirectory);
    }

    Menu* generateLocalMenu(Menu* prev, std::filesystem::path dir) {
        delete prev;
        CoverCachePause coverPause;   // the scan owns the SD while it runs
        // resolve the folder to scan; empty -> the persisted dir, else the root
        std::string dirStr = dir.generic_string();
        if (dirStr.empty()) dirStr = gBrowseDir.empty() ? BROWSE_ROOT : gBrowseDir;
        std::error_code ec;
        if (!std::filesystem::is_directory(dirStr, ec)) {
            // first run: make the roms folder so it's always there to land on
            if (dirStr == BROWSE_ROOT) std::filesystem::create_directories(dirStr, ec);
            if (!std::filesystem::is_directory(dirStr, ec)) dirStr = "sdmc:/";
        }
        gBrowseDir = dirStr;
        gLocalHidden = 0;
        installed3dsRefresh();    // AM installed set: GBA inject + .cia detection
        refreshNdsForwarders();   // NDS forwarder detection
        // extension -> platform slug + tag. This is what makes the browser
        // layout-agnostic: a file's system comes from its name, not its folder.
        auto slugForExt = [](const std::string& ext, std::string& tag) -> std::string {
            if (ext == ".cia") { tag = "CIA"; return ROMM_SLUG_3DS; }
            if (ext == ".nds" || ext == ".srl" || ext == ".ids") { tag = "NDS"; return ROMM_SLUG_NDS; }
            if (ext == ".gba" || ext == ".agb") { tag = "GBA"; return ROMM_SLUG_GBA; }
            return "";
        };
        // optional metadata reuse: filename -> cached library entry, per slug,
        // only when that library is already loaded (never forces a load)
        auto libLookup = [](const std::string& slug, const std::string& fname) -> const RommRom* {
            if (!gCacheOk[slug]) return nullptr;
            std::string key = toLowerCase(fname);
            for (auto& cr : gCache[slug]) {
                if (toLowerCase(cr.fsName) == key) return &cr;
                if (toLowerCase(std::filesystem::path(rommLocalPath(cr.fsName, cr.platformSlug))
                        .filename().generic_string()) == key) return &cr;
            }
            return nullptr;
        };
        std::vector<MenuSelection*> folders;   // subfolders (A enters)
        std::vector<MenuSelection*> files;     // installable roms in this folder
        std::vector<std::filesystem::path> subdirs, romPaths;
        for (auto& de : std::filesystem::directory_iterator(dirStr, ec)) {
            std::string fn = de.path().filename().generic_string();
            if (fn.empty() || fn[0] == '.') continue;
            std::error_code de_ec;
            if (de.is_directory(de_ec)) { subdirs.push_back(de.path()); continue; }
            std::string tag;
            std::string ext = toLowerCase(de.path().extension().generic_string());
            if (slugForExt(ext, tag).empty() && ext != ".zip") continue;   // not a rom/zip we handle
            romPaths.push_back(de.path());
        }
        std::sort(subdirs.begin(), subdirs.end());
        std::sort(romPaths.begin(), romPaths.end());
        for (auto& p : subdirs) {
            MenuSelection* e = new MenuSelection();
            e->action = OpenFolder;
            e->path = p;
            e->display = "  " + utf8FoldLatin(p.filename().generic_string()) + "/";
            folders.push_back(e);
        }
        // predicted extracted rom name for a zip (deterministic: stem + the
        // platform's rom extension) — used for the marker and, at install, as
        // the forced extract name so markers/tids/covers all agree.
        auto platformExt = [](const std::string& slug) -> std::string {
            if (slug == ROMM_SLUG_3DS) return ".cia";
            if (slug == ROMM_SLUG_GBA) return ".gba";
            return ".nds";
        };
        for (auto& p : romPaths) {
            std::string fname = p.filename().generic_string();
            std::string stem  = p.stem().generic_string();
            std::string tag, ext = toLowerCase(p.extension().generic_string());
            bool isZip = (ext == ".zip");
            // zip: type comes from the rom inside, not the archive extension
            std::string slug = isZip ? zipInnerSlug(p.generic_string()) : slugForExt(ext, tag);
            if (slug.empty()) continue;   // empty/foreign zip
            if (isZip) tag = (slug==ROMM_SLUG_3DS?"3DS":slug==ROMM_SLUG_GBA?"GBA":"NDS") + std::string(" zip");
            // the file the install will actually work on (zip -> predicted rom)
            std::string romName = isZip ? (stem + platformExt(slug)) : fname;
            MenuSelection* e = new MenuSelection();
            e->action = LocalInstall;
            e->platformSlug = slug;
            e->path = p;
            e->fsName = romName;   // zip: the name it extracts to (not the .zip)
            e->sizeBytes = std::filesystem::file_size(p, ec);
            const RommRom* lib = libLookup(slug, romName);
            e->title = lib ? lib->name : stem;
            if (lib) { e->rommId = lib->id; e->coverPath = lib->coverPath;
                       e->coverSmallPath = lib->coverSmallPath; e->year = lib->year; }
            bool inst;
            if (slug == ROMM_SLUG_3DS) {
                // a zip's inner cia id is unknown without extracting -> unmarked
                e->titleId = isZip ? 0 : ciaFileTitleId(p.generic_string());
                inst = e->titleId && installed3dsHasTitle(e->titleId);
            } else if (slug == ROMM_SLUG_GBA) {
                e->tid = gbaTidForRom(romName);
                inst = installed3dsHasTitle(e->tid);
            } else {
                inst = ndsForwarderInstalled(romName);
            }
            e->installed = inst;
            if (inst && !gLocalShowInstalled) { gLocalHidden++; delete e; continue; }
            e->display = (inst ? "* " : "  ") + std::string("[") + tag + "] " + utf8FoldLatin(stem);
            files.push_back(e);
        }
        // assemble: [.. up] -> folders -> files. Batch install is by Y-mark +
        // START (or A on a marked row) — same as the RomM / Manage screens — so
        // no pinned action rows here; R marks all, then START installs them all.
        std::vector<MenuSelection*> entries;
        std::string parent = browseParent(dirStr);
        if (!parent.empty()) {   // not the SD root: offer a ".." row (B also goes up)
            MenuSelection* up = new MenuSelection();
            up->action = OpenFolder;
            up->path = std::filesystem::path(parent);
            up->display = "  .. (up)";
            entries.push_back(up);
        }
        entries.insert(entries.end(), folders.begin(), folders.end());
        entries.insert(entries.end(), files.begin(), files.end());
        Menu* menu = new Menu(entries);
        menu->currentDirectory = std::filesystem::path(dirStr);
        menu->type = MENU_LOCAL;
        FS_ArchiveResource sd = {};
        std::string free = "";
        if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD)))
            free = " - " + humanSize((u64)sd.freeClusters * sd.clusterSize) + " free";
        // friendly heading: path relative to the SD root ("roms/nds"), or "SD root"
        std::string rel = dirStr;
        if (rel.rfind("sdmc:/", 0) == 0) rel = rel.substr(6);
        while (!rel.empty() && rel.back() == '/') rel.pop_back();
        if (rel.empty()) rel = "SD root";
        // keep it short (like "Manage NDS - X free"): path + free, plus a terse
        // hidden-count. The bottom panel / X hint explain the rest.
        menu->heading = rel + free
                      + (gLocalHidden && !gLocalShowInstalled
                         ? "  (" + std::to_string(gLocalHidden) + " hidden)" : "");
        menu->init();
        return menu;
    }
    Menu* generateMainMenu(Menu* prev) {
        delete prev;
        std::vector<MenuSelection*> entries;
        // offline-first: the SD browser leads, RomM is one optional source
        MenuSelection* sd = new MenuSelection();
        sd->display="Browse SD Card";
        sd->action=OpenSDBrowser;
        entries.push_back(sd);
        MenuSelection* manage = new MenuSelection();
        manage->display="Manage Installed";
        manage->action=OpenManage;
        entries.push_back(manage);
        MenuSelection* romm = new MenuSelection();
        romm->display="RomM Library";
        romm->action=OpenRommLibrary;
        entries.push_back(romm);
        MenuSelection* cfg = new MenuSelection();
        cfg->display="Settings";
        cfg->action=OpenSettings;
        entries.push_back(cfg);
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_MAIN;
        menu->heading="romm3ds";
        menu->init();
        return menu;
    }


    static Config* gConfigPtr = nullptr;

    Menu* generateServerMenu(Menu* prev) {
        delete prev;
        gRomm.loadConfig();
        std::vector<MenuSelection*> entries;
        auto add = [&](int id, const std::string& label) {
            MenuSelection* e = new MenuSelection();
            e->display = label;
            e->action = SettingToggle;
            e->rommId = id;
            entries.push_back(e);
        };
        add(SETTING_SRV_HOST, "Server: " + (gRomm.host.empty() ? "not set" : gRomm.host));
        add(SETTING_SRV_USER, "Username: " + (gRomm.user.empty() ? "not set" : gRomm.user));
        add(SETTING_SRV_PASS, std::string("Password: ") + (gRomm.pass.empty() ? "not set" : "******"));
        add(SETTING_SRV_TEST, "Test connection");
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_SERVER;
        menu->heading="RomM server";
        menu->init();
        return menu;
    }

    Menu* generateSettingsMenu(Menu* prev, Config* config) {
        delete prev;
        std::vector<MenuSelection*> entries;
        auto add = [&](int id, const std::string& label) {
            MenuSelection* e = new MenuSelection();
            e->display = label;
            e->action = SettingToggle;
            e->rommId = id;
            entries.push_back(e);
        };
        add(SETTING_CUSTOM_TITLE, std::string("Ask for custom title: ") + (config->customTitle ? "on" : "off"));
        add(SETTING_FORCE,        std::string("Force install: ") + (config->forceInstall ? "on" : "off"));
        add(SETTING_DELETE_SRC,   std::string("After install: ") + (config->deleteAfterInstall ? "delete .cia source" : "keep source files"));
        add(SETTING_ART_NOTIFY,   std::string("Art: ") + (config->artNotify ? "notify when missing" : "silent fallback"));
        add(SETTING_SGDB_KEY,     std::string("SteamGridDB key: ") + (ensureSgdb() ? "found" : "missing"));
        static const char* gbaScreenNames[] = {"AGS-101 colors", "original dark filter", "unfiltered", "brighter gamma", "night (warm)"};
        add(SETTING_GBA_SCREEN,   std::string("GBA screen: ") + gbaScreenNames[config->gbaScreen % GBA_SCREEN_COUNT]);
        add(SETTING_MANAGE_ART,   std::string("Manage art: ") + (config->manageIcons ? "title icons" : "RomM covers"));
        {
            int cf; u64 cb;
            artCacheStats(cf, cb);
            add(SETTING_ART_CACHE, std::string("Art cache: ") +
                (cf ? std::to_string(cf) + " files - " + humanSize(cb) : "empty"));
        }
        if (config->templates.size() > 1)
            add(SETTING_TEMPLATE, "Template: " + config->templates.at(config->currentTemplate));
        gRomm.loadConfig();
        add(SETTING_SERVER,       "RomM server: " + (gRomm.host.empty() ? "not set" : gRomm.host));
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_SETTINGS;
        menu->heading="Settings";
        menu->init();
        return menu;
    }
    // builds the RomM list from a cached library, optionally filtered.
    // src = the platform cache (or the combined list for cross-system search).
    // canonical key: drop extension, region/() tags, punctuation; lowercase alnum only.
    // makes RomM "Pokemon - X (Spain).zip" match the SD forwarder's "Pokemon X.nds".
    static std::string normNds(std::string s) {
        size_t dot = s.find_last_of('.');
        if (dot != std::string::npos && dot + 5 >= s.size()) s = s.substr(0, dot);
        std::string o; bool paren = false;
        for (char c : s) {
            if (c == '(' || c == '[') paren = true;
            else if (c == ')' || c == ']') paren = false;
            else if (!paren && std::isalnum((unsigned char)c)) o += (char)std::tolower((unsigned char)c);
        }
        return o;
    }
    // build once/session: NDS roms with ANY forwarder on the HOME menu (all 3 types, AM-verified)
    static void refreshNdsForwarders() {
        if (gFwdReady) return;
        gFwdNames.clear();
        for (auto& m : scanManagedRoms(ROMM_NDS_DIR))
            if (m.installed || m.yanbfTid || m.rommTid)
                gFwdNames.insert(normNds(m.display));
        gFwdReady = true;
    }
    // is a forwarder for this NDS rom currently on the HOME menu?
    static bool ndsForwarderInstalled(const std::string& fsName) {
        return gFwdNames.count(normNds(fsName)) > 0;
    }

    // filter + slug are taken BY VALUE on purpose: we delete `prev` below, and
    // callers pass this->filter / this->platformSlug — a reference would dangle.
    static Menu* buildRommMenu(Menu* prev, std::string filter,
                               const std::vector<RommRom>& src,
                               std::string slug, bool cross) {
        delete prev;
        std::string flow = toLowerCase(filter);
        installed3dsRefresh();     // 3DS: titles installed on the console
        refreshNdsForwarders();    // NDS: forwarders on the HOME menu
        std::vector<MenuSelection*> entries;
        for (auto& rom : src) {
            if (!flow.empty() &&
                toLowerCase(rom.name).find(flow) == std::string::npos &&
                toLowerCase(rom.fsName).find(flow) == std::string::npos)
                continue;
            // only decrypted .cia files install on-device: hide the rest
            if (rom.platformSlug == ROMM_SLUG_3DS && !rom.installable)
                continue;
            MenuSelection* e = new MenuSelection();
            // marker: 3DS/GBA = title installed on the console (AM, by title id);
            //         NDS = forwarder on the HOME menu
            bool marked = (rom.platformSlug == ROMM_SLUG_3DS)
                          ? installed3dsHasTitle(rom.titleId)
                          : rom.platformSlug == ROMM_SLUG_GBA
                              ? installed3dsHasTitle(gbaTidForRom(std::filesystem::path(rommLocalPath(rom.fsName, rom.platformSlug)).filename().generic_string()))
                              : ndsForwarderInstalled(rom.fsName);
            std::string tag = cross ? (std::string("[") + (rom.platformSlug==ROMM_SLUG_3DS?"3DS":
                                       rom.platformSlug==ROMM_SLUG_GBA?"GBA":"DS") + "] ") : "";
            e->display=(marked?"* ":"  ")+tag+rom.name;
            e->action=RommInstall;
            e->rommId=rom.id;
            e->fsName=rom.fsName;
            e->fileId=rom.fileId;
            e->titleId=rom.titleId;
            e->platformSlug=rom.platformSlug;
            e->installable=rom.installable;
            e->title=rom.name;
            e->coverPath=rom.coverPath;
            e->coverSmallPath=rom.coverSmallPath;
            e->summary=rom.summary;
            e->genres=rom.genres;
            e->year=rom.year;
            e->rating=rom.rating;
            e->sizeBytes=rom.sizeBytes;
            e->path=std::filesystem::path(rommDirFor(rom.platformSlug) + rom.fsName);
            entries.push_back(e);
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_ROMM;
        menu->filter=filter;
        menu->platformSlug=slug;
        menu->crossSystem=cross;
        std::string scope = cross ? std::string("All systems") : std::string(systemName(slug));
        if (filter.empty())
            menu->heading=scope;   // search hint lives in the bottom bar only
        else
            menu->heading="\""+filter+"\" - "+std::to_string(entries.size())+" found";
        rlog.info(" buildRommMenu: " + std::to_string(entries.size()) + " entries (slug=" + slug + " cross=" + (cross?"1":"0") + ")");
        menu->init();
        rlog.info(" menu ready");
        return menu;
    }

    // system-selection screen shown when entering the RomM Library
    Menu* generateSystemMenu(Menu* prev) {
        delete prev;
        std::vector<MenuSelection*> entries;
        auto add = [&](const std::string& label, MenuAction act, const std::string& slug){
            MenuSelection* e = new MenuSelection();
            e->display = label;
            e->action = act;
            e->platformSlug = slug;
            entries.push_back(e);
        };
        add("Nintendo 3DS", OpenPlatform, ROMM_SLUG_3DS);
        add("Nintendo DS", OpenPlatform, ROMM_SLUG_NDS);
        add("Game Boy Advance", OpenPlatform, ROMM_SLUG_GBA);
        add("Search all systems", OpenSearchAll, "");
        add("Refresh from server", RefreshLibraries, "");
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_SYSTEMS;
        menu->heading="RomM Library";
        menu->init();
        return menu;
    }

    Menu* generateRommMenu(Menu* prev, C3D_RenderTarget* target, const std::string& slug) {
        rlog.info("open library slug=" + slug);
        if (!gRomm.hasConfig() && !gRomm.loadConfig()) {
            if (!gRomm.promptConfig()) {
                return (prev!=nullptr)?prev:generateMainMenu(nullptr);
            }
        }
        showLoading(target, {"Connecting to RomM...", gRomm.host});
        showLoading(target, {std::string("Loading ")+systemName(slug)+" library..."});
        if (!ensurePlatformLoaded(slug, target)) {
            Dialog(target,0,0,320,240,{"RomM error",gRomm.lastError},{"OK"}).handle();
            return (prev!=nullptr)?prev:generateMainMenu(nullptr);
        }
        return buildRommMenu(prev, "", gCache[slug], slug, false);
    }

    // cross-system search: ask for the query FIRST, then load + filter
    Menu* generateSearchAllMenu(Menu* prev, C3D_RenderTarget* target) {
        if (!gRomm.hasConfig() && !gRomm.loadConfig()) {
            if (!gRomm.promptConfig()) return (prev!=nullptr)?prev:generateMainMenu(nullptr);
        }
        char buf[64] = {0};
        SwkbdState kb;
        swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 63);
        swkbdSetHintText(&kb, "Search all systems (empty = all)");
        swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
        if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM)
            return generateSystemMenu(prev);   // cancelled -> back to system pick
        showLoading(target, {"Loading all systems..."});
        bool anyNds = ensurePlatformLoaded(ROMM_SLUG_NDS, target);
        bool any3ds = ensurePlatformLoaded(ROMM_SLUG_3DS, target);
        bool anyGba = ensurePlatformLoaded(ROMM_SLUG_GBA, target);
        if (!anyNds && !any3ds && !anyGba) {
            Dialog(target,0,0,320,240,{"RomM error",gRomm.lastError},{"OK"}).handle();
            return (prev!=nullptr)?prev:generateMainMenu(nullptr);
        }
        gCombined.clear();
        if (anyNds) gCombined.insert(gCombined.end(), gCache[ROMM_SLUG_NDS].begin(), gCache[ROMM_SLUG_NDS].end());
        if (any3ds) gCombined.insert(gCombined.end(), gCache[ROMM_SLUG_3DS].begin(), gCache[ROMM_SLUG_3DS].end());
        if (anyGba) gCombined.insert(gCombined.end(), gCache[ROMM_SLUG_GBA].begin(), gCache[ROMM_SLUG_GBA].end());
        return buildRommMenu(prev, std::string(buf), gCombined, "", true);
    }

    Menu* Menu::searchPrompt() {
        if (this->type != MENU_ROMM && this->type != MENU_MANAGE) return this;
        char buf[64] = {0};
        SwkbdState kb;
        swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 63);
        swkbdSetHintText(&kb, "Search games (empty = show all)");
        swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
        if (!this->filter.empty()) swkbdSetInitialText(&kb, this->filter.c_str());
        if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM)
            return this;
        if (this->type == MENU_MANAGE) {
            // filter the manage list in place; empty query = full list again
            std::string q = toLowerCase(std::string(buf));
            std::string slug = this->platformSlug;
            if (q.empty()) {
                while (this->queue.size() > 0) this->queue.pop();
                return generateManageMenu(this, 0, slug, nullptr);
            }
            std::vector<MenuSelection*> keep;
            for (auto e : this->entries) {
                if (toLowerCase(e->title).find(q) != std::string::npos ||
                    toLowerCase(e->display).find(q) != std::string::npos)
                    keep.push_back(e);
                else
                    delete e;
            }
            this->entries.clear();          // survivors now belong to the new menu
            Menu* m = new Menu(keep);
            m->type = MENU_MANAGE;
            m->platformSlug = slug;
            m->filter = std::string(buf);
            m->currentDirectory = std::filesystem::path("/");
            m->heading = "\"" + std::string(buf) + "\" - " + std::to_string((int)keep.size()) + " found";
            m->init();
            delete this;
            return m;
        }
        const std::vector<RommRom>& src = this->crossSystem ? gCombined : gCache[this->platformSlug];
        return buildRommMenu(this, std::string(buf), src, this->platformSlug, this->crossSystem);
    }
    // Manage system-selection screen (NDS / 3DS), mirrors the library flow
    Menu* generateManageSystemMenu(Menu* prev) {
        delete prev;
        // storage breakdown for the bottom panel. Cached -> instant; stale
        // (an install/uninstall happened) -> recompute on a WORKER while the
        // panel keeps the old numbers, so B here never blocks on AM/SD
        if (storageTallyCached()) {
            gManageTally = computeStorageTally();
            gManageTallyStale = false;
            gManageTallyEver = true;
        } else {
            gManageTallyStale = true;
            storageTallyKickAsync();
        }
        std::vector<MenuSelection*> entries;
        auto add = [&](const std::string& label, const std::string& slug){
            MenuSelection* e = new MenuSelection();
            e->display = label; e->action = OpenManage; e->platformSlug = slug;
            entries.push_back(e);
        };
        add("Nintendo 3DS", ROMM_SLUG_3DS);
        add("Nintendo DS", ROMM_SLUG_NDS);
        add("Game Boy Advance", ROMM_SLUG_GBA);
        // Duplicate installers: .cia files still on the card whose title is
        // already installed - pure duplicates of what's in the title database.
        // Its own top-level entry now (was buried in the 3DS list), so freeing
        // that space is a deliberate, visible action.
        installed3dsRefresh();
        {
            u64 dupBytes = 0; int dupCount = 0;
            for (auto& c : listCiaFiles()) if (c.installed) { dupBytes += c.sizeBytes; dupCount++; }
            if (dupCount > 0) {
                MenuSelection* e = new MenuSelection();
                e->action = CleanupCias;
                e->title = "Duplicate installers";
                e->display = "Duplicate installers - " + humanSize(dupBytes);
                e->sizeBytes = dupBytes;
                e->rommId = dupCount;   // count, for the details panel
                entries.push_back(e);
            }
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory = std::filesystem::path("/");
        menu->type = MENU_SYSTEMS;   // back -> main
        menu->heading = "Manage Installed";
        menu->init();
        return menu;
    }

    // interrupted downloads: .zip archives in a rom dir whose ROM never got
    // extracted (B during extract / crash). Listed in Manage so they can be
    // finished (extract + install) or deleted — invisible everywhere else.
    static void appendZipRows(std::vector<MenuSelection*>& entries, const std::string& dir,
                              const std::string& slug,
                              const std::map<std::string, const RommRom*>& libByName) {
        std::error_code ec;
        std::vector<std::filesystem::path> zips;
        for (auto& de : std::filesystem::directory_iterator(dir, ec))
            if (toLowerCase(de.path().extension().generic_string()) == ".zip")
                zips.push_back(de.path());
        std::sort(zips.begin(), zips.end());
        for (auto& p : zips) {
            MenuSelection* e = new MenuSelection();
            std::string stem = p.stem().generic_string();
            e->action = ManageZip;
            e->platformSlug = slug;
            e->path = p;
            e->title = utf8FoldLatin(stem);
            e->display = "  [zip] " + utf8FoldLatin(stem);
            e->sizeBytes = std::filesystem::file_size(p, ec);
            auto hit = libByName.find(toLowerCase(p.filename().generic_string()));
            if (hit != libByName.end()) {
                e->year = hit->second->year;
                e->coverPath = hit->second->coverPath;
                e->coverSmallPath = hit->second->coverSmallPath;
                e->rommId = hit->second->id;
            }
            entries.push_back(e);
        }
    }

    Menu* generateManageMenu(Menu* prev, unsigned long dsiwareCount, std::string slug,
                             C3D_RenderTarget* target) {
        (void)dsiwareCount;
        delete prev;
        CoverCachePause coverPause;   // the AM/SD scans own the card while they run
        // drop the rail art slots: after a rebake the same tid must reload
        // its (new) icon and banner instead of showing the old textures
        if (gTitleIcon.tex) freeTexImage(&gTitleIcon);
        gTitleIconTid = 0; gIconWantTid = 0;
        if (gBannerPrev.tex) freeTexImage(&gBannerPrev);
        gBannerPrevTid = 0; gBannerWantTid = 0;
        std::vector<MenuSelection*> entries;
      if (slug == ROMM_SLUG_3DS) {
        installed3dsRefresh();   // the .cia scan below needs the AM set
        // (Duplicate installers moved to their own row on the Manage picker.)
        // 3DS: EVERY installed app on SD (not just the ones on RomM). Titles
        // that match a library entry borrow its display name/year; art is
        // always the title's own HOME icon (never the RomM cover — cover
        // loads fought the SMDH reads and made this screen crawl). Biggest
        // first — this screen is about space. quiet=true: no refresh/cover
        // worker while the scan owns the SD card.
        if (!gCacheOk[ROMM_SLUG_3DS] && gRomm.loadConfig() && gRomm.hasConfig())
            ensurePlatformLoaded(ROMM_SLUG_3DS, nullptr, true);
        installed3dsRefresh();
        std::map<u64, const RommRom*> libByTid;
        for (auto& cr : gCache[ROMM_SLUG_3DS])
            if (cr.titleId) libByTid.emplace(cr.titleId, &cr);
        // first pass reads one SMDH per title; cached on SD, so later opens
        // don't hit the card at all
        std::vector<InstalledTitle> titles = listInstalledApps(true, true,
            [&](int done, int need) {
                showLoading(target, {"Reading titles...",
                                     std::to_string(done + 1) + "/" + std::to_string(need)});
            });
        for (auto& t : titles) {
            if (t.protectedTitle) continue;                        // this app, system titles
            if (t.kind != TK_APP && t.kind != TK_DEMO) continue;   // our forwarders/injects: own tabs
            MenuSelection* e = new MenuSelection();
            std::string name = t.name;
            e->action = ManageRom;
            e->platformSlug = ROMM_SLUG_3DS;
            e->tid = t.tid;
            e->region = t.region;
            e->installed = true;
            e->protectedTitle = t.protectedTitle;
            e->sizeBytes = t.sizeBytes;   // installed size, not the server's file size
            auto hit = libByTid.find(t.tid);
            if (hit != libByTid.end()) {
                const RommRom* cr = hit->second;
                if (!cr->name.empty()) name = utf8FoldLatin(cr->name);
                e->year = cr->year;
                // rommId<=0 makes the rail show the title's own icon instead
                // of fetching the cover (Settings -> Manage art)
                if (!(gConfigPtr && gConfigPtr->manageIcons)) {
                    e->rommId = cr->id;
                    e->coverPath = cr->coverPath;
                    e->coverSmallPath = cr->coverSmallPath;
                }
            }
            if (name.empty()) name = "Unknown title";
            e->title = name;
            e->display = "* " + name + (t.kind == TK_DEMO ? " (demo)" : "");
            entries.push_back(e);
        }
      } else if (slug == ROMM_SLUG_GBA) {
        // GBA: roms on SD + inject install state (AM, deterministic tid)
        if (!gCacheOk[ROMM_SLUG_GBA] && gRomm.loadConfig() && gRomm.hasConfig())
            ensurePlatformLoaded(ROMM_SLUG_GBA);
        installed3dsRefresh();
        // filename -> lib entry (covers/metadata), keyed by the extracted name
        // (and the server fsName, so un-extracted .zip rows match too)
        std::map<std::string, const RommRom*> libByName;
        for (auto& cr : gCache[ROMM_SLUG_GBA]) {
            libByName.emplace(toLowerCase(std::filesystem::path(
                rommLocalPath(cr.fsName, cr.platformSlug)).filename().generic_string()), &cr);
            libByName.emplace(toLowerCase(cr.fsName), &cr);
        }
        std::error_code ec;
        std::vector<std::filesystem::path> paths;
        for (auto& de : std::filesystem::directory_iterator(ROMM_GBA_DIR, ec)) {
            std::string ext = toLowerCase(de.path().extension().generic_string());
            if (ext == ".gba" || ext == ".agb") paths.push_back(de.path());
        }
        std::sort(paths.begin(), paths.end());
        for (auto& p : paths) {
            std::string fname = p.filename().generic_string();
            u64 gtid = gbaTidForRom(fname);
            bool inst = installed3dsHasTitle(gtid);
            MenuSelection* e = new MenuSelection();
            std::string clean = p.stem().generic_string();
            ArtEntry gae = artStoreGet(fname);
            bool weakArt = inst && gae.weak;   // ⚠: fallback art in use
            e->gbaScreen = gae.screen;         // filter chip in the details card
            e->display = (inst ? "* " : "  ") + std::string(weakArt ? "[!] " : "") + utf8FoldLatin(clean);
            e->title = utf8FoldLatin(clean);
            e->action = ManageRom;
            e->platformSlug = ROMM_SLUG_GBA;
            e->path = p;
            e->tid = gtid;
            e->installed = inst;
            e->sizeBytes = std::filesystem::file_size(p, ec);
            auto hit = libByName.find(toLowerCase(fname));
            if (hit != libByName.end()) {
                const RommRom* cr = hit->second;
                e->year = cr->year;
                e->coverPath = cr->coverPath;          // art flows still need it
                e->coverSmallPath = cr->coverSmallPath;
                if (!(gConfigPtr && gConfigPtr->manageIcons) || !inst)
                    e->rommId = cr->id;                // rail cover only in covers mode
            }
            entries.push_back(e);
        }
        appendZipRows(entries, ROMM_GBA_DIR, ROMM_SLUG_GBA, libByName);
      } else {
        // NDS: roms on SD + their forwarder state
        if (!gCacheOk[ROMM_SLUG_NDS] && gRomm.loadConfig() && gRomm.hasConfig())
            ensurePlatformLoaded(ROMM_SLUG_NDS);
        std::vector<ManagedRom> roms = scanManagedRoms(ROMM_NDS_DIR);
        // filename -> lib entry index, built once (was a linear scan per rom)
        std::map<std::string, const RommRom*> libByName;
        for (auto& cr : gCache[ROMM_SLUG_NDS]) {
            libByName.emplace(toLowerCase(cr.fsName), &cr);
            libByName.emplace(toLowerCase(std::filesystem::path(
                rommLocalPath(cr.fsName, cr.platformSlug)).filename().generic_string()), &cr);
        }
        for (auto& rom : roms) {
            MenuSelection* e = new MenuSelection();
            // clean name: no extension, no decorations; dot marks "has forwarder"
            std::string clean = rom.display;
            size_t dot = clean.find_last_of('.');
            if (dot != std::string::npos) clean = clean.substr(0, dot);
            bool hasFwd = rom.rommTid || rom.installed || rom.yanbfTid;
            bool weakArt = hasFwd && artStoreGet(rom.display).weak;   // ⚠: fallback art
            e->display = (hasFwd ? "* " : "  ") + std::string(weakArt ? "[!] " : "") + utf8FoldLatin(clean);
            e->title = utf8FoldLatin(clean);
            e->action=ManageRom;
            e->path=std::filesystem::path(rom.path);
            e->tid=rom.tid;
            e->ytid=rom.yanbfTid;
            e->rtid=rom.rommTid;
            e->installed=rom.installed;
            e->fwdCia=rom.orphanCia;
            e->sizeBytes=rom.sizeBytes;
            // match against the NDS RomM cache to reuse cover art
            auto hit = libByName.find(toLowerCase(rom.display));
            if (hit != libByName.end()) {
                const RommRom* cr = hit->second;
                e->year = cr->year;
                e->coverPath = cr->coverPath;          // art flows still need it
                e->coverSmallPath = cr->coverSmallPath;
                if (!(gConfigPtr && gConfigPtr->manageIcons) || !hasFwd)
                    e->rommId = cr->id;                // rail cover only in covers mode
            }
            entries.push_back(e);
        }
        appendZipRows(entries, ROMM_NDS_DIR, ROMM_SLUG_NDS, libByName);
      }
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_MANAGE;
        menu->platformSlug=slug;
        FS_ArchiveResource sd = {};
        std::string free = "";
        if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD)))
            free = " - " + humanSize((u64)sd.freeClusters * sd.clusterSize) + " free";
        menu->heading = std::string(slug==ROMM_SLUG_3DS ? "Manage 3DS" :
                                    slug==ROMM_SLUG_GBA ? "Manage GBA" : "Manage NDS") + free;
        menu->init();
        return menu;
    }
    void Menu::init() {
        this->top=entries.begin();
        this->selection=entries.begin();
    }
    // put the cursor back on row i after a rebuild (Settings toggles): the
    // list scrolls just enough to keep the row visible
    void Menu::selectIndex(size_t i) {
        if (this->entries.empty()) return;
        if (i >= this->entries.size()) i = this->entries.size() - 1;
        this->selection = this->entries.begin() + i;
        this->top = this->entries.begin();
        if (this->entries.size() > (size_t)MAX_ENTRY_COUNT && i >= (size_t)MAX_ENTRY_COUNT)
            this->top = this->entries.begin() + (i - MAX_ENTRY_COUNT + 1);
    }
    void Menu::down() {
        if (this->entries.size()==0) return;
        if (this->selection+1 == this->entries.end()) {
            this->selection=this->entries.begin();
            this->top=this->entries.begin();
        }else{
            if (this->selection+1==this->top+MAX_ENTRY_COUNT) {
                top++;
            }
            this->selection++;
        }
    }
    void Menu::up() {
        if (this->entries.size()==0) return;
        if (this->selection == this->entries.begin()) {
            this->selection=this->entries.end()-1;
            if (this->entries.size() > MAX_ENTRY_COUNT)
                this->top=this->entries.end()-MAX_ENTRY_COUNT;
        }else{
            if (this->selection==this->top) {
                this->top--;
            }
            this->selection--;
        }
    }
    void Menu::action() {
        if (this->entries.size()==0) return;
        // A on a MARKED row acts on the whole selection (same as START);
        // A on an unmarked row still manages/installs just that one
        MenuSelection* sel = *this->selection;
        if (sel->selected && this->selectedCount() > 0 && this->startBatch()) return;
        // copy the pointed-to row in full (implicit copy ctor = all fields).
        // NB double deref: entries holds MenuSelection*, so *selection is the
        // pointer; **selection is the object. The old MenuSelection(ptr) ctor
        // copied only 21/26 fields and silently dropped fwdCia/region/gbaScreen.
        this->queue.push(MenuSelection(**this->selection));
    }
    // Y: toggle the batch mark on the current row. Only the installable
    // library rows and the manage rows (zip archives included) can be
    // selected; a non-installable 3DS .3ds row is skipped.
    void Menu::toggleSelect() {
        if (this->entries.empty()) return;
        MenuSelection* sel = *this->selection;
        if (sel->action != RommInstall && sel->action != ManageRom &&
            sel->action != ManageZip && sel->action != LocalInstall) return;
        if (sel->action == RommInstall && sel->platformSlug == ROMM_SLUG_3DS && !sel->installable) return;
        sel->selected = !sel->selected;
    }
    int Menu::selectedCount() {
        int n = 0;
        for (auto e : this->entries) if (e->selected) n++;
        return n;
    }
    // R: mark every selectable row; when everything is already marked, a
    // second press clears all (all/none toggle — partial marks extend to all)
    void Menu::toggleSelectAll() {
        if (this->entries.empty()) return;
        auto selectable = [](MenuSelection* e) {
            if (e->action != RommInstall && e->action != ManageRom &&
                e->action != LocalInstall && e->action != ManageZip) return false;
            if (e->action == RommInstall && e->platformSlug == ROMM_SLUG_3DS && !e->installable) return false;
            return true;
        };
        bool any = false, allSelected = true;
        for (auto e : this->entries) {
            if (!selectable(e)) continue;
            any = true;
            if (!e->selected) allSelected = false;
        }
        if (!any) return;
        if (allSelected) { this->clearSelection(); return; }
        for (auto e : this->entries)
            if (selectable(e)) e->selected = true;
    }
    void Menu::clearSelection() {
        for (auto e : this->entries) e->selected = false;
    }
    // START: queue a batch action for the selected rows. Returns false when
    // there's nothing to batch here, so the caller quits the app instead.
    bool Menu::startBatch() {
        if (this->type == MENU_LOCAL) {      // Install-from-SD batch
            int nSel = 0;
            for (auto e : this->entries) if (e->selected) nSel++;
            if (nSel == 0) return false;     // nothing marked -> START quits as usual
            MenuSelection s("");
            s.action = LocalInstallSelected;
            this->queue.push(s);
            return true;
        }
        if (this->selectedCount() == 0) return false;
        MenuSelection m;
        if (this->type == MENU_ROMM)        m.action = BatchRommInstall;
        else if (this->type == MENU_MANAGE) m.action = BatchManage;
        else return false;
        this->queue.push(m);
        return true;
    }
    void Menu::pageDown() {
        if (this->entries.size()==0) return;
        if (this->entries.size() <= MAX_ENTRY_COUNT || this->top+MAX_ENTRY_COUNT == this->entries.end()) {

            this->selection=this->entries.end()-1;

        } else {

            for (int i=0;i < MAX_ENTRY_COUNT && this->top+MAX_ENTRY_COUNT != this->entries.end(); i++) {
                this->top++;
                this->selection++;
            }
        }
     }
    void Menu::pageUp() {
        if (this->entries.size()==0) return;
        if (this->top==this->entries.begin()) {
            this->selection=this->entries.begin();
        }else{
            for (int i=0;i<MAX_ENTRY_COUNT && this->top!=this->entries.begin();i++) {
                this->top--;
                this->selection--;
            }
        }
    }
    Menu* Menu::back() {
        rlog.info("back: type=" + std::to_string((int)this->type) + " filter='" + this->filter +
                  "' cross=" + (this->crossSystem?"1":"0"));
        switch (this->type) {
            case MENU_MAIN:
                return this;
            case MENU_ROMM:
                // cross-system search is search-first (no browse-all state): B -> system select
                if (this->crossSystem)
                    return generateSystemMenu(this);
                if (!this->filter.empty()) {           // platform search: clear the filter first
                    std::string slug = this->platformSlug;
                    return buildRommMenu(this, "", gCache[slug], slug, false);
                }
                return generateSystemMenu(this);       // full platform list -> system select
            case MENU_SYSTEMS:
                return generateMainMenu(this);
            case MENU_MANAGE:
                return generateManageSystemMenu(this);   // back to NDS/3DS pick
            case MENU_SETTINGS:
                return generateMainMenu(this);
            case MENU_SERVER:
                if (gConfigPtr) return generateSettingsMenu(this, gConfigPtr);
                return generateMainMenu(this);
            case MENU_LOCAL: {
                // Browse SD Card: B walks up one folder; at the SD root it
                // leaves to the main menu.
                std::string parent = browseParent(this->currentDirectory.generic_string());
                if (parent.empty()) return generateMainMenu(this);
                return generateLocalMenu(this, std::filesystem::path(parent));
            }
            default:
                return generateMainMenu(this);
        }
    }
    bool Menu::hasQueue() {
        return this->queue.size() > 0;
    }

    // TWL forwarder duplicate-title flow (item: random tid on demand): the
    // first build keeps the rom's own game code; when that title id is
    // already installed (rom hacks share the original's code), ask — install
    // as a new copy with a random title id (keeps both) or overwrite.
    // B keeps the ALREADY_EXISTS result, which the caller reports as-is.
    static ReturnResult* buildTwlResolvingDuplicate(Builder* builder, C3D_RenderTarget* target,
                                                    const std::string& romPath,
                                                    const std::string& showName,
                                                    const std::string& customTitle,
                                                    bool force) {
        ReturnResult* r = builder->buildCIA(romPath, false, customTitle, force);
        if (r != nullptr && r->code == ERROR_INSTALL_ALREADY_EXISTS) {
            int c = actionMenu(target, "Already installed", showName, {
                {"Install as new", "Give this copy a random title ID so both stay installed. For rom hacks that share the original's game code."},
                {"Overwrite", "Replace the installed game with this one."}});
            if (c >= 0) {
                delete r;
                r = builder->buildCIA(romPath, c == 0, customTitle, true);
            }
        }
        return r;
    }

    Menu* Menu::handleQueue(Builder* builder, C3D_RenderTarget* target, Config* config) {
        gConfigPtr = config;
        // collect a finished background library refresh (only between actions).
        // the worker already did the heavy SD work (json save, miss cleanup) —
        // here it's just a vector swap and, if visible, a menu rebuild.
        if (!this->hasQueue()) {
            std::string slug; std::vector<RommRom> fresh; bool ok = false, changed = false;
            if (libRefreshTake(slug, fresh, ok, changed)) {
                if (ok && changed) {
                    gCache[slug] = std::move(fresh);
                    rlog.info("background refresh applied: " + slug);
                }
                if (ok) gCacheOk[slug] = true;
                coverCacheStart(gRomm, gCache[slug]);   // covers waited on the refresh
                if (ok && changed && this->type == MENU_ROMM && !this->crossSystem &&
                    this->platformSlug == slug)
                    return buildRommMenu(this, this->filter, gCache[slug], slug, false);
            }
        }
        if (!this->hasQueue())
            return this;
        if (target==nullptr)
            target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
        while (queue.size() > 0) {
            MenuSelection entry = this->queue.front();
            switch (entry.action) {
                case OpenFolder:
                    while (this->queue.size() > 0) this->queue.pop();
                    // Browse SD Card navigates within itself (folders + ".." up)
                    return generateLocalMenu(this, entry.path);
                case OpenSDBrowser:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateLocalMenu(this, std::filesystem::path(BROWSE_ROOT));
                case OpenRommLibrary:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateSystemMenu(this);
                case OpenPlatform:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateRommMenu(this, target, entry.platformSlug);
                case OpenSearchAll:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateSearchAllMenu(this, target);   // prompts the query itself
                case RefreshLibraries:
                    while (this->queue.size() > 0) this->queue.pop();
                    invalidateAllCaches();     // wipe memory + on-SD json caches
                    coverCacheClearMisses();   // retry covers that were missing before
                    gFwdReady = false; invalidateYanbfCache();   // re-scan installed forwarders
                    Dialog(target,0,0,320,240,{"Library cache cleared.","Will reload from server."},{"OK"}).handle();
                    return generateSystemMenu(this);
                case OpenManage:
                    while (this->queue.size() > 0) this->queue.pop();
                    if (entry.platformSlug.empty()) {        // main entry -> system pick
                        showLoading(target, {"Opening Manage..."});
                        return generateManageSystemMenu(this);
                    }
                    showLoading(target, {std::string("Scanning ")+(entry.platformSlug==ROMM_SLUG_3DS?"3DS":entry.platformSlug==ROMM_SLUG_GBA?"GBA":"NDS")+" titles..."});
                    return generateManageMenu(this,config->dsiwareCount,entry.platformSlug,target);
                case OpenSettings:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateSettingsMenu(this, config);
                case SettingToggle: {
                    // rebuilds below put the cursor back on the toggled row
                    // instead of snapping to the top of the list
                    size_t selIdx = (size_t)(this->selection - this->entries.begin());
                    if (entry.rommId >= SETTING_SRV_HOST && entry.rommId <= SETTING_SRV_TEST) {
                        switch (entry.rommId) {
                            case SETTING_SRV_HOST:
                                if (gRomm.promptOne(0)) invalidateAllCaches();
                                break;
                            case SETTING_SRV_USER:
                                if (gRomm.promptOne(1)) invalidateAllCaches();
                                break;
                            case SETTING_SRV_PASS:
                                if (gRomm.promptOne(2)) invalidateAllCaches();
                                break;
                            case SETTING_SRV_TEST: {
                                Dialog(target,0,0,320,240,{"Testing...",gRomm.host},{},0).handle();
                                int pid = gRomm.findNdsPlatform();
                                if (pid >= 0)
                                    Dialog(target,0,0,320,240,{"Connected.","RomM is reachable."},{"OK"}).handle();
                                else
                                    Dialog(target,0,0,320,240,{"Connection failed",gRomm.lastError},{"OK"}).handle();
                                break;
                            }
                        }
                        while (this->queue.size() > 0) this->queue.pop();
                        {
                            Menu* m = generateServerMenu(this);
                            m->selectIndex(selIdx);
                            return m;
                        }
                    }
                    // multi-option settings open a vertical picker (same menu
                    // as everywhere else) instead of cycling values blind;
                    // plain on/off rows still just toggle
                    switch (entry.rommId) {
                        case SETTING_CUSTOM_TITLE: config->customTitle = !config->customTitle; break;
                        case SETTING_FORCE:        config->forceInstall = !config->forceInstall; break;
                        case SETTING_DELETE_SRC: {
                            int c = actionMenu(target, "After a Browse-SD install", "", {
                                {"Delete .cia source", "Reclaim the space: a .cia is a duplicate once the game is installed. DS/GBA game files are always kept - the game needs them to run and to change art."},
                                {"Keep source files", "Leave every file on the SD card after installing."}},
                                config->deleteAfterInstall ? 0 : 1);
                            if (c >= 0) config->deleteAfterInstall = (c == 0);
                            break;
                        }
                        case SETTING_ART_NOTIFY: {
                            int c = actionMenu(target, "Missing art at install", "", {
                                {"Notify", "Ask before falling back to the RomM cover, with the option to search by name."},
                                {"Silent fallback", "Use the RomM cover without asking; such games get a [!] mark in Manage."}},
                                config->artNotify ? 0 : 1);
                            if (c >= 0) config->artNotify = (c == 0);
                            break;
                        }
                        case SETTING_GBA_SCREEN: {
                            int sel = pickGbaScreenPreset(target, config, "Default for new GBA installs");
                            if (sel >= 0) config->gbaScreen = sel;
                            break;
                        }
                        case SETTING_MANAGE_ART: {
                            int c = actionMenu(target, "Manage art", "Art for installed games in Manage", {
                                {"Title icons", "Each game's own HOME icon - fast, always available, exactly what HOME shows."},
                                {"RomM covers", "Box art from the server, for games matched in the library."}},
                                config->manageIcons ? 0 : 1);
                            if (c >= 0) config->manageIcons = (c == 0);
                            break;
                        }
                        case SETTING_ART_CACHE: {
                            int cf; u64 cb;
                            artCacheStats(cf, cb);
                            if (cf == 0) {
                                Dialog(target,0,0,320,240,{"Art cache is empty."},{"OK"}).handle();
                                break;
                            }
                            if (Dialog(target,0,0,320,240,
                                       {"Clear art cache?",
                                        std::to_string(cf) + " files - " + humanSize(cb),
                                        "Covers re-download as you browse;","previews and icons rebuild.",
                                        "Your picked art (art.json) is kept."},
                                       {"Clear","Back"},1).handle()!=0) break;
                            showLoading(target, {"Clearing art cache..."});
                            int gone;
                            {
                                CoverCachePause pause;   // the worker owns httpc + the SD
                                gone = artCacheClear();
                            }
                            coverCacheClearMisses();     // missing covers retry next browse
                            installedTitlesInvalidate(); // drop the RAM icon cache too
                            Dialog(target,0,0,320,240,{"Cleared " + std::to_string(gone) + " files",
                                   humanSize(cb) + " freed"},{"OK"}).handle();
                            break;
                        }
                        case SETTING_SGDB_KEY: {
                            gSgdbKeyTried = false;   // re-read sgdb.env on demand
                            int c;
                            if (ensureSgdb())
                                c = Dialog(target,0,0,320,240,{"SteamGridDB key loaded.",SGDB_ENV_PATH},{"Enter key","OK"},1).handle();
                            else
                                c = Dialog(target,0,0,320,240,{"No SteamGridDB key found.","Enter it here, or put","STEAMGRIDDB_API_KEY=... in",SGDB_ENV_PATH},{"Enter key","OK"}).handle();
                            if (c == 0) {            // manual entry -> saved to sgdb.env
                                SwkbdState kb;
                                char buf[128] = {0};
                                swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 100);
                                swkbdSetHintText(&kb, "SteamGridDB API key");
                                swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
                                if (swkbdInputText(&kb, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
                                    std::string k = buf;
                                    size_t b = k.find_first_not_of(" \t\r\n");
                                    size_t e2 = k.find_last_not_of(" \t\r\n");
                                    k = (b == std::string::npos) ? "" : k.substr(b, e2 - b + 1);
                                    if (!k.empty()) {
                                        std::error_code ec;
                                        std::filesystem::create_directories("sdmc:/3ds/romm3ds", ec);
                                        FILE* f = fopen(SGDB_ENV_PATH, "wb");
                                        if (f) {
                                            fprintf(f, "STEAMGRIDDB_API_KEY=%s\n", k.c_str());
                                            fclose(f);
                                            gSgdbKeyTried = false;
                                            Dialog(target,0,0,320,240,{ensureSgdb()?"Key saved.":"Key saved but not readable?",SGDB_ENV_PATH},{"OK"}).handle();
                                        } else
                                            Dialog(target,0,0,320,240,{"Couldn't write",SGDB_ENV_PATH},{"OK"}).handle();
                                    }
                                }
                            }
                            break;
                        }
                        case SETTING_TEMPLATE: {
                            std::vector<MenuOpt> topts;
                            for (auto& t : config->templates) topts.push_back({t, ""});
                            int c = actionMenu(target, "Template", "Used by SD card installs",
                                               topts, (int)config->currentTemplate);
                            if (c >= 0) config->currentTemplate = c;
                            break;
                        }
                        case SETTING_SERVER:
                            while (this->queue.size() > 0) this->queue.pop();
                            return generateServerMenu(this);
                    }
                    config->save();
                    while (this->queue.size() > 0) this->queue.pop();
                    {
                        Menu* m = generateSettingsMenu(this, config);
                        m->selectIndex(selIdx);
                        return m;
                    }
                }
                case RommInstall: {
                    bool is3ds = (entry.platformSlug == ROMM_SLUG_3DS);
                    bool isGba = (entry.platformSlug == ROMM_SLUG_GBA);
                    rlog.info("install: " + entry.fsName + " slug=" + entry.platformSlug +
                              " fileId=" + std::to_string(entry.fileId) + " installable=" + (entry.installable?"1":"0"));
                    if (is3ds && entry.fileId == -1) {
                        // file list still unresolved (library was opened offline)
                        showLoading(target, {"Checking files...", entry.title});
                        hidScanInput();
                        if (hidKeysDown() & KEY_B) break;   // mistaken tap: nothing fetched yet
                        if (!settle3dsFilePick(entry)) {
                            Dialog(target,0,0,320,240,{"Can't read file list",gRomm.lastError},{"OK"}).handle();
                            break;
                        }
                    }
                    if (is3ds && !entry.installable) {
                        Dialog(target,0,0,320,240,{"Not a .cia — can't install here.",entry.fsName,"Convert on PC with ready3ds,","then upload the .cia to RomM."},{"OK"}).handle();
                        break;
                    }
                    std::string dir = rommDirFor(entry.platformSlug);
                    std::string dest = dir + entry.fsName;                         // download target (nds may be .zip)
                    std::string romPath = rommLocalPath(entry.fsName, entry.platformSlug); // playable file
                    std::string romBase = std::filesystem::path(romPath).filename().generic_string();
                    bool onSD = fileExists(is3ds ? dest : romPath);
                    bool needDownload = true;
                    bool pickArt = false;    // "+ Art"/"Change art": picker before the (re)install
                    int screenOverride = -1;   // gba: filter picked at install time
                    // installed state, per system — same checks as the list markers
                    u64 gbaTid = isGba ? gbaTidForRom(romBase) : 0;
                    bool inst = is3ds ? installed3dsHasTitle(entry.titleId)
                              : isGba ? installed3dsHasTitle(gbaTid)
                                      : ndsForwarderInstalled(entry.fsName);
                    // same title id as an installed game, but a DIFFERENT RomM
                    // entry: a rom hack that kept the original's id. Installing
                    // silently REPLACES the original (a .cia's id can't be
                    // changed on device — it keys the NCCH crypto), so warn —
                    // and don't offer the installed-game hub for it.
                    if (is3ds && inst &&
                        installed3dsRommIdForTitle(entry.titleId) != entry.rommId) {
                        if (Dialog(target,0,0,320,240,
                                   {"Same title ID already installed",
                                    "Rom hacks often keep the original","game's title ID. Installing this",
                                    "REPLACES that game (they share","the save data)."},
                                   {"Install anyway","Cancel"},1).handle()!=0)
                            break;
                        inst = false;   // proceed as a fresh install
                    }
                    if (is3ds && inst) {
                        // installed: the Manage actions live right here too
                        TitleExtras ex = findTitleExtras(entry.titleId, true);
                        // uninstall (+ update/DLC) first, reinstall last
                        std::vector<MenuOpt> mo; std::vector<int> ma;
                        addUninstall3dsOpts(mo, ma, entry.sizeBytes, ex);
                        mo.push_back({"Reinstall", onSD ? "Install again from the .cia on the SD card."
                                                        : "Download from RomM and install over the current copy."});
                        ma.push_back(-1);
                        if (onSD) {
                            mo.push_back({"Download again", "Fetch a fresh copy from RomM first, then install it."});
                            ma.push_back(-2);
                        }
                        int c = actionMenu(target, entry.title, "Installed", mo);
                        if (c < 0) break;
                        if (ma[c] >= 0) {   // one of the uninstall/extras rows
                            if (execUninstall3ds(target, entry.title, entry.titleId,
                                                 entry.sizeBytes, ex, ma[c]) && ma[c] == 0) {
                                for (auto e : this->entries)
                                    if (e->action==RommInstall && e->titleId==entry.titleId &&
                                        e->display.rfind("* ",0)==0)
                                        e->display = "  " + e->display.substr(2);
                                gDescForId = -1;   // refresh the INSTALLED chip
                            }
                            break;
                        }
                        needDownload = (ma[c] == -2) || !onSD;
                    } else if (onSD && is3ds) {
                        int c = actionMenu(target, "Already downloaded", entry.fsName, {
                            {"Install", "Install the .cia already on the SD card."},
                            {"Download again", "Fetch a fresh copy from RomM first, then install it."}});
                        if (c < 0) break;
                        needDownload = (c==1);
                    } else if (isGba && inst && onSD) {
                        // installed game: shared [Uninstall, Art & filter >, Reinstall]
                        GbaChoice gc = gbaInstalledMenu(target, entry.title, true);
                        if (gc == GBA_NONE) break;
                        if (gc == GBA_CHG_ART) {
                            changeArtGbaItem(target, config, romBase, entry.title, entry.coverPath, romPath, true, -1);
                            break;
                        }
                        if (gc == GBA_FILTER || gc == GBA_ART_FILTER) {
                            int fc = pickGbaScreenPreset(target, config, entry.title, artStoreGet(romBase).screen);
                            if (fc < 0) break;
                            if (gc == GBA_FILTER) applyGbaScreenItem(target, config, romBase, entry.title, entry.coverPath, romPath, true, fc);
                            else                  changeArtGbaItem(target, config, romBase, entry.title, entry.coverPath, romPath, true, fc);
                            break;
                        }
                        if (gc == GBA_UNINSTALL) {
                            if (Dialog(target,0,0,320,240,{"Uninstall game?",entry.title},
                                       {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                                break;
                            showLoading(target, {"Uninstalling...", entry.title});
                            Result drg = AM_DeleteTitle(MEDIATYPE_SD, gbaTid);
                            AM_DeleteTicket(gbaTid);
                            if (R_FAILED(drg)) {
                                Dialog(target,0,0,320,240,{"Uninstall failed",entry.title},{"OK"}).handle();
                                break;
                            }
                            std::error_code ecu;
                            std::filesystem::remove(romPath, ecu);   // single-pass: inject + ROM
                            installedTitlesInvalidate();
                            installed3dsRefresh();
                            Dialog(target,0,0,320,240,{"Uninstalled.",entry.title},{"OK"}).handle();
                            for (auto e : this->entries)
                                if (e->action==RommInstall && e->fsName==entry.fsName &&
                                    e->display.rfind("* ",0)==0)
                                    e->display = "  " + e->display.substr(2);
                            gDescForId = -1;
                            break;
                        }
                        needDownload = false;   // GBA_REINSTALL: art + filter reused
                    } else if (!is3ds && !isGba && inst) {
                        // forwarder on HOME: the Manage actions, plus reinstall.
                        // tids come from the manage scan (matched like the marker)
                        u64 ntid = 0, nytid = 0, nrtid = 0;
                        std::string nrompath = romPath;
                        for (auto& m : scanManagedRoms(ROMM_NDS_DIR)) {
                            if (normNds(m.display) != normNds(entry.fsName)) continue;
                            ntid = m.tid; nytid = m.yanbfTid; nrtid = m.rommTid;
                            nrompath = m.path;
                            break;
                        }
                        // uninstall-first, reinstall-last (ma codes unchanged)
                        std::vector<MenuOpt> mo = {{"Uninstall", "Uninstall and delete the game file."}};
                        std::vector<int> ma = {2};
                        if (nrtid && onSD) {
                            mo.push_back({"Change art", "Pick a new HOME banner; same slot, save kept."});
                            ma.push_back(1);
                        }
                        mo.push_back({"Reinstall", onSD ? "Install again with the art it already uses."
                                                        : "Download again and reinstall the game."});
                        ma.push_back(0);
                        int c = actionMenu(target, entry.title, "Installed", mo);
                        if (c < 0) break;
                        if (ma[c] == 1) {
                            std::string nbase = std::filesystem::path(nrompath).filename().generic_string();
                            changeArtNdsRommItem(target, config, nbase, entry.title,
                                                 entry.coverPath, nrompath, nrtid, true);
                            break;
                        }
                        if (ma[c] == 2) {
                            if (Dialog(target,0,0,320,240,{"Uninstall game?",entry.title},
                                       {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                                break;
                            showLoading(target, {"Uninstalling...", entry.title});
                            MenuSelection um;
                            um.platformSlug = ROMM_SLUG_NDS;
                            um.installed = (ntid != 0);
                            um.tid = ntid; um.ytid = nytid; um.rtid = nrtid;
                            um.path = std::filesystem::path(nrompath);
                            bool uok = uninstallManageItem(config, um);
                            gFwdReady = false; invalidateManagedRoms(); invalidateYanbfCache();
                            installedTitlesInvalidate();
                            if (!uok) {
                                Dialog(target,0,0,320,240,{"Uninstall failed",entry.title},{"OK"}).handle();
                                break;
                            }
                            Dialog(target,0,0,320,240,{"Uninstalled.",entry.title},{"OK"}).handle();
                            for (auto e : this->entries)
                                if (e->action==RommInstall && e->fsName==entry.fsName &&
                                    e->display.rfind("* ",0)==0)
                                    e->display = "  " + e->display.substr(2);
                            gDescForId = -1;
                            break;
                        }
                        needDownload = !onSD;
                    } else if (is3ds) {
                        // not installed, not on SD. A 3DS .cia carries its own
                        // icon - nothing to pick, so just confirm the install.
                        int c = actionMenu(target, "Install this game?",
                                           entry.fsName + "  (" + humanSize(entry.sizeBytes) + ")",
                                           {{"Install", "Download and install to the HOME menu."}});
                        if (c < 0) break;
                        needDownload = true;
                    } else if (onSD) {
                        int c = actionMenu(target, "Already on SD", entry.fsName, {
                            {"Install", "Install with the art it already uses."},
                            {"Install + choose art", "Open the art picker first, then install."}});
                        if (c < 0) break;
                        pickArt = (c==1);
                        needDownload = false;
                    } else if (isGba) {
                        int c = actionMenu(target, "Install this game?",
                                           entry.fsName + "  (" + humanSize(entry.sizeBytes) + ")", {
                            {"Install", "Automatic art and the default filter."},
                            {"Install + choose art", "Pick the HOME icon and banner before installing."},
                            {"Install + filter", "Pick the color filter for this install."},
                            {"Install + art + filter", "Customize both: art picker, then the preset."}});
                        if (c < 0) break;
                        pickArt = (c==1 || c==3);
                        if (c==2 || c==3) {
                            screenOverride = pickGbaScreenPreset(target, config, entry.title,
                                                                 artStoreGet(entry.fsName).screen);
                            if (screenOverride < 0) break;
                        }
                    } else {
                        int c = actionMenu(target, "Install this game?",
                                           entry.fsName + "  (" + humanSize(entry.sizeBytes) + ")", {
                            {"Install", "Download and install the game with automatic art."},
                            {"Install + choose art", "Pick the HOME art first (the DS icon is the default)."}});
                        if (c < 0) break;
                        pickArt = (c==1);
                    }
                    if (!is3ds && !ensureCtrBuilder(target)) break;  // CIA shell template (nds fwd + gba inject)
                    // GBA art resolves BEFORE the long download — correcting a
                    // bad name is cheapest here (ART-UX-SPEC S2)
                    ArtEntry gbaArtEntry;
                    ArtPieces gbaArt;
                    if (isGba)
                        resolveGbaArtInteractive(target, config, entry.fsName, entry.title,
                                                 entry.coverPath, gbaArtEntry, gbaArt, pickArt);
                    InstallOutcome io = installOneRomm(target, config, entry, needDownload,
                                                       gbaArtEntry, gbaArt, pickArt, true,
                                                       screenOverride);
                    if (io.ok) {
                        Dialog(target,0,0,320,240,{"Installed!",entry.title},{"OK"}).handle();
                        for (auto e : this->entries) {
                            if (e->action==RommInstall && e->fsName==entry.fsName && e->display.rfind("* ",0)!=0)
                                e->display="* "+e->display.substr(2);
                        }
                    }
                    break;
                }
                case BatchRommInstall: {
                    // the selected installable rows, taken straight from the live menu
                    std::vector<MenuSelection*> items;
                    for (auto e : this->entries)
                        if (e->selected && e->action == RommInstall &&
                            !(e->platformSlug == ROMM_SLUG_3DS && !e->installable))
                            items.push_back(e);
                    if (items.empty()) break;
                    // split by installed state (same checks as the row markers):
                    // a selection with installed games first picks its SCOPE —
                    // install the new ones, reinstall everything, or uninstall
                    // the installed ones — instead of blindly reinstalling all
                    installed3dsRefresh();
                    refreshNdsForwarders();
                    auto rowInstalled = [](MenuSelection* e) -> bool {
                        if (e->platformSlug == ROMM_SLUG_3DS)
                            return installed3dsHasTitle(e->titleId);
                        if (e->platformSlug == ROMM_SLUG_GBA)
                            return installed3dsHasTitle(gbaTidForRom(std::filesystem::path(
                                rommLocalPath(e->fsName, e->platformSlug)).filename().generic_string()));
                        return ndsForwarderInstalled(e->fsName);
                    };
                    std::vector<MenuSelection*> instItems, newItems;
                    for (auto e : items)
                        (rowInstalled(e) ? instItems : newItems).push_back(e);
                    if (!instItems.empty()) {
                        std::string ssub = std::to_string((int)items.size()) + " selected - " +
                                           std::to_string((int)instItems.size()) + " installed";
                        std::vector<MenuOpt> so;
                        std::vector<int> sa;   // 0 install new, 1 reinstall all, 2 uninstall installed
                        if (!newItems.empty()) {
                            so.push_back({"Install new (" + std::to_string((int)newItems.size()) + ")",
                                          "Only the games that aren't installed yet; the " +
                                          std::to_string((int)instItems.size()) + " installed are skipped."});
                            sa.push_back(0);
                        }
                        so.push_back({newItems.empty()
                                          ? "Reinstall (" + std::to_string((int)instItems.size()) + ")"
                                          : "Install + reinstall all (" + std::to_string((int)items.size()) + ")",
                                      "Everything selected installs; the installed ones are reinstalled over."});
                        sa.push_back(1);
                        so.push_back({"Uninstall installed (" + std::to_string((int)instItems.size()) + ")",
                                      "Uninstall the installed ones - updates, DLC and game files go too."});
                        sa.push_back(2);
                        int sc = actionMenu(target, "Selected", ssub, so);
                        if (sc < 0) break;
                        if (sa[sc] == 0) items = newItems;
                        else if (sa[sc] == 2) {
                            // batch uninstall, manage-style, right from the library
                            int K = (int)instItems.size();
                            if (Dialog(target,0,0,320,240,
                                       {"Uninstall " + std::to_string(K) + " games?",
                                        "Updates, DLC and game files","go with their ROM files."},
                                       {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                                break;
                            // NDS forwarder tids come from the manage scan
                            std::map<std::string, ManagedRom> ndsBy;
                            for (auto e : instItems) {
                                if (e->platformSlug == ROMM_SLUG_3DS || e->platformSlug == ROMM_SLUG_GBA) continue;
                                for (auto& m : scanManagedRoms(ROMM_NDS_DIR))
                                    ndsBy[normNds(m.display)] = m;
                                break;
                            }
                            int okCount = 0;
                            std::vector<std::string> failed;
                            for (int i = 0; i < K; i++) {
                                MenuSelection* it = instItems[i];
                                showLoading(target, {"Uninstalling " + std::to_string(i+1) + "/" + std::to_string(K), it->title});
                                MenuSelection um;
                                um.platformSlug = it->platformSlug;
                                std::string lp = rommLocalPath(it->fsName, it->platformSlug);
                                if (it->platformSlug == ROMM_SLUG_3DS) {
                                    um.tid = it->titleId;
                                } else if (it->platformSlug == ROMM_SLUG_GBA) {
                                    um.tid = gbaTidForRom(std::filesystem::path(lp).filename().generic_string());
                                    um.installed = true;
                                    um.path = std::filesystem::path(lp);
                                } else {
                                    auto hit = ndsBy.find(normNds(it->fsName));
                                    if (hit == ndsBy.end()) { failed.push_back(it->title); continue; }
                                    um.tid = hit->second.tid;
                                    um.ytid = hit->second.yanbfTid;
                                    um.rtid = hit->second.rommTid;
                                    um.installed = hit->second.installed;
                                    um.path = std::filesystem::path(hit->second.path);
                                }
                                if (uninstallManageItem(config, um)) okCount++;
                                else failed.push_back(it->title);
                            }
                            gFwdReady = false; invalidateManagedRoms(); invalidateYanbfCache();
                            installedTitlesInvalidate();
                            installed3dsRefresh();
                            std::vector<std::string> msg;
                            msg.push_back("Uninstalled " + std::to_string(okCount) + " of " + std::to_string(K));
                            int shownU = 0;
                            for (auto& f : failed) { if (shownU++ >= 3) break; msg.push_back(f); }
                            if ((int)failed.size() > 3)
                                msg.push_back("...and " + std::to_string((int)failed.size()-3) + " more");
                            Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                            while (this->queue.size() > 0) this->queue.pop();
                            const std::vector<RommRom>& usrc = this->crossSystem ? gCombined : gCache[this->platformSlug];
                            return buildRommMenu(this, this->filter, usrc, this->platformSlug, this->crossSystem);
                        }
                        // sa[sc] == 1: keep every selected item (reinstall over)
                    }
                    int M = (int)items.size();
                    u64 total = 0; for (auto e : items) total += e->sizeBytes;
                    bool anyGba = false, anyCtr = false;
                    for (auto e : items) {
                        if (e->platformSlug == ROMM_SLUG_GBA) anyGba = true;
                        if (e->platformSlug != ROMM_SLUG_3DS) anyCtr = true;
                    }
                    // same customization depth as a single install (menu IS the confirm)
                    bool pickArtAll = false;
                    int batchScreen = -1;
                    std::vector<MenuOpt> bopts = {
                        {"Install " + std::to_string(M),
                         anyCtr ? "Automatic art for each." : "Install to the HOME menu."}};
                    if (anyGba) {
                        bopts.push_back({"Install + choose art", "Art picker for each GBA game first, then everything installs unattended."});
                        bopts.push_back({"Install + filter", "One color filter for every game installed here."});
                        bopts.push_back({"Install + art + filter", "Art picker per game, one preset for all."});
                    }
                    int bc = actionMenu(target, "Install selected",
                                        std::to_string(M) + " games - " + humanSize(total) + " to download",
                                        bopts);
                    if (bc < 0) break;
                    pickArtAll = (bc == 1 || bc == 3);
                    if (bc == 2 || bc == 3) {
                        batchScreen = pickGbaScreenPreset(target, config, std::to_string(M) + " games");
                        if (batchScreen < 0) break;
                    }
                    if (anyCtr && !ensureCtrBuilder(target)) break;   // shared CIA shell template
                    // PHASE 1 (interactive): resolve GBA art up front — correcting a
                    // bad name is cheapest before the long downloads. 3DS/NDS need
                    // nothing here (NDS art resolves silently at build time — its
                    // sources key on the ROM's gamecode, only known post-download).
                    std::vector<ArtEntry> artEntries(M);
                    std::vector<ArtPieces> arts(M);
                    for (int i = 0; i < M; i++) {
                        if (items[i]->platformSlug != ROMM_SLUG_GBA) continue;
                        showLoading(target, {"Art " + std::to_string(i+1) + "/" + std::to_string(M), items[i]->title});
                        resolveGbaArtInteractive(target, config, items[i]->fsName, items[i]->title,
                                                 items[i]->coverPath, artEntries[i], arts[i], pickArtAll);
                    }
                    // PHASE 2 (unattended): download -> extract -> install/inject/build
                    // for each item. B cancels the current item and stops the rest;
                    // failures don't stop the run.
                    int okCount = 0;
                    u64 okBytes = 0;
                    std::vector<std::string> failed;
                    bool stopped = false;
                    for (int i = 0; i < M; i++) {
                        MenuSelection* it = items[i];
                        showLoading(target, {"Installing " + std::to_string(i+1) + "/" + std::to_string(M), it->title});
                        bool is3ds = (it->platformSlug == ROMM_SLUG_3DS);
                        if (!settle3dsFilePick(*it) || (is3ds && !it->installable)) {
                            failed.push_back(it->title);   // no file list / no .cia in folder
                            continue;
                        }
                        std::string romPath = rommLocalPath(it->fsName, it->platformSlug);
                        std::string dest = rommDirFor(it->platformSlug) + it->fsName;
                        bool onSD = fileExists(is3ds ? dest : romPath);
                        InstallOutcome io = installOneRomm(target, config, *it, !onSD,
                                                           artEntries[i], arts[i], false, false,
                                                           batchScreen);
                        if (io.ok) {
                            okCount++;
                            okBytes += it->sizeBytes;
                            it->selected = false;
                            if (it->display.size() >= 2 && it->display.rfind("* ",0)!=0)
                                it->display = "* " + it->display.substr(2);
                        } else {
                            failed.push_back(it->title);
                        }
                        if (io.cancelled) { stopped = true; break; }
                    }
                    std::vector<std::string> msg;
                    msg.push_back("Installed " + std::to_string(okCount) + " of " + std::to_string(M) +
                                  (okBytes ? " - " + humanSize(okBytes) : "") +
                                  (stopped ? " (stopped)" : ""));
                    if (!failed.empty()) {
                        msg.push_back("Could not install:");
                        int shownFail = 0;
                        for (auto& f : failed) {          // Dialog word-wraps: full names
                            if (shownFail++ >= 3) break;
                            msg.push_back(f);
                        }
                        if ((int)failed.size() > 3)
                            msg.push_back("...and " + std::to_string((int)failed.size()-3) + " more");
                    }
                    Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                    // rebuild the library so fresh install markers show and marks clear
                    while (this->queue.size() > 0) this->queue.pop();
                    const std::vector<RommRom>& src = this->crossSystem ? gCombined : gCache[this->platformSlug];
                    return buildRommMenu(this, this->filter, src, this->platformSlug, this->crossSystem);
                }
                case CleanupCias: {
                    // installer files whose title is already on the console
                    std::vector<CiaFile> all = listCiaFiles();
                    std::vector<CiaFile> done;
                    u64 doneBytes = 0, otherBytes = 0;
                    int otherCount = 0;
                    for (auto& c : all) {
                        if (c.installed) { done.push_back(c); doneBytes += c.sizeBytes; }
                        else { otherBytes += c.sizeBytes; otherCount++; }
                    }
                    if (done.empty()) {
                        Dialog(target,0,0,320,240,{"Nothing to reclaim.","No installer file here is already installed."},{"OK"}).handle();
                        break;
                    }
                    std::vector<std::string> msg;
                    msg.push_back(std::to_string((int)done.size()) + " installed .cia files - " + humanSize(doneBytes));
                    for (size_t i = 0; i < done.size() && i < 4; i++)
                        msg.push_back(shorten(done[i].name, 34));
                    if (done.size() > 4)
                        msg.push_back("... and " + std::to_string((int)done.size() - 4) + " more");
                    msg.push_back("The games stay installed. Only these files are deleted.");
                    if (otherCount > 0)
                        msg.push_back(std::to_string(otherCount) + " not-installed files (" + humanSize(otherBytes) + ") are kept.");
                    if (Dialog(target,0,0,320,240, msg, {"Delete","Back"}, 1).handle() != 0) break;
                    int okCount = 0;
                    u64 freed = 0;
                    std::error_code ec;
                    for (size_t i = 0; i < done.size(); i++) {
                        showLoading(target, {"Deleting " + std::to_string(i+1) + "/" + std::to_string(done.size()),
                                             done[i].name});
                        if (std::filesystem::remove(done[i].path, ec)) { okCount++; freed += done[i].sizeBytes; }
                    }
                    installedTitlesInvalidate();
                    Dialog(target,0,0,320,240,{"Deleted " + std::to_string(okCount) + " files",
                                               humanSize(freed) + " reclaimed"},{"OK"}).handle();
                    while (this->queue.size() > 0) this->queue.pop();
                    showLoading(target, {"Refreshing..."});
                    // Duplicates lives on the Manage picker now -> rebuild that
                    return generateManageSystemMenu(this);
                }
                case ManageZip: {
                    // interrupted download: finish it (extract + install, same
                    // customization depth as any other install) or drop it
                    bool zGba = (entry.platformSlug == ROMM_SLUG_GBA);
                    std::vector<MenuOpt> zo = {
                        {"Extract + install", zGba ? "Unzip, then install the game with automatic art and the saved filter."
                                                   : "Unzip, then install the game with automatic art."},
                        {"Extract + choose art", zGba ? "Art picker first, then the install."
                                                      : "Pick the HOME art first (the DS icon is the default)."}};
                    if (zGba) {
                        zo.push_back({"Extract + filter", "Pick the color filter for this install."});
                        zo.push_back({"Extract + art + filter", "Customize both: art picker, then the preset."});
                    }
                    zo.push_back({"Delete archive", "Remove the .zip from the SD card."});
                    int c = actionMenu(target, entry.title,
                                       "Archive - not extracted (" + humanSize(entry.sizeBytes) + ")", zo);
                    if (c < 0) break;
                    bool zPickArt = (c == 1 || (zGba && c == 3));
                    int zScreen = -1;
                    if (zGba && (c == 2 || c == 3)) {
                        zScreen = pickGbaScreenPreset(target, config, entry.title);
                        if (zScreen < 0) break;
                    }
                    if (c == (int)zo.size() - 1) {
                        if (Dialog(target,0,0,320,240,{"Delete archive?",entry.title,humanSize(entry.sizeBytes)},
                                   {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0) break;
                        std::error_code ec;
                        std::filesystem::remove(entry.path, ec);
                        while (this->queue.size() > 0) this->queue.pop();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                    }
                    std::string zdir = rommDirFor(entry.platformSlug);
                    std::string wantName = entry.path.stem().generic_string() + (zGba ? ".gba" : ".nds");
                    Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",entry.title},{},0).handle();
                    std::string extracted, zerr;
                    u64 lastZ = 0;
                    bool zok = extractFirstRom(entry.path.generic_string(), zdir,
                                               zipRomExtsFor(entry.platformSlug), extracted, zerr,
                        [&](unsigned long long done, unsigned long long total) -> bool {
                            hidScanInput();
                            if (hidKeysDown() & KEY_B) return false;
                            if (done - lastZ < (2<<20) && done != total) return true;
                            lastZ = done;
                            int pct = (total>0)?(int)(done*100/total):0;
                            Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",entry.title,std::to_string(pct)+"%"},{},0).handle();
                            return true;
                        }, wantName);
                    if (!zok) {
                        // the archive stays: a bad/cancelled extract must be retryable
                        Dialog(target,0,0,320,240,{(zerr=="cancelled")?"Extract cancelled":"Extract failed",zerr},{"OK"}).handle();
                        break;
                    }
                    { std::error_code ec; std::filesystem::remove(entry.path, ec); }   // zip -> rom, single copy
                    bool ok = false;
                    if (ensureCtrBuilder(target)) {
                        if (zGba) {
                            ArtEntry ae; ArtPieces pieces;
                            resolveGbaArtInteractive(target, config, wantName, entry.title,
                                                     entry.coverPath, ae, pieces, zPickArt);
                            if (zScreen >= 0) ae.screen = zScreen;   // picked filter for this bake
                            ok = installGbaInject(target, config, extracted, entry.title,
                                                  wantName, ae, pieces);
                        } else {
                            ok = buildForwarderFor(target, config, extracted, entry.title, entry.coverPath, zPickArt);
                        }
                    }
                    if (ok) Dialog(target,0,0,320,240,{"Installed!",entry.title},{"OK"}).handle();
                    while (this->queue.size() > 0) this->queue.pop();
                    gFwdReady = false; invalidateManagedRoms(); invalidateYanbfCache();
                    installedTitlesInvalidate();
                    showLoading(target, {"Refreshing..."});
                    return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                }
                case ManageRom: {
                    if (entry.platformSlug == ROMM_SLUG_3DS) {   // installed 3DS title -> uninstall
                        std::string n3 = entry.title;
                        if (entry.protectedTitle) {   // never touch this app or a system title
                            Dialog(target,0,0,320,240,{n3,"System title.","This one can't be uninstalled."},{"OK"}).handle();
                            break;
                        }
                        // updates (0004000E) and DLC (0004008C) of this game stay
                        // behind unless we take them too. Default = uninstall
                        // everything; below it, granular rows for just the
                        // update, just the DLC, or both — game kept.
                        TitleExtras ex = findTitleExtras(entry.tid);
                        std::vector<MenuOpt> opts;
                        std::vector<int> what;
                        addUninstall3dsOpts(opts, what, entry.sizeBytes, ex);
                        int c = actionMenu(target, n3,
                                           "Installed - " + humanSize(entry.sizeBytes + ex.bytes) +
                                           (ex.empty() ? "" : " total"), opts);
                        if (c < 0) break;
                        if (!execUninstall3ds(target, n3, entry.tid, entry.sizeBytes, ex, what[c]))
                            break;
                        while (this->queue.size() > 0) this->queue.pop();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                    }
                    if (entry.platformSlug == ROMM_SLUG_GBA) {   // GBA rom on SD +/- installed inject
                        std::string ng = entry.title;
                        if (entry.installed) {
                            std::string romBase = entry.path.filename().generic_string();
                            std::string gp = entry.path.generic_string();
                            GbaChoice gc = gbaInstalledMenu(target, ng, true);
                            if (gc == GBA_NONE) break;
                            if (gc == GBA_CHG_ART) {
                                // rebuild in place: same TID keeps HOME slot + save
                                if (changeArtGbaItem(target, config, romBase, ng, entry.coverPath, gp, true, -1) == 0) break;
                                while (this->queue.size() > 0) this->queue.pop();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            }
                            if (gc == GBA_FILTER || gc == GBA_ART_FILTER) {
                                int fc = pickGbaScreenPreset(target, config, ng, artStoreGet(romBase).screen);
                                if (fc < 0) break;
                                if (gc == GBA_FILTER) applyGbaScreenItem(target, config, romBase, ng, entry.coverPath, gp, true, fc);
                                else if (changeArtGbaItem(target, config, romBase, ng, entry.coverPath, gp, true, fc) == 0) break;
                                while (this->queue.size() > 0) this->queue.pop();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            }
                            if (gc == GBA_REINSTALL) {
                                // re-bake in place with the art + filter it already uses
                                if (!ensureCtrBuilder(target)) break;
                                ArtEntry ae; ArtPieces pieces;
                                resolveGbaArtInteractive(target, config, romBase, ng, entry.coverPath, ae, pieces, false);
                                if (!installGbaInject(target, config, gp, ng, romBase, ae, pieces)) break;
                                while (this->queue.size() > 0) this->queue.pop();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            }
                            // GBA_UNINSTALL: remove the inject AND the ROM file
                            if (Dialog(target,0,0,320,240,{"Uninstall game?",ng},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0) break;
                            showLoading(target, {"Uninstalling...", ng});
                            Result dr = AM_DeleteTitle(MEDIATYPE_SD, entry.tid);
                            AM_DeleteTicket(entry.tid);
                            if (R_FAILED(dr)) {
                                Dialog(target,0,0,320,240,{"Uninstall failed",ng},{"OK"}).handle();
                            } else {
                                std::error_code ec;
                                std::filesystem::remove(entry.path, ec);
                                Dialog(target,0,0,320,240,{"Uninstalled.",ng},{"OK"}).handle();
                            }
                        } else {
                            int c = actionMenu(target, ng, "Not installed - ROM on SD", {
                                {"Install", "Install the game with automatic art and its saved filter."},
                                {"Install + choose art", "Open the art picker first, then install."},
                                {"Delete ROM", "Remove the ROM file from the SD card."}});
                            if (c < 0) break;
                            if (c==0 || c==1) {
                                bool pickArt = (c==1);   // "+ Art": picker first, like the library flow
                                if (!ensureCtrBuilder(target)) break;
                                std::string romBase = entry.path.filename().generic_string();
                                u64 gtid = gCtr.allocateGbaTID(romBase);
                                if (gtid == 0) { Dialog(target,0,0,320,240,{"No free install slots"},{"OK"}).handle(); break; }
                                ArtEntry gbaArtEntry;
                                ArtPieces gbaArt;
                                resolveGbaArtInteractive(target, config, romBase, ng,
                                                         entry.coverPath, gbaArtEntry, gbaArt, pickArt);
                                Dialog(target,0,0,320,240,{"Installing...",ng},{},0).handle();
                                u64 lastG = 0;
                                int mode = gbaScreenFor(gbaArtEntry, config);
                                ReturnResult* gr = gCtr.buildGbaCIA(entry.path.generic_string(), ng, gtid,
                                                                    gbaArt.icon48, gbaArt.bannerTex,
                                                                    mode,
                                    [&](u64 done, u64 total) -> bool {
                                        hidScanInput();
                                        if (hidKeysDown() & KEY_B) return false;
                                        if (done - lastG < (2<<20) && done != total) return true;
                                        lastG = done;
                                        int pct = (total>0)?(int)(done*100/total):0;
                                        Dialog(target,0,0,320,240,{"Installing... (B = cancel)",ng,std::to_string(pct)+"%"},{},0).handle();
                                        return true;
                                    });
                                if (gr->isSuccess()) {
                                    ArtEntry e2 = gbaArtEntry; e2.screen = mode;
                                    artStorePut(romBase, e2);
                                    Dialog(target,0,0,320,240,{"Installed!",ng},{"OK"}).handle();
                                } else Dialog(target,0,0,320,240,{(gr->message=="cancelled")?"Install cancelled":"Install failed",gr->message},{"OK"}).handle();
                                delete gr;
                            } else if (c==2) {
                                if (Dialog(target,0,0,320,240,{"Delete ROM file?",ng},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0) break;
                                showLoading(target, {"Deleting..."});
                                std::error_code ec;
                                std::filesystem::remove(entry.path, ec);
                            } else break;
                        }
                        while (this->queue.size() > 0) this->queue.pop();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                    }
                    std::string name = entry.path.filename().generic_string();
                    bool hasFwd = entry.rtid || entry.installed || entry.ytid;
                    if (!hasFwd) {
                        if (!entry.fwdCia.empty()) {
                            // an uninstalled forwarder .cia exists on SD: offer to install it directly
                            int c = actionMenu(target, name, "Not installed - a ready .cia is on your SD", {
                                {"Install", "Install the .cia already on the card."},
                                {"Rebuild", "Install fresh (new art) instead of using the .cia."},
                                {"Delete ROM", "Remove the ROM file from the SD card."}});
                            if (c==0) {
                                showLoading(target, {"Installing..."});
                                std::string ierr;
                                bool ok = installCiaFromFile(entry.fwdCia, ierr, true, nullptr);
                                if (!ok) {
                                    Dialog(target,0,0,320,240,{"Install failed",shorten(ierr,30)},{"OK"}).handle();
                                    break;
                                }
                                Dialog(target,0,0,320,240,{"Installed!",entry.title},{"OK"}).handle();
                                while (this->queue.size() > 0) this->queue.pop();
                                gFwdReady = false; invalidateYanbfCache();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            } else if (c==2) {
                                if (Dialog(target,0,0,320,240,{"Delete ROM file?",name},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                                    std::error_code ec;
                                    std::filesystem::remove(entry.path, ec);
                                    while (this->queue.size() > 0) this->queue.pop();
                                    gFwdReady = false; invalidateManagedRoms();
                                    showLoading(target, {"Refreshing..."});
                                    return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                                }
                                break;
                            } else if (c!=1) {
                                break;
                            }
                            // c==1 falls through to the regular build flow below
                        }
                        // no forwarder yet: offer to build one
                        int c = entry.fwdCia.empty()
                            ? actionMenu(target, name, "Not installed - ROM on SD", {
                                  {"Install", "Install the game with automatic art."},
                                  {"Install + choose art", "Pick the HOME art first (the DS icon is the default)."},
                                  {"Delete ROM", "Remove the ROM file from the SD card."}})
                            : 0;
                        if (c < 0) break;
                        if (c==0 || c==1) {
                            if (config->dsiwareCount >= MAX_DSIWARE) {
                                Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                                break;
                            }
                            if (buildForwarderFor(target, config, entry.path.generic_string(), entry.title, entry.coverPath, c==1))
                                Dialog(target,0,0,320,240,{"Installed!",entry.title},{"OK"}).handle();
                            while (this->queue.size() > 0) this->queue.pop();
                            gFwdReady = false; invalidateYanbfCache();
                            showLoading(target, {"Refreshing..."});
                            return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                        } else if (c==2) {
                            if (Dialog(target,0,0,320,240,{"Delete ROM file?",name},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                                std::error_code ec;
                                std::filesystem::remove(entry.path, ec);
                                while (this->queue.size() > 0) this->queue.pop();
                                gFwdReady = false; invalidateManagedRoms();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            }
                        }
                        break;
                    }
                    std::string fwdState = "Installed";
                    if (artStoreGet(name).weak) fwdState += " - using fallback art";
                    // Change art only for our own forwarders (romm3ds TID range)
                    if (entry.rtid) {
                        int m = actionMenu(target, name, fwdState, {
                            {"Uninstall", "Uninstall and delete the game file."},
                            {"Change art", "Pick a new HOME banner; same slot, save kept."}});
                        if (m < 0) break;
                        int c = (m == 1) ? 0 : 1;   // old order: art / uninstall
                        if (c==0) {
                            // picker (banner page only — the icon stays the DS
                            // icon), then rebuild in place: same TID
                            int rc = changeArtNdsRommItem(target, config, name, entry.title,
                                                          entry.coverPath, entry.path.generic_string(),
                                                          entry.rtid, true);
                            if (rc == 0) break;   // picker cancelled — stay put
                            while (this->queue.size() > 0) this->queue.pop();
                            gFwdReady = false; invalidateYanbfCache();
                            showLoading(target, {"Refreshing..."});
                            return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                        }
                        if (c!=1) break;
                    } else {
                        // single-pass: uninstall removes the forwarder AND the ROM file
                        if (actionMenu(target, name, fwdState, {
                                {"Uninstall", "Uninstall and delete the game file."}}) < 0)
                            break;
                    }
                    bool delFwd = true;
                    bool delRom = true;
                    if (Dialog(target,0,0,320,240,{"Uninstall game?",name},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                        break;
                    showLoading(target, {"Uninstalling...", name});
                    bool err=false;
                    if (delFwd && entry.installed && entry.tid!=0) {
                        if (R_FAILED(deleteForwarder(entry.tid))) {
                            Dialog(target,0,0,320,240,{"Uninstall failed"},{"OK"}).handle();
                            err=true;
                        } else if (config->dsiwareCount>0) {
                            config->dsiwareCount--;
                        }
                    }
                    if (delFwd && entry.ytid!=0) {
                        if (R_FAILED(deleteYanbfForwarder(entry.ytid))) {
                            Dialog(target,0,0,320,240,{"Uninstall failed"},{"OK"}).handle();
                            err=true;
                        }
                    }
                    if (delFwd && entry.rtid!=0) {
                        if (R_FAILED(deleteRommCtrForwarder(entry.rtid))) {
                            Dialog(target,0,0,320,240,{"Uninstall failed"},{"OK"}).handle();
                            err=true;
                        }
                    }
                    if (delRom && !err) {
                        std::error_code ec;
                        if (!std::filesystem::remove(entry.path, ec)) {
                            Dialog(target,0,0,320,240,{"Uninstall failed"},{"OK"}).handle();
                            err=true;
                        }
                    }
                    if (!err)
                        Dialog(target,0,0,320,240,{"Uninstalled.",name},{"OK"}).handle();
                    while (this->queue.size() > 0) this->queue.pop();
                    gFwdReady = false; invalidateYanbfCache();
                    showLoading(target, {"Refreshing..."});
                    return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                }
                case LocalInstallSelected:
                case LocalInstallAll: {
                    // Batch: PHASE 1 resolves GBA art (with its prompts), then
                    // PHASE 2 installs each item unattended. "All" skips titles
                    // already installed; "Selected" takes exactly the Y-marks.
                    bool all = (entry.action == LocalInstallAll);
                    std::vector<MenuSelection*> items;
                    for (auto e : this->entries) {
                        if (e->action != LocalInstall) continue;
                        if (all ? !e->installed : e->selected) items.push_back(e);
                    }
                    if (items.empty()) {
                        if (all) Dialog(target,0,0,320,240,{"Nothing to install.","Everything here is already installed."},{"OK"}).handle();
                        else     Dialog(target,0,0,320,240,{"Nothing selected.","Press Y to mark games first."},{"OK"}).handle();
                        break;
                    }
                    // coherent with the RomM / Manage batch: a vertical scope
                    // menu when the selection includes already-installed games;
                    // an all-new selection installs straight away (like RomM).
                    {
                        std::vector<MenuSelection*> newItems, instItems;
                        for (auto it : items) (it->installed ? instItems : newItems).push_back(it);
                        if (!instItems.empty()) {
                            std::string ssub = std::to_string((int)items.size()) + " selected - " +
                                               std::to_string((int)instItems.size()) + " installed";
                            std::vector<MenuOpt> so; std::vector<int> sa;
                            if (!newItems.empty()) {
                                so.push_back({"Install new (" + std::to_string((int)newItems.size()) + ")",
                                              "Only the games not installed yet; the " +
                                              std::to_string((int)instItems.size()) + " installed are skipped."});
                                sa.push_back(0);
                            }
                            so.push_back({newItems.empty()
                                              ? "Reinstall (" + std::to_string((int)instItems.size()) + ")"
                                              : "Install + reinstall all (" + std::to_string((int)items.size()) + ")",
                                          "Everything selected installs; the installed ones are reinstalled over."});
                            sa.push_back(1);
                            so.push_back({"Uninstall installed (" + std::to_string((int)instItems.size()) + ")",
                                          "Uninstall those games and delete their files."});
                            sa.push_back(2);
                            int sc = actionMenu(target, "Selected", ssub, so);
                            if (sc < 0) break;
                            if (sa[sc] == 0) items = newItems;
                            else if (sa[sc] == 2) {   // batch uninstall the installed ones
                                int K = (int)instItems.size();
                                if (Dialog(target,0,0,320,240,{"Uninstall " + std::to_string(K) + " games?",
                                           "Their game files are deleted too."},
                                           {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                                    break;
                                int okU = 0;
                                std::vector<ManagedRom> ndsScan; bool ndsScanned = false;
                                for (int i = 0; i < K; i++) {
                                    MenuSelection* it = instItems[i];
                                    showLoading(target, {"Uninstalling " + std::to_string(i+1) + "/" + std::to_string(K), it->title});
                                    if (it->platformSlug == ROMM_SLUG_3DS) {
                                        u64 tid = it->titleId ? it->titleId : ciaFileTitleId(it->path.generic_string());
                                        TitleExtras ex = findTitleExtras(tid, true);
                                        if (execUninstall3ds(target, it->title, tid, it->sizeBytes, ex, 0)) okU++;
                                    } else if (it->platformSlug == ROMM_SLUG_GBA) {
                                        u64 gtid = it->tid ? it->tid : gbaTidForRom(it->fsName);
                                        if (R_SUCCEEDED(AM_DeleteTitle(MEDIATYPE_SD, gtid))) {
                                            AM_DeleteTicket(gtid);
                                            std::error_code ec; std::filesystem::remove(it->path, ec); okU++;
                                        }
                                    } else {
                                        if (!ndsScanned) { ndsScan = scanManagedRoms(ROMM_NDS_DIR); ndsScanned = true; }
                                        u64 ntid=0,nytid=0,nrtid=0; std::string np = it->path.generic_string();
                                        for (auto& m : ndsScan) if (normNds(m.display)==normNds(it->fsName)) {
                                            ntid=m.tid; nytid=m.yanbfTid; nrtid=m.rommTid; np=m.path; break; }
                                        MenuSelection um; um.platformSlug=ROMM_SLUG_NDS; um.installed=(ntid!=0);
                                        um.tid=ntid; um.ytid=nytid; um.rtid=nrtid; um.path=std::filesystem::path(np);
                                        if (uninstallManageItem(config, um)) okU++;
                                    }
                                }
                                installedTitlesInvalidate(); installed3dsRefresh();
                                gFwdReady=false; invalidateManagedRoms(); invalidateYanbfCache();
                                Dialog(target,0,0,320,240,{"Uninstalled " + std::to_string(okU) + " of " + std::to_string(K)},{"OK"}).handle();
                                while (this->queue.size() > 0) this->queue.pop();
                                return generateLocalMenu(this, this->currentDirectory);
                            }
                        }
                    }
                    // GBA batch art/screen — same choice as the single-item and
                    // RomM-batch flows (was missing here: GBA always got auto art).
                    bool pickArtAll = false;
                    int  batchScreen = -1;
                    {
                        int anyGba = 0;
                        for (auto it : items) if (it->platformSlug == ROMM_SLUG_GBA) anyGba++;
                        if (anyGba) {
                            int bc = actionMenu(target, "Install selected",
                                std::to_string(anyGba) + " GBA game(s)", {
                                {"Install", "Automatic art and the default filter for each."},
                                {"Install + choose art", "Pick the HOME icon and banner for each game."},
                                {"Install + filter", "One color filter for every game."},
                                {"Install + art + filter", "Pick the preset, then art for each game."}});
                            if (bc < 0) break;
                            pickArtAll = (bc == 1 || bc == 3);
                            if (bc == 2 || bc == 3) {
                                batchScreen = pickGbaScreenPreset(target, config, "Filter for selected", -1);
                                if (batchScreen < 0) break;
                            }
                        }
                    }
                    bool needCtr = false;
                    for (auto it : items) if (it->platformSlug != ROMM_SLUG_3DS) needCtr = true;
                    if (needCtr && !ensureCtrBuilder(target)) break;
                    CoverCachePause coverPause;   // own the network across the batch

                    // PHASE 1: art (GBA only — NDS resolves inline at build, CIA none)
                    std::vector<ArtEntry> aes(items.size());
                    std::vector<ArtPieces> pcs(items.size());
                    int gbaTotal = 0;
                    for (auto it : items) if (it->platformSlug == ROMM_SLUG_GBA) gbaTotal++;
                    if (gbaTotal) ensureSgdb();
                    int gbaN = 0;
                    for (size_t i = 0; i < items.size(); i++) {
                        MenuSelection* it = items[i];
                        if (it->platformSlug != ROMM_SLUG_GBA) continue;
                        gbaN++;
                        showLoading(target, {"Art "+std::to_string(gbaN)+"/"+std::to_string(gbaTotal), it->title});
                        resolveGbaArtInteractive(target, config, it->fsName, it->title,
                                                 it->coverPath, aes[i], pcs[i], pickArtAll);
                        if (batchScreen >= 0) aes[i].screen = batchScreen;   // bake the chosen preset
                    }

                    // PHASE 2: unattended install; continue past failures, B cancels the rest
                    int okCount = 0;
                    std::vector<std::string> fails;
                    bool cancelled = false;
                    for (size_t i = 0; i < items.size(); i++) {
                        MenuSelection* it = items[i];
                        hidScanInput();
                        if (hidKeysDown() & KEY_B) { cancelled = true; break; }
                        std::string prog = "Installing "+std::to_string(i+1)+"/"+std::to_string(items.size());
                        showLoading(target, {prog, it->title});
                        std::string romPath = it->path.generic_string();
                        // .zip: extract the rom inside (next to the archive) first
                        bool zipItem = isZipName(it->path.filename().generic_string());
                        if (zipItem) {
                            bool consumed; std::string zerr;
                            std::string extracted = resolveLocalRom(target, romPath, it->platformSlug, consumed, zerr);
                            if (extracted.empty()) {
                                if (zerr == "cancelled") { cancelled = true; break; }
                                fails.push_back(it->title);
                                continue;
                            }
                            romPath = extracted;
                        }
                        bool ok = false;
                        if (it->platformSlug == ROMM_SLUG_3DS) {
                            std::string ierr; u64 lastI = 0;
                            ok = installCiaFromFile(romPath, ierr, config->forceInstall,
                                [&](unsigned long long done, unsigned long long total) -> bool {
                                    hidScanInput();
                                    if (hidKeysDown() & KEY_B) return false;
                                    if (done - lastI < (4<<20) && done != total) return true;
                                    lastI = done;
                                    int pct = (total>0)?(int)(done*100/total):0;
                                    Dialog(target,0,0,320,240,{prog+" (B = cancel)",it->title,std::to_string(pct)+"%"},{},0).handle();
                                    return true;
                                });
                            if (ok) { u64 tid = it->titleId ? it->titleId : ciaFileTitleId(romPath);
                                      if (tid && it->rommId > 0) installed3dsRecord(it->rommId, tid);
                                      // delete-after-install: reclaim the duplicate .cia
                                      // (romPath = the extracted/plain cia, not the zip)
                                      if (config->deleteAfterInstall) {
                                          std::error_code ec; std::filesystem::remove(std::filesystem::path(romPath), ec); } }
                            else if (ierr == "cancelled") cancelled = true;
                        } else if (it->platformSlug == ROMM_SLUG_GBA) {
                            bool wasCancel = false;
                            ok = installGbaInject(target, config, romPath, it->title, it->fsName,
                                                  aes[i], pcs[i], &wasCancel, true);
                            if (wasCancel) cancelled = true;
                        } else {
                            ok = buildForwarderFor(target, config, romPath, it->title, it->coverPath, false);
                            if (ok) { gFwdReady = false; invalidateManagedRoms(); }
                        }
                        // a zip's job was to carry the rom; drop the archive once installed
                        if (ok && zipItem) { std::error_code ec; std::filesystem::remove(it->path, ec); }
                        if (ok) okCount++;
                        else if (!cancelled) fails.push_back(it->title);
                        if (cancelled) break;
                    }

                    u64 okBytes = 0;
                    for (size_t i = 0; i < items.size(); i++)
                        if (items[i]->installed) okBytes += items[i]->sizeBytes;
                    std::string summary = "Installed "+std::to_string(okCount)+" of "+std::to_string(items.size())
                                        + (okBytes ? " - " + humanSize(okBytes) : "");
                    std::string sub = cancelled ? "Cancelled - remaining skipped."
                                    : fails.empty() ? "All installed." : "Could not install:";
                    std::string failsJoined;
                    for (size_t i = 0; i < fails.size(); i++) { if (i) failsJoined += ", "; failsJoined += fails[i]; }
                    Dialog(target,0,0,320,240,{summary, sub, failsJoined},{"OK"}).handle();
                    while (this->queue.size() > 0) this->queue.pop();
                    showLoading(target, {"Refreshing..."});
                    return generateLocalMenu(this);
                }
                case LocalInstall: {
                    // "Install from SD": a local file already on the SD card —
                    // no download. Reuses the RomM per-system install flows.
                    std::string slug = entry.platformSlug;
                    bool is3ds = (slug == ROMM_SLUG_3DS);
                    bool isGba = (slug == ROMM_SLUG_GBA);
                    std::string romPath = entry.path.generic_string();
                    std::string fname = entry.path.filename().generic_string();
                    std::string name = entry.title;
                    bool isZip = isZipName(fname);
                    std::string romBase = isZip ? entry.fsName : fname;   // predicted rom name for a zip
                    bool pickArt = false;
                    int  screenPreset = -1;   // GBA: preset baked at install
                    // Per-item choices use the same vertical actionMenu as the
                    // RomM / Manage screens. GBA/NDS in-place art & screen edits
                    // act and return; the rest fall through to (re)install.
                    if (entry.installed) {
                        if (is3ds) {
                            // parity with RomM/Manage: Uninstall (+ update/DLC
                            // rows) first, Reinstall last. Reuses the shared 3DS
                            // uninstall helpers.
                            u64 tid3 = entry.titleId ? entry.titleId : ciaFileTitleId(romPath);
                            TitleExtras ex = findTitleExtras(tid3, true);
                            std::vector<MenuOpt> mo; std::vector<int> ma;
                            addUninstall3dsOpts(mo, ma, entry.sizeBytes, ex);
                            mo.push_back({"Reinstall", "Install to the HOME menu again."}); ma.push_back(-1);
                            int c = actionMenu(target, name, "Installed", mo);
                            if (c < 0) break;
                            if (ma[c] >= 0) {   // an uninstall / extras row
                                if (execUninstall3ds(target, name, tid3, entry.sizeBytes, ex, ma[c])) {
                                    while (this->queue.size() > 0) this->queue.pop();
                                    return generateLocalMenu(this, this->currentDirectory);
                                }
                                break;
                            }
                            // ma[c] == -1: Reinstall -> fall through to the install
                        } else if (isGba && !isZip) {
                            // shared installed-GBA menu: [Uninstall, Art & filter >, Reinstall]
                            GbaChoice gc = gbaInstalledMenu(target, name, true);
                            if (gc == GBA_NONE) break;
                            if (gc == GBA_CHG_ART) { changeArtGbaItem(target, config, romBase, name, entry.coverPath, romPath, true, -1); break; }
                            if (gc == GBA_FILTER || gc == GBA_ART_FILTER) {
                                int fc = pickGbaScreenPreset(target, config, name, artStoreGet(romBase).screen);
                                if (fc < 0) break;
                                if (gc == GBA_FILTER) applyGbaScreenItem(target, config, romBase, name, entry.coverPath, romPath, true, fc);
                                else                  changeArtGbaItem(target, config, romBase, name, entry.coverPath, romPath, true, fc);
                                break;
                            }
                            if (gc == GBA_UNINSTALL) {
                                if (Dialog(target,0,0,320,240,{"Uninstall game?",name},
                                           {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                                    break;
                                showLoading(target, {"Uninstalling...", name});
                                u64 gtid = entry.tid ? entry.tid : gbaTidForRom(romBase);
                                Result drg = AM_DeleteTitle(MEDIATYPE_SD, gtid);
                                AM_DeleteTicket(gtid);
                                if (R_FAILED(drg)) {
                                    Dialog(target,0,0,320,240,{"Uninstall failed",name},{"OK"}).handle();
                                    break;
                                }
                                std::error_code ecu; std::filesystem::remove(entry.path, ecu);   // the .gba
                                installedTitlesInvalidate(); installed3dsRefresh();
                                Dialog(target,0,0,320,240,{"Uninstalled.",name},{"OK"}).handle();
                                while (this->queue.size() > 0) this->queue.pop();
                                return generateLocalMenu(this, this->currentDirectory);
                            }
                            // GBA_REINSTALL: art + filter reused -> fall through
                        } else {   // installed NDS (or an installed zip row: reinstall)
                            int c = actionMenu(target, name, "Installed", {
                                {"Uninstall", "Uninstall and delete the game file."},
                                {"Change art", "Pick a new HOME banner/icon, then reinstall."},
                                {"Reinstall", "Install the game again."}});
                            if (c < 0) break;
                            if (c == 0) {   // reuse the Manage uninstall (resolve tids by name)
                                if (Dialog(target,0,0,320,240,{"Uninstall game?",name},
                                           {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                                    break;
                                showLoading(target, {"Uninstalling...", name});
                                u64 ntid=0,nytid=0,nrtid=0; std::string nrompath = romPath;
                                for (auto& m : scanManagedRoms(ROMM_NDS_DIR)) {
                                    if (normNds(m.display) != normNds(entry.fsName)) continue;
                                    ntid=m.tid; nytid=m.yanbfTid; nrtid=m.rommTid; nrompath=m.path; break;
                                }
                                MenuSelection um;
                                um.platformSlug = ROMM_SLUG_NDS;
                                um.installed = (ntid != 0);
                                um.tid=ntid; um.ytid=nytid; um.rtid=nrtid;
                                um.path = std::filesystem::path(nrompath);
                                bool uok = uninstallManageItem(config, um);
                                gFwdReady = false; invalidateManagedRoms(); invalidateYanbfCache();
                                installedTitlesInvalidate();
                                Dialog(target,0,0,320,240,{uok?"Uninstalled.":"Uninstall failed",name},{"OK"}).handle();
                                while (this->queue.size() > 0) this->queue.pop();
                                return generateLocalMenu(this, this->currentDirectory);
                            }
                            pickArt = (c == 1);
                        }
                    } else if (is3ds) {
                        int c = actionMenu(target, name, humanSize(entry.sizeBytes),
                            {{"Install", "Install to the HOME menu."}});
                        if (c < 0) break;
                    } else if (isGba) {
                        int c = actionMenu(target, name, humanSize(entry.sizeBytes), {
                            {"Install", "Automatic art and the default filter."},
                            {"Install + choose art", "Pick the HOME icon and banner before installing."},
                            {"Install + filter", "Pick the color filter for this install."},
                            {"Install + art + filter", "Customize both: art picker, then the preset."}});
                        if (c < 0) break;
                        pickArt = (c == 1 || c == 3);
                        if (c == 2 || c == 3) {
                            screenPreset = pickGbaScreenPreset(target, config, name, artStoreGet(romBase).screen);
                            if (screenPreset < 0) break;
                        }
                    } else {   // NDS not installed
                        int c = actionMenu(target, name, humanSize(entry.sizeBytes), {
                            {"Install", "Install the game (the DS icon is the default art)."},
                            {"Install + choose art", "Pick the HOME art first."}});
                        if (c < 0) break;
                        pickArt = (c == 1);
                    }
                    if (!is3ds && !ensureCtrBuilder(target)) break;
                    // .zip: extract the rom inside (next to the archive) first,
                    // then install it like any other local file
                    if (isZip) {
                        bool consumed; std::string zerr;
                        std::string extracted = resolveLocalRom(target, romPath, slug, consumed, zerr);
                        if (extracted.empty()) {
                            Dialog(target,0,0,320,240,{(zerr=="cancelled")?"Extract cancelled":"Extract failed",zerr},{"OK"}).handle();
                            break;
                        }
                        romPath = extracted;
                        fname = std::filesystem::path(extracted).filename().generic_string();
                    }
                    bool installed = false;
                    if (is3ds) {
                        std::string ierr; u64 lastI = 0;
                        showLoading(target, {"Installing...", name});
                        installed = installCiaFromFile(romPath, ierr, config->forceInstall,
                            [&](unsigned long long done, unsigned long long total) -> bool {
                                hidScanInput();
                                if (hidKeysDown() & KEY_B) return false;
                                if (done - lastI < (4<<20) && done != total) return true;
                                lastI = done;
                                int pct = (total>0)?(int)(done*100/total):0;
                                Dialog(target,0,0,320,240,{"Installing... (B = cancel)",name,std::to_string(pct)+"%"},{},0).handle();
                                return true;
                            });
                        if (installed) {
                            u64 tid = entry.titleId ? entry.titleId : ciaFileTitleId(romPath);
                            if (tid && entry.rommId > 0) installed3dsRecord(entry.rommId, tid);
                        } else
                            Dialog(target,0,0,320,240,{(ierr=="cancelled")?"Install cancelled":"Install failed",ierr},{"OK"}).handle();
                    } else if (isGba) {
                        ArtEntry ae; ArtPieces pieces;
                        resolveGbaArtInteractive(target, config, fname, name, entry.coverPath, ae, pieces, pickArt);
                        if (screenPreset >= 0) ae.screen = screenPreset;   // bake the chosen preset
                        installed = installGbaInject(target, config, romPath, name, fname, ae, pieces);
                    } else {
                        installed = buildForwarderFor(target, config, romPath, name, entry.coverPath, pickArt);
                        if (installed) { gFwdReady = false; invalidateManagedRoms(); }
                    }
                    if (installed) {
                        // clean up sources. zip: always drop the archive (the
                        // extracted rom is the keeper). 3DS .cia: drop it too
                        // (title-DB duplicate) when the setting is on. NDS/GBA
                        // roms are kept — their forwarder/inject re-reads them
                        // to launch and to change art.
                        std::error_code ec;
                        bool zipGone = false, ciaGone = false;
                        if (isZip) zipGone = std::filesystem::remove(entry.path, ec);
                        if (is3ds && config->deleteAfterInstall)
                            ciaGone = std::filesystem::remove(std::filesystem::path(romPath), ec);
                        std::string note = ciaGone ? "Source .cia removed."
                                         : zipGone ? "Archive removed." : "";
                        Dialog(target,0,0,320,240,{"Installed!",name,note},{"OK"}).handle();
                        if (isZip || ciaGone) {
                            // files changed on disk: rescan this folder
                            while (this->queue.size() > 0) this->queue.pop();
                            return generateLocalMenu(this, this->currentDirectory);
                        }
                        for (auto e : this->entries)
                            if (e->action==LocalInstall && e->path==entry.path) {
                                e->installed = true;
                                if (e->display.rfind("* ",0)!=0) e->display = "* " + e->display.substr(2);
                            }
                    }
                    break;
                }
                case BatchManage: {
                    // the selected manage rows, straight from the live menu
                    // (zip rows batch too: Install extracts them first,
                    // Delete ROMs removes the archive file)
                    std::vector<MenuSelection*> items;
                    for (auto e : this->entries)
                        if (e->selected && (e->action == ManageRom || e->action == ManageZip))
                            items.push_back(e);
                    if (items.empty()) break;
                    int M = (int)items.size();
                    std::string slug = this->platformSlug;
                    bool is3ds = (slug == ROMM_SLUG_3DS);
                    // rebuildable = Change-art applies: GBA injects + romm3ds NDS forwarders
                    int rebuildable = 0;
                    for (auto e : items)
                        if ((slug == ROMM_SLUG_GBA && e->installed) || (slug == ROMM_SLUG_NDS && e->rtid))
                            rebuildable++;
                    // rows on SD without a title yet: these can be installed
                    int notInstalled = 0;
                    for (auto e : items)
                        if (!is3ds && !manageItemInstalled(slug, *e)) notInstalled++;
                    // action dialog: what's offered follows what's selected
                    enum { A_INSTALL, A_UNINSTALL, A_CHANGEART, A_SCREEN, A_ARTSCREEN, A_EXTRAS, A_BACK } act = A_BACK;
                    std::vector<u64> extrasSel;   // 3DS: update/DLC titles to strip (A_EXTRAS)
                    u64 extrasSelBytes = 0;
                    std::string bsub = std::to_string(M) + " selected";
                    if (is3ds) {
                        // updates/DLC across the selection: same granular rows
                        // as the single-item menu, applied to every selected
                        // game at once. Default stays "uninstall everything".
                        std::vector<u64> updT, dlcT;
                        u64 updB = 0, dlcB = 0, totalB = 0;
                        for (auto e : items) {
                            TitleExtras ex = findTitleExtras(e->tid, true);
                            totalB += e->sizeBytes + ex.bytes;
                            updT.insert(updT.end(), ex.updateTids.begin(), ex.updateTids.end());
                            dlcT.insert(dlcT.end(), ex.dlcTids.begin(), ex.dlcTids.end());
                            updB += ex.updateBytes;
                            dlcB += ex.dlcBytes;
                        }
                        std::vector<MenuOpt> mo = {
                            {"Uninstall selected", "Remove every selected game, updates and DLC included. Frees " +
                                                   humanSize(totalB) + "."}};
                        std::vector<int> ma = {A_UNINSTALL};
                        std::vector<std::pair<std::vector<u64>, u64>> mx = {{{}, 0}};
                        if (!updT.empty() && !dlcT.empty()) {
                            std::vector<u64> both = updT;
                            both.insert(both.end(), dlcT.begin(), dlcT.end());
                            mo.push_back({"Remove update + DLC", "Keep the games; strip every update and DLC. Frees " +
                                                                 humanSize(updB + dlcB) + "."});
                            ma.push_back(A_EXTRAS); mx.push_back({both, updB + dlcB});
                        }
                        if (!updT.empty()) {
                            mo.push_back({dlcT.empty() ? "Remove updates" : "Remove updates only",
                                          "Keep the games; delete " + std::to_string((int)updT.size()) +
                                          " update title(s). Frees " + humanSize(updB) + "."});
                            ma.push_back(A_EXTRAS); mx.push_back({updT, updB});
                        }
                        if (!dlcT.empty()) {
                            mo.push_back({updT.empty() ? "Remove DLC" : "Remove DLC only",
                                          "Keep the games; delete " + std::to_string((int)dlcT.size()) +
                                          " DLC title(s). Frees " + humanSize(dlcB) + "."});
                            ma.push_back(A_EXTRAS); mx.push_back({dlcT, dlcB});
                        }
                        int c = actionMenu(target, "Selected", bsub, mo);
                        if (c >= 0) {
                            act = (decltype(act))ma[c];
                            extrasSel = mx[c].first;
                            extrasSelBytes = mx[c].second;
                        }
                    } else if (notInstalled == M) {
                        std::string cnt = " (" + std::to_string(M) + ")";
                        int c = actionMenu(target, "Selected", bsub, {
                            {"Install" + cnt, "Art first, then every game installs unattended."},
                            {"Delete files" + cnt, "Remove the selected game files from the SD card."}});
                        act = (c==0) ? A_INSTALL : (c==1) ? A_UNINSTALL : A_BACK;
                    } else if (notInstalled > 0) {
                        // mixed selection (typical after R = select all): counts on
                        // every row, "M selected - K installed" subtitle (like RomM)
                        int nInst = M - notInstalled;
                        std::vector<MenuOpt> mo = {
                            {"Install (" + std::to_string(notInstalled) + ")", "Install the ones not installed yet."},
                            {"Uninstall (" + std::to_string(nInst) + ")", "Uninstall the installed ones and delete their files."}};
                        std::vector<int> ma = {A_INSTALL, A_UNINSTALL};
                        if (rebuildable > 0) {
                            std::string rc = " (" + std::to_string(rebuildable) + ")";
                            mo.push_back({"Change art" + rc, slug == ROMM_SLUG_GBA
                                ? "Art picker for each installed game, applied in place."
                                : "Banner picker for each installed game, applied in place."});
                            ma.push_back(A_CHANGEART);
                            if (slug == ROMM_SLUG_GBA) {
                                mo.push_back({"Filter" + rc, "Pick one filter and apply it to the installed games."});
                                ma.push_back(A_SCREEN);
                                mo.push_back({"Art + filter" + rc, "One filter for all, then art per game."});
                                ma.push_back(A_ARTSCREEN);
                            }
                        }
                        int c = actionMenu(target, "Selected",
                                           std::to_string(M) + " selected - " + std::to_string(nInst) + " installed", mo);
                        if (c >= 0) act = (decltype(act))ma[c];
                    } else if (rebuildable == 0) {
                        int c = actionMenu(target, "Selected", bsub, {
                            {"Uninstall" + std::string(" (") + std::to_string(M) + ")", "Uninstall and delete every selected game."}});
                        act = (c==0) ? A_UNINSTALL : A_BACK;
                    } else if (slug == ROMM_SLUG_GBA) {
                        std::string cnt = " (" + std::to_string(M) + ")";
                        int c = actionMenu(target, "Selected", bsub, {
                            {"Uninstall" + cnt, "Uninstall and delete the selected games."},
                            {"Change art" + cnt, "Art picker per game, applied in place."},
                            {"Filter" + cnt, "Pick one filter and apply it to all selected."},
                            {"Art + filter" + cnt, "One filter for all, then art per game."}});
                        act = (c==0) ? A_UNINSTALL : (c==1) ? A_CHANGEART : (c==2) ? A_SCREEN
                            : (c==3) ? A_ARTSCREEN : A_BACK;
                    } else {
                        std::string cnt = " (" + std::to_string(M) + ")";
                        int c = actionMenu(target, "Selected", bsub, {
                            {"Uninstall" + cnt, "Uninstall and delete the selected games."},
                            {"Change art" + cnt, "Banner picker per game, applied in place."}});
                        act = (c==0) ? A_UNINSTALL : (c==1) ? A_CHANGEART : A_BACK;
                    }
                    if (act == A_BACK) break;
                    if (act == A_EXTRAS) {
                        // strip the chosen update/DLC titles; the games stay
                        if (Dialog(target,0,0,320,240,
                                   {"Remove " + std::to_string((int)extrasSel.size()) + " update/DLC title(s)?",
                                    "Frees " + humanSize(extrasSelBytes),
                                    "The games stay installed."},
                                   {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                            break;
                        int okCount = 0;
                        for (size_t i = 0; i < extrasSel.size(); i++) {
                            showLoading(target, {"Removing " + std::to_string((int)i+1) + "/" +
                                                 std::to_string((int)extrasSel.size())});
                            if (R_SUCCEEDED(AM_DeleteTitle(MEDIATYPE_SD, extrasSel[i]))) okCount++;
                            AM_DeleteTicket(extrasSel[i]);
                        }
                        installedTitlesInvalidate();
                        Dialog(target,0,0,320,240,
                               {"Removed " + std::to_string(okCount) + " of " + std::to_string((int)extrasSel.size()),
                                humanSize(extrasSelBytes) + " freed - the games stay installed."},{"OK"}).handle();
                        while (this->queue.size() > 0) this->queue.pop();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,slug,target);
                    }
                    if (act == A_SCREEN) {
                        // pick a preset once, re-bake every selected inject with it
                        int fc = pickGbaScreenPreset(target, config, std::to_string(M)+" selected");
                        if (fc < 0) break;
                        int okCount = 0;
                        std::vector<std::string> failed;
                        CoverCachePause coverPause;
                        for (int i = 0; i < M; i++) {
                            MenuSelection* it = items[i];
                            if (!it->installed) continue;
                            showLoading(target, {"Filter "+std::to_string(i+1)+"/"+std::to_string(M), it->title});
                            std::string base = it->path.filename().generic_string();
                            if (applyGbaScreenItem(target, config, base, it->title, it->coverPath,
                                                   it->path.generic_string(), false, fc) == 1) okCount++;
                            else failed.push_back(it->title);
                        }
                        std::vector<std::string> msg;
                        msg.push_back("Filter applied to "+std::to_string(okCount)+" of "+std::to_string(M));
                        if (!failed.empty()) {
                            msg.push_back("Failed:");
                            int shown = 0;
                            for (auto& f : failed) { if (shown++ >= 3) break; msg.push_back(f); }
                        }
                        Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                        while (this->queue.size() > 0) this->queue.pop();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,slug,target);
                    }
                    if (act == A_INSTALL) {
                        if (!ensureCtrBuilder(target)) break;
                        std::vector<MenuSelection*> todo;
                        for (auto e : items)
                            if (!manageItemInstalled(slug, *e)) todo.push_back(e);
                        CoverCachePause coverPause;
                        std::vector<std::string> failed;
                        // PHASE 0: finish interrupted downloads — extract zip
                        // rows in place so the art/install phases see a ROM
                        for (size_t zi = 0; zi < todo.size(); ) {
                            MenuSelection* it = todo[zi];
                            if (it->action != ManageZip) { zi++; continue; }
                            std::string wantName = it->path.stem().generic_string() +
                                                   (slug == ROMM_SLUG_GBA ? ".gba" : ".nds");
                            Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",it->title},{},0).handle();
                            std::string extracted, zerr;
                            u64 lastZ = 0;
                            bool zok = extractFirstRom(it->path.generic_string(), rommDirFor(slug),
                                                       zipRomExtsFor(slug), extracted, zerr,
                                [&](unsigned long long done, unsigned long long total) -> bool {
                                    hidScanInput();
                                    if (hidKeysDown() & KEY_B) return false;
                                    if (done - lastZ < (2<<20) && done != total) return true;
                                    lastZ = done;
                                    int pct = (total>0)?(int)(done*100/total):0;
                                    Dialog(target,0,0,320,240,{"Extracting... (B = cancel)",it->title,std::to_string(pct)+"%"},{},0).handle();
                                    return true;
                                }, wantName);
                            if (!zok) {   // keep the archive for a retry
                                failed.push_back(it->title);
                                todo.erase(todo.begin() + zi);
                                continue;
                            }
                            std::error_code ec;
                            std::filesystem::remove(it->path, ec);   // zip -> rom, single copy
                            it->path = std::filesystem::path(extracted);
                            it->action = ManageRom;   // menu rebuilds after the batch anyway
                            zi++;
                        }
                        int N = (int)todo.size();
                        // PHASE 1: GBA art up front (prompts here, not mid-install)
                        std::vector<ArtEntry> aes(N);
                        std::vector<ArtPieces> pcs(N);
                        if (slug == ROMM_SLUG_GBA) {
                            ensureSgdb();
                            for (int i = 0; i < N; i++) {
                                showLoading(target, {"Art "+std::to_string(i+1)+"/"+std::to_string(N), todo[i]->title});
                                resolveGbaArtInteractive(target, config, todo[i]->path.filename().generic_string(),
                                                         todo[i]->title, todo[i]->coverPath, aes[i], pcs[i], false);
                            }
                        }
                        // PHASE 2: unattended build + install
                        int okCount = 0;
                        for (int i = 0; i < N; i++) {
                            MenuSelection* it = todo[i];
                            showLoading(target, {"Installing "+std::to_string(i+1)+"/"+std::to_string(N), it->title});
                            bool ok;
                            if (slug == ROMM_SLUG_GBA)
                                ok = installGbaInject(target, config, it->path.generic_string(), it->title,
                                                      it->path.filename().generic_string(), aes[i], pcs[i]);
                            else
                                ok = buildForwarderFor(target, config, it->path.generic_string(), it->title,
                                                       it->coverPath, false);
                            if (ok) okCount++;
                            else failed.push_back(it->title);
                        }
                        std::vector<std::string> msg;
                        msg.push_back("Installed "+std::to_string(okCount)+" of "+std::to_string(N));
                        if (!failed.empty()) {
                            msg.push_back("Could not install:");
                            int shown = 0;
                            for (auto& f : failed) { if (shown++ >= 3) break; msg.push_back(f); }
                        }
                        Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                        while (this->queue.size() > 0) this->queue.pop();
                        gFwdReady = false; invalidateYanbfCache(); invalidateManagedRoms();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,slug,target);
                    }
                    if (act == A_UNINSTALL) {
                        // mixed selections uninstall only the installed rows
                        // (the option says so); all-not-installed = Delete ROMs
                        std::vector<MenuSelection*> todoU = items;
                        if (notInstalled > 0 && notInstalled < M) {
                            todoU.clear();
                            for (auto e : items)
                                if (manageItemInstalled(slug, *e)) todoU.push_back(e);
                        }
                        int NU = (int)todoU.size();
                        std::string q = (!is3ds && notInstalled == M)
                            ? "Delete " + std::to_string(NU) + " ROM file(s)?"
                            : "Uninstall " + std::to_string(NU) + " games?";
                        if (Dialog(target,0,0,320,240,{q},
                                   {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                            break;
                        int okCount = 0;
                        std::vector<std::string> failed;
                        for (int i = 0; i < NU; i++) {
                            showLoading(target, {"Uninstalling "+std::to_string(i+1)+"/"+std::to_string(NU), todoU[i]->title});
                            if (uninstallManageItem(config, *todoU[i])) okCount++;
                            else failed.push_back(todoU[i]->title);
                        }
                        std::vector<std::string> msg;
                        msg.push_back("Uninstalled "+std::to_string(okCount)+" of "+std::to_string(NU));
                        int shown = 0;
                        for (auto& f : failed) { if (shown++ >= 4) break; msg.push_back("x "+shorten(f,28)); }
                        if ((int)failed.size() > 4) msg.push_back("...and "+std::to_string((int)failed.size()-4)+" more");
                        Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                    } else {   // A_CHANGEART / A_ARTSCREEN: sequential picker per rebuildable item
                        // Art + filter: ONE preset picked up front, baked into
                        // the same re-bake as each game's new art (never two)
                        int fc = -1;
                        if (act == A_ARTSCREEN) {
                            fc = pickGbaScreenPreset(target, config,
                                                     std::to_string(rebuildable) + " selected");
                            if (fc < 0) break;
                        }
                        int okCount = 0, skipped = 0, done = 0;
                        std::vector<std::string> failed;
                        for (int i = 0; i < M; i++) {
                            MenuSelection* it = items[i];
                            bool rb = (slug == ROMM_SLUG_GBA && it->installed) || (slug == ROMM_SLUG_NDS && it->rtid);
                            if (!rb) continue;
                            done++;
                            showLoading(target, {"Art "+std::to_string(done)+"/"+std::to_string(rebuildable), it->title});
                            std::string base = it->path.filename().generic_string();
                            int rc = (slug == ROMM_SLUG_GBA)
                                ? changeArtGbaItem(target, config, base, it->title, it->coverPath, it->path.generic_string(), false, fc)
                                : changeArtNdsRommItem(target, config, base, it->title, it->coverPath, it->path.generic_string(), it->rtid, false);
                            // Art + filter with the art picker skipped: the
                            // game still gets the preset it was promised
                            if (rc == 0 && act == A_ARTSCREEN)
                                rc = applyGbaScreenItem(target, config, base, it->title,
                                                        it->coverPath, it->path.generic_string(), false, fc);
                            if (rc == 1) okCount++;
                            else if (rc == 0) skipped++;
                            else failed.push_back(it->title);
                        }
                        std::vector<std::string> msg;
                        msg.push_back("Updated art for "+std::to_string(okCount)+" of "+std::to_string(rebuildable));
                        if (skipped > 0) msg.push_back(std::to_string(skipped)+" skipped");
                        int shown = 0;
                        for (auto& f : failed) { if (shown++ >= 3) break; msg.push_back(f); }
                        if ((int)failed.size() > 3) msg.push_back("...and "+std::to_string((int)failed.size()-3)+" more");
                        Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                    }
                    while (this->queue.size() > 0) this->queue.pop();
                    if (!is3ds) { gFwdReady = false; invalidateYanbfCache(); invalidateManagedRoms(); }
                    showLoading(target, {"Refreshing..."});
                    return generateManageMenu(this,config->dsiwareCount,slug,target);
                }
                default:
                    break;
            }
            this->queue.pop();
        }
        return this;
    }
    bool sortMenuSelections(MenuSelection* a, MenuSelection* b) {
        std::string aDisplay = toLowerCase(a->display);
        std::string bDisplay = toLowerCase(b->display);
        if ((a->action==OpenFolder && b->action==OpenFolder) || (!(a->action==OpenFolder) && !(b->action==OpenFolder)))
            return aDisplay<bDisplay;
        return a->action==OpenFolder;
    }
std::string shorten(std::string s, u16 len) {
    if (len > 8 && s.length() > len) {
        return s.substr(0,5)+"..."+s.substr(s.length()-(len-8));
    }
    return s;
}
