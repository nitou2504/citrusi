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

#define SETTING_RANDOM_TID 0
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
#define SETTING_SHOW_3DS 7
#define SETTING_ART_NOTIFY 8
#define SETTING_SGDB_KEY 9
#define SETTING_GBA_SCREEN 20   // outside the server-row range (10-13)
#define SETTING_MANAGE_ART 21

static CtrBuilder gCtr;
static bool gCtrReady = false;

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
    bool persist = false;
    if (pickArt) {                                // "Change art": banner picker first
        ArtEntry pe = ae;
        if (pe.query.empty()) {
            std::vector<std::string> qs = artQueriesFor(romBase, title);
            pe.query = qs.empty() ? artSanitizeQuery(romBase) : qs[0];
        }
        ArtPieces picked;
        bool bCh = false;
        artPickerRun(target, romBase, title, coverPath, ROMM_SLUG_NDS,
                     pe, picked, false, true, nullptr, &bCh);
        if (bCh) {
            boxart = picked.bannerTex;
            pe.weak = false;
            ae = pe;
            persist = true;
        }
    }
    if (boxart.empty() && ae.valid && !ae.bannerSource.empty()) {   // F6: reuse silently
        showLoading(target, {"Preparing art...", title});
        ArtPieces p;
        if (artBuildFromEntry(gSgdb, gRomm, romBase, coverPath, ae, p))
            boxart = p.bannerTex;
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
    ReturnResult* r = gCtr.buildCIA(romPath, title, ctid, boxart, gameCwav);
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
                                     bool pickArt, bool interactive) {
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
        int mode = gbaScreenFor(gbaArtEntry, config);
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
static int pickGbaScreenPreset(C3D_RenderTarget* target, Config* config,
                               const std::string& title, int current = -1) {
    static const char* names[GBA_SCREEN_COUNT] = {
        "AGS-101 colors", "Original dark filter", "Unfiltered",
        "Brighter gamma", "Night (warm)"};
    static const char* descs[GBA_SCREEN_COUNT] = {
        "Gamma-corrected to match the backlit AGS-101 screen. Vivid colors without the dark cast. Recommended.",
        "Nintendo's own Virtual Console filter. Authentic, but noticeably dark and muted.",
        "The raw palette, no filter at all. Brightest picture; colors look washed out.",
        "A gentler gamma correction - halfway between AGS-101 and unfiltered.",
        "AGS-101 colors plus a warm 3400K tint - easier on the eyes in the dark."};
    int def = config->gbaScreen % GBA_SCREEN_COUNT;
    if (current >= 0) current %= GBA_SCREEN_COUNT;
    int sel = (current >= 0) ? current : def;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_UP)   sel = (sel + GBA_SCREEN_COUNT - 1) % GBA_SCREEN_COUNT;
        if (kd & KEY_DOWN) sel = (sel + 1) % GBA_SCREEN_COUNT;
        if (kd & (KEY_A | KEY_START)) return sel;
        if (kd & KEY_B) return -1;
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(target, COL_BG);
        C2D_SceneBegin(target);
        drawText(160, 16, 0.5f, 0.55f, 0, COL_TEXT, "Screen filter", C2D_AlignCenter);
        drawText(160, 34, 0.5f, 0.42f, 0, COL_TEXT_DIM, title.c_str(), C2D_AlignCenter);
        float y = 50;
        for (int i = 0; i < GBA_SCREEN_COUNT; i++, y += 24) {
            bool hot = (i == sel);
            if (hot) C2D_DrawRectSolid(12, y, 0.4f, 296, 22, COL_ACCENT);
            std::string label = names[i];
            if (i == current)  label += "  (current)";
            else if (i == def) label += "  (default)";
            drawText(22, y + 11, 0.5f, 0.5f, 0,
                     hot ? HIGHLIGHT_FOREGROUND : COL_TEXT_DIM, label.c_str(), 0);
        }
        C2D_DrawRectSolid(12, y + 4, 0.4f, 296, 1, COL_ELEV);
        drawWrapped(16, y + 12, 288, 13, 0.42f, COL_TEXT_DIM, descs[sel], 3);
        drawText(160, 230, 0.5f, 0.4f, 0, COL_TEXT_DIM, "A apply    B cancel", C2D_AlignCenter);
        C3D_FrameEnd(0);
    }
    return -1;
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
    Dialog(target,0,0,320,240,{"Applying screen filter...",title},{},0).handle();
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
        if (rc == 1) Dialog(target,0,0,320,240,{"Screen filter applied!",title},{"OK"}).handle();
        else Dialog(target,0,0,320,240,{(gr->message=="cancelled")?"Cancelled":"Update failed",gr->message},{"OK"}).handle();
    }
    delete gr;
    return rc;
}

static int changeArtGbaItem(C3D_RenderTarget* target, Config* config,
                            const std::string& romBase, const std::string& title,
                            const std::string& coverPath, const std::string& romPath,
                            bool interactive) {
    if (!ensureCtrBuilder(target)) return -1;
    // ask WHICH art up front — skipping an unwanted page with a well-timed B
    // was the only way before, and easy to fumble
    bool pIcon = true, pBanner = true;
    if (interactive) {
        int w = Dialog(target,0,0,320,240,{"Change which art?",title},
                       {"Icon","Banner","Both","Back"}).handle();
        if (w < 0 || w == 3) return 0;
        pIcon   = (w == 0 || w == 2);
        pBanner = (w == 1 || w == 2);
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
    int mode = gbaScreenFor(ae, config);   // art change keeps the game's preset
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
    ArtEntry ae = artStoreGet(name);
    if (ae.query.empty()) {
        std::vector<std::string> qs = artQueriesFor(name, title);
        ae.query = qs.empty() ? artSanitizeQuery(name) : qs[0];
    }
    ArtPieces pieces;
    bool bCh = false;
    artPickerRun(target, name, title, coverPath, ROMM_SLUG_NDS,
                 ae, pieces, false, true, nullptr, &bCh);
    if (!bCh) return 0;                                  // picker cancelled
    ae.weak = false;
    Dialog(target,0,0,320,240,{"Fetching sound...",title},{},0).handle();
    std::string gameCwav = fetchGameSound(gRomm, romPath);
    Dialog(target,0,0,320,240,{"Updating art...",title},{},0).handle();
    ReturnResult* r = gCtr.buildCIA(romPath, title, rtid, pieces.bannerTex, gameCwav);
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

static bool uninstallManageItem(Config* config, const MenuSelection& it) {
    if (it.platformSlug == ROMM_SLUG_3DS) {
        if (it.protectedTitle) return false;   // this app / a system title
        Result dr = AM_DeleteTitle(MEDIATYPE_SD, it.tid);
        AM_DeleteTicket(it.tid);
        if (R_SUCCEEDED(dr)) installedTitlesInvalidate();
        return R_SUCCEEDED(dr);
    }
    if (it.platformSlug == ROMM_SLUG_GBA) {
        Result dr = AM_DeleteTitle(MEDIATYPE_SD, it.tid);
        AM_DeleteTicket(it.tid);
        if (R_FAILED(dr)) return false;
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
    MenuSelection::MenuSelection(MenuSelection* old) {
        this->display=old->display;
        this->path=old->path;
        this->action=old->action;
        this->rommId=old->rommId;
        this->fsName=old->fsName;
        this->fileId=old->fileId;
        this->titleId=old->titleId;
        this->platformSlug=old->platformSlug;
        this->installable=old->installable;
        this->title=old->title;
        this->coverPath=old->coverPath;
        this->coverSmallPath=old->coverSmallPath;
        this->summary=old->summary;
        this->genres=old->genres;
        this->year=old->year;
        this->rating=old->rating;
        this->sizeBytes=old->sizeBytes;
        this->tid=old->tid;
        this->ytid=old->ytid;
        this->rtid=old->rtid;
        this->installed=old->installed;
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
    void Menu::refreshStrings() {
        for (auto entry : this->entries) {
            if (entry->action == Install_All) {
                entry->display = gLang.getString("menu_installAll");
            }
        }
    }
    // Install-from-SD lists the three rom dirs, which also hold everything
    // downloaded from RomM — so installed files are hidden by default and X
    // reveals them (needed for Reinstall / Change art).
    static bool gLocalShowInstalled = false;
    static int  gLocalHidden = 0;        // installed files the filter left out

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
            if (this->type == MENU_LOCAL) {
                int nSel = 0;
                for (auto e : this->entries) if (e->selected) nSel++;
                if (nSel > 0) title += "  " + std::to_string(nSel) + " selected";
            }
            // flat background + header
            C2D_DrawRectSolid(0, 0, 0, 400, 240, COL_BG);
            drawText(12, 7, 0.5f, 0.5f, COL_BG, COL_TEXT_DIM, title.c_str(), 0);
            if (!this->entries.empty() && this->type != MENU_MAIN && this->type != MENU_SETTINGS && this->type != MENU_SERVER) {
                int nsel = this->selectedCount();
                char pos[24];
                if (nsel > 0) {          // multiselect: show the count in the accent color
                    snprintf(pos, sizeof(pos), "%d selected", nsel);
                    drawText(390, 7, 0.5f, 0.5f, COL_BG, COL_ACCENT, pos, C2D_AlignRight);
                } else {
                    snprintf(pos, sizeof(pos), "%d/%d",
                             (int)(this->selection - this->entries.begin()) + 1,
                             (int)this->entries.size());
                    drawText(390, 7, 0.5f, 0.5f, COL_BG, COL_TEXT_DIM, pos, C2D_AlignRight);
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
                // Install-from-SD: [x] prefix on batch-marked rows
                if (this->type == MENU_LOCAL && (*entry)->selected) body = "[x] " + body;
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
        if ((this->type != MENU_ROMM && this->type != MENU_MANAGE) || this->entries.empty()) return;
        MenuSelection* sel = *this->selection;
        if (sel->action != RommInstall && sel->action != ManageRom) return;
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
                drawBottomFrame("B Back    START Quit");
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
            std::string hint = "A Install   Y Select   B Back";
            if (maxScroll > 0) hint += "   X/L Scroll";
            hint += nsel > 0 ? "   START Install " + std::to_string(nsel) : "   START Quit";
            drawText(160, BAR_Y + (240 - BAR_Y) / 2, 0.56f, 0.42f, 0, COL_TEXT_DIM, hint.c_str(), C2D_AlignCenter);
            return;
        }
        if (this->type == MENU_MANAGE) {
            int nsel = this->selectedCount();
            std::string mhint = this->entries.empty()
                ? std::string("A Manage    B Back    START Quit")
                : ("A Manage   Y Select   B Back" +
                   std::string(nsel > 0 ? "   START Batch " + std::to_string(nsel) : "   START Quit"));
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
            } else {
                if (sel->rtid) cxp = drawChip(cxp, y, "romm3ds", COL_ACCENT);
                if (sel->installed) cxp = drawChip(cxp, y, "TWL", COL_ACCENT);
                if (sel->ytid) cxp = drawChip(cxp, y, "YANBF", COL_ACCENT);
                if (!sel->rtid && !sel->installed && !sel->ytid)
                    drawChip(cxp, y, sel->fwdCia.empty() ? "not installed" : ".cia on SD", COL_TEXT_DIM);
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
            drawBottomFrame("A Change    B Back    START Quit");
            static const char* descs[] = {
                "Give each install a random title ID. Useful for rom hacks that share a game code.",
                "Ask for a custom HOME menu name on every install.",
                "Overwrite existing installs without asking first.",
                "", // language (removed)
                "DSiWare template used by SD card installs.",
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
                else if (id == SETTING_ART_NOTIFY) d = "When icon/banner art isn't found at install, ask before falling back to the RomM cover. Off = silent fallback (marked in Manage).";
                else if (id == SETTING_SGDB_KEY) d = "HOME icons come from SteamGridDB. Press A to type the key (saved to sd:/3ds/romm3ds/sgdb.env) or re-read the file.";
                else if (id == SETTING_GBA_SCREEN) d = "Default color filter baked into new GBA installs. AGS-101 = gamma-corrected (recommended); original = Nintendo's dark filter; unfiltered = brightest, washed; brighter gamma = between the two; night = AGS-101 + warm blue-light filter. Per game: Manage -> game -> Screen.";
                else if (id == SETTING_MANAGE_ART) d = "Art shown for installed games in Manage. Title icons = each game's own HOME icon (fast, always available); RomM covers = box art from the server for library matches.";
                else if (id >= SETTING_SRV_HOST && id <= SETTING_SRV_TEST) d = srvDescs[id - SETTING_SRV_HOST];
                if (d)
                    drawWrapped(CTX, y, CTW, 14, 0.45f, C2D_Color32(0xC6,0xCF,0xE2,255), d, 4);
            }
            return;
        }
        if (this->type == MENU_LOCAL) {
            if (this->entries.empty()) {
                drawBottomFrame(gLocalHidden ? "X Show installed    B Back" : "B Back    START Quit");
                if (gLocalHidden && !gLocalShowInstalled) {
                    drawText(160, 88, 0.55f, 0.45f, COL_SURFACE, COL_TEXT_DIM, "Nothing new to install.", C2D_AlignCenter);
                    drawWrapped(48, 112, 224, 14, 0.42f, COL_TEXT_DIM,
                                "All " + std::to_string(gLocalHidden) + " files on the SD card are already "
                                "installed. Press X to show them and reinstall or change art.", 4);
                } else {
                    drawText(160, 92, 0.55f, 0.45f, COL_SURFACE, COL_TEXT_DIM, "No games found.", C2D_AlignCenter);
                    drawWrapped(48, 116, 224, 14, 0.42f, COL_TEXT_DIM,
                                "Drop .cia in sd:/cia, .nds in sd:/roms/nds, .gba in sd:/roms/gba.", 3);
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
                const char* tag = (sel->platformSlug==ROMM_SLUG_3DS)?"3DS":
                                  (sel->platformSlug==ROMM_SLUG_GBA)?"GBA":"NDS";
                float cxp = drawChip(CTX, y, tag, COL_TEXT_DIM);
                cxp = drawChip(cxp, y, humanSize(sel->sizeBytes), COL_TEXT_DIM);
                if (sel->installed) cxp = drawChip(cxp, y, "INSTALLED", COL_ACCENT);
                if (sel->selected)  drawChip(cxp, y, "SELECTED", COL_ACCENT);
                y += 21;
                y = cardDivider(y) + 5;
                drawWrapped(CTX, y, CTW, 14, 0.45f, lineCol,
                            sel->installed ? "Installed. A reinstalls. Y marks it for a batch."
                                           : "Press A to install. Y marks it for a batch.", 3);
            } else {
                y = drawWrapped(CTX, y, CTW, 17, 0.58f, COL_TEXT, sel->display, 2);
                y += 5;
                y = cardDivider(y) + 5;
                drawWrapped(CTX, y, CTW, 14, 0.45f, lineCol,
                            (sel->action==LocalInstallSelected)
                                ? "Install every game you marked with Y. Art is resolved first, then each one installs unattended."
                                : "Install every game listed that isn't installed yet.", 4);
            }
            std::string hint = (nSel > 0)
                ? std::to_string(nSel) + " selected   START Install   B Back"
                : std::string("A Install   Y Select   X ") +
                  (gLocalShowInstalled ? "Hide done" : "Show all") + "   B Back";
            drawText(160, BAR_Y + (240 - BAR_Y) / 2, 0.56f, 0.42f, 0, COL_TEXT_DIM,
                     hint.c_str(), C2D_AlignCenter);
            return;
        }
        // main menu / systems / SD browser
        drawBottomFrame("A Select    B Back    START Quit");
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
                }
                const StorageTally& s = gManageTally;
                float y = CARD_Y + PAD;
                drawLineTop(CTX, y, 17, 0.58f, COL_TEXT, "Installed");
                y += 17 + 4;
                y = cardDivider(y) + 6;
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

    bool validExtension(const char* extension) {
        char extensions[][5] = {".nds", ".srl", ".ids"};
        for (int i=0;i<3;i++) {
            if (strcasecmp(extension,extensions[i])==0) return true;
        }
        return false;
    }
    Menu* generateMenu(std::filesystem::path path, Menu* prev) {
        delete prev;
        std::vector<MenuSelection*> entries;

        bool ndsFilesVisible=false;
        for (const auto & entry : std::filesystem::directory_iterator(path)) {
            std::string filename = entry.path().filename();
            if (
                filename[0]=='.' ||
                !(entry.is_directory() || validExtension(entry.path().extension().c_str())) ||
                (filename=="_nds" && path.generic_string()=="/")
            )
                continue;
            MenuSelection* menuEntry = new MenuSelection();
            menuEntry->path=entry.path();
            menuEntry->display=filename;
            if (entry.is_directory())  {
                menuEntry->action=OpenFolder;

            }else{
                menuEntry->action=Install;
                ndsFilesVisible=true;

            }
            entries.push_back(menuEntry);
        }
        std::sort(entries.begin(), entries.end(), sortMenuSelections);

        if (path.has_parent_path() && path.parent_path().compare(path)) {
            MenuSelection* prevFolder = new MenuSelection();
            prevFolder->display="..";
            prevFolder->action=OpenFolder;
            prevFolder->path=path.parent_path();
            entries.insert(entries.begin(),prevFolder);
        }
        if (ndsFilesVisible) {
            MenuSelection* installAll = new MenuSelection();
            installAll->action=Install_All;
            installAll->display=gLang.getString("menu_installAll");
            installAll->path=path;
            entries.insert(entries.begin(),installAll);
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory=path.generic_string();
        menu->type=MENU_SD;
        menu->init();
        return menu;
    }
    // "Install from SD": one flat screen listing the local files the user
    // dropped onto the SD card — decrypted .cia (sd:/cia), .nds (sd:/roms/nds)
    // and .gba (sd:/roms/gba) — each row tagged with its system and marked
    // "* " when already installed. No RomM needed; if a platform library is
    // already cached, its title/cover is reused to drive the art pipeline.

    Menu* generateLocalMenu(Menu* prev);   // fwd: the toggle rebuilds the list

    Menu* Menu::toggleShowInstalled() {
        if (this->type != MENU_LOCAL) return this;
        gLocalShowInstalled = !gLocalShowInstalled;
        return generateLocalMenu(this);
    }

    Menu* generateLocalMenu(Menu* prev) {
        delete prev;
        CoverCachePause coverPause;   // the scan owns the SD while it runs
        gLocalHidden = 0;
        installed3dsRefresh();    // AM installed set: GBA inject + .cia detection
        refreshNdsForwarders();   // NDS forwarder detection
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
        std::vector<MenuSelection*> entries;
        std::error_code ec;
        struct Src { std::string dir, slug, tag; std::vector<std::string> exts; };
        std::vector<Src> srcs = {
            {ROMM_CIA_DIR, ROMM_SLUG_3DS, "CIA", {".cia"}},
            {ROMM_NDS_DIR, ROMM_SLUG_NDS, "NDS", {".nds",".srl",".ids"}},
            {ROMM_GBA_DIR, ROMM_SLUG_GBA, "GBA", {".gba",".agb"}},
        };
        for (auto& s : srcs) {
            std::vector<std::filesystem::path> paths;
            for (auto& de : std::filesystem::directory_iterator(s.dir, ec)) {
                if (!de.is_regular_file()) continue;
                std::string fn = de.path().filename().generic_string();
                if (fn.empty() || fn[0]=='.') continue;
                std::string ext = toLowerCase(de.path().extension().generic_string());
                if (std::find(s.exts.begin(), s.exts.end(), ext) == s.exts.end()) continue;
                paths.push_back(de.path());
            }
            std::sort(paths.begin(), paths.end());
            for (auto& p : paths) {
                std::string fname = p.filename().generic_string();
                std::string stem  = p.stem().generic_string();
                MenuSelection* e = new MenuSelection();
                e->action = LocalInstall;
                e->platformSlug = s.slug;
                e->path = p;
                e->fsName = fname;
                e->sizeBytes = std::filesystem::file_size(p, ec);
                const RommRom* lib = libLookup(s.slug, fname);
                e->title = lib ? lib->name : stem;
                if (lib) { e->rommId = lib->id; e->coverPath = lib->coverPath;
                           e->coverSmallPath = lib->coverSmallPath; e->year = lib->year; }
                bool inst;
                if (s.slug == ROMM_SLUG_3DS) {
                    e->titleId = ciaFileTitleId(p.generic_string());
                    inst = installed3dsHasTitle(e->titleId);
                } else if (s.slug == ROMM_SLUG_GBA) {
                    e->tid = gbaTidForRom(fname);
                    inst = installed3dsHasTitle(e->tid);
                } else {
                    inst = ndsForwarderInstalled(fname);
                }
                e->installed = inst;
                if (inst && !gLocalShowInstalled) { gLocalHidden++; delete e; continue; }
                e->display = (inst ? "* " : "  ") + std::string("[") + s.tag + "] " + utf8FoldLatin(stem);
                entries.push_back(e);
            }
        }
        if (!entries.empty()) {
            // batch action rows pinned to the top (kept out of the alnum sort)
            MenuSelection* all = new MenuSelection();
            all->display = "Install all"; all->action = LocalInstallAll;
            entries.insert(entries.begin(), all);
            MenuSelection* sel = new MenuSelection();
            sel->display = "Install selected"; sel->action = LocalInstallSelected;
            entries.insert(entries.begin(), sel);
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory = std::filesystem::path("/");
        menu->type = MENU_LOCAL;
        FS_ArchiveResource sd = {};
        std::string free = "";
        if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD)))
            free = " - " + humanSize((u64)sd.freeClusters * sd.clusterSize) + " free";
        menu->heading = std::string(gLocalShowInstalled ? "Install from SD (all)" : "Install from SD")
                      + (gLocalHidden && !gLocalShowInstalled
                         ? "  " + std::to_string(gLocalHidden) + " installed hidden" : "")
                      + free;
        menu->init();
        return menu;
    }
    Menu* generateMainMenu(Menu* prev) {
        delete prev;
        std::vector<MenuSelection*> entries;
        MenuSelection* romm = new MenuSelection();
        romm->display="RomM Library";
        romm->action=OpenRommLibrary;
        entries.push_back(romm);
        MenuSelection* manage = new MenuSelection();
        manage->display="Manage Installed";
        manage->action=OpenManage;
        entries.push_back(manage);
        MenuSelection* sd = new MenuSelection();
        sd->display="Install from SD";
        sd->action=OpenSDBrowser;
        entries.push_back(sd);
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
        add(SETTING_RANDOM_TID,   std::string("Random title ID: ") + (config->randomTID ? "on" : "off"));
        add(SETTING_CUSTOM_TITLE, std::string("Ask for custom title: ") + (config->customTitle ? "on" : "off"));
        add(SETTING_FORCE,        std::string("Force install: ") + (config->forceInstall ? "on" : "off"));
        add(SETTING_SHOW_3DS,     std::string("Show 3DS .3ds (non-installable): ") + (config->show3dsRoms ? "on" : "off"));
        add(SETTING_ART_NOTIFY,   std::string("Art: ") + (config->artNotify ? "notify when missing" : "silent fallback"));
        add(SETTING_SGDB_KEY,     std::string("SteamGridDB key: ") + (ensureSgdb() ? "found" : "missing"));
        static const char* gbaScreenNames[] = {"AGS-101 colors", "original dark filter", "unfiltered", "brighter gamma", "night (warm)"};
        add(SETTING_GBA_SCREEN,   std::string("GBA screen: ") + gbaScreenNames[config->gbaScreen % GBA_SCREEN_COUNT]);
        add(SETTING_MANAGE_ART,   std::string("Manage art: ") + (config->manageIcons ? "title icons" : "RomM covers"));
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
        bool show3ds = gConfigPtr ? gConfigPtr->show3dsRoms : false;
        installed3dsRefresh();     // 3DS: titles installed on the console
        refreshNdsForwarders();    // NDS: forwarders on the HOME menu
        std::vector<MenuSelection*> entries;
        for (auto& rom : src) {
            if (!flow.empty() &&
                toLowerCase(rom.name).find(flow) == std::string::npos &&
                toLowerCase(rom.fsName).find(flow) == std::string::npos)
                continue;
            // hide non-installable 3DS .3ds entries unless the setting is on
            if (rom.platformSlug == ROMM_SLUG_3DS && !rom.installable && !show3ds)
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
            menu->heading=scope+" - SELECT to search";
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
        if (this->type != MENU_ROMM) return this;
        char buf[64] = {0};
        SwkbdState kb;
        swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 63);
        swkbdSetHintText(&kb, "Search games (empty = show all)");
        swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
        if (!this->filter.empty()) swkbdSetInitialText(&kb, this->filter.c_str());
        if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM)
            return this;
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
        Menu* menu = new Menu(entries);
        menu->currentDirectory = std::filesystem::path("/");
        menu->type = MENU_SYSTEMS;   // back -> main
        menu->heading = "Manage Installed";
        menu->init();
        return menu;
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
        // .cia installers left on the card: once the title is installed the
        // file is a duplicate — offer to reclaim the space
        {
            u64 doneBytes = 0; int doneCount = 0;
            for (auto& c : listCiaFiles()) if (c.installed) { doneBytes += c.sizeBytes; doneCount++; }
            if (doneCount > 0) {
                MenuSelection* e = new MenuSelection();
                e->action = CleanupCias;
                e->title = "Duplicate files";
                e->display = "  Duplicates - " + humanSize(doneBytes);
                e->sizeBytes = doneBytes;
                e->rommId = doneCount;   // count, for the details panel
                entries.push_back(e);
            }
        }
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
        std::map<std::string, const RommRom*> libByName;
        for (auto& cr : gCache[ROMM_SLUG_GBA])
            libByName.emplace(toLowerCase(std::filesystem::path(
                rommLocalPath(cr.fsName, cr.platformSlug)).filename().generic_string()), &cr);
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
            bool weakArt = inst && artStoreGet(fname).weak;   // ⚠: fallback art in use
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
        this->queue.push(MenuSelection(*this->selection));
    }
    // Y: toggle the batch mark on the current row. Only the installable
    // library rows and the manage rows can be selected; a non-installable
    // 3DS .3ds row is skipped (nothing to install).
    void Menu::toggleSelect() {
        if (this->entries.empty()) return;
        MenuSelection* sel = *this->selection;
        if (sel->action != RommInstall && sel->action != ManageRom) return;
        if (sel->action == RommInstall && sel->platformSlug == ROMM_SLUG_3DS && !sel->installable) return;
        sel->selected = !sel->selected;
    }
    int Menu::selectedCount() {
        int n = 0;
        for (auto e : this->entries) if (e->selected) n++;
        return n;
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
            case MENU_LOCAL:
                return generateMainMenu(this);
            case MENU_SD:
            default:
                if (this->currentDirectory.generic_string()=="/" || !this->currentDirectory.has_parent_path())
                    return generateMainMenu(this);
                return generateMenu(this->currentDirectory.parent_path(),this);
        }
    }
    bool Menu::hasQueue() {
        return this->queue.size() > 0;
    }

    // shared install helper: template load + buildCIA + overwrite dialog
    static bool installForwarder(Builder* builder, C3D_RenderTarget* target, Config* config,
                                 const std::string& romPath, bool allowRandomTid) {
        if (!(builder->loadTemplate(config->templates.at(config->currentTemplate)))->isSuccess()) {
            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getString("menu_noTemplate")},{gLang.getString("menu_ok")}).handle();
            return false;
        }
        bool randomTID = allowRandomTid && config->randomTID;
        ReturnResult* buildResult = builder->buildCIA(romPath, randomTID, "", config->forceInstall);
        if (buildResult != nullptr && buildResult->code == ERROR_INSTALL_ALREADY_EXISTS) {
            if (Dialog(target,0,0,320,240,{gLang.getString("error_030102"),gLang.getString("menu_overwriteQ")},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                delete buildResult;
                buildResult = builder->buildCIA(romPath, randomTID, "", true);
            }
        }
        bool ok = buildResult->isSuccess();
        if (!ok) {
            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getErrorString(buildResult->code),gLang.parseString("format_hex",(u32)buildResult->code)},{gLang.getString("menu_ok")}).handle();
        }
        delete buildResult;
        return ok;
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
                case Install:
                    if (config->dsiwareCount >= MAX_DSIWARE) {
                        Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                        break;
                    }
                    if (!(builder->loadTemplate(config->templates.at(config->currentTemplate)))->isSuccess()) {
                        Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getString("menu_noTemplate")},{gLang.getString("menu_ok")}).handle();
                        break;
                    }
                    if (Dialog(target,0,0,320,240,{gLang.getString("menu_installTitleQ"),entry.path.filename().generic_string()},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                        ReturnResult* buildResult=nullptr;
                        std::string customTitle="";
                        bool randomTID = false;
                        bool forceInstall = false;
                        if (config!=nullptr) {
                            randomTID = config->randomTID;
                            forceInstall = config->forceInstall;
                            if (config->customTitle) {
                                char customTitleBuffer[0x51] = {0};
                                SwkbdState kbstate;
                                swkbdInit(&kbstate,SWKBD_TYPE_NORMAL,2,0x50);
                                swkbdSetHintText(&kbstate,gLang.getString("menu_customTitleQ").c_str());
                                swkbdSetFeatures(&kbstate,SWKBD_MULTILINE | SWKBD_DEFAULT_QWERTY);
                                swkbdInputText(&kbstate,customTitleBuffer,0x51);
                                customTitle=std::string(customTitleBuffer);
                            }
                            buildResult = builder->loadTemplate(config->templates.at(config->currentTemplate));
                            if (buildResult->isSuccess()) {
                                delete buildResult;
                                buildResult = builder->buildCIA(entry.path.generic_string(), randomTID, customTitle, forceInstall);
                            }
                        } else {
                            buildResult = builder->buildCIA(entry.path.generic_string());
                        }

                        if (buildResult != nullptr && (buildResult->code == ERROR_INSTALL_ALREADY_EXISTS)) {
                            if (Dialog(target,0,0,320,240,{gLang.getString("error_030102"),gLang.getString("menu_overwriteQ")},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                                delete buildResult;
                                buildResult = builder->buildCIA(entry.path.generic_string(), randomTID, customTitle, true);
                            }
                        }

                        if (buildResult->isSuccess()) {
                            config->dsiwareCount++;
                            Dialog(target,0,0,320,240,gLang.getString("menu_installComplete"),{gLang.getString("menu_ok")}).handle();
                        }else{
                            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getErrorString(buildResult->code),gLang.parseString("format_hex",(u32)buildResult->code)},{gLang.getString("menu_ok")}).handle();
                        }
                        delete buildResult;
                    }
                    break;
                case Install_All:
                    if (Dialog(target,0,0,320,240,{gLang.getString("menu_installTitleQ"),gLang.getString("menu_allForwarders"),(!entry.path.filename().generic_string().empty())?entry.path.filename().generic_string():"/"},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                        if (!(builder->loadTemplate(config->templates.at(config->currentTemplate)))->isSuccess()) {
                            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getString("menu_noTemplate")},{gLang.getString("menu_ok")}).handle();
                            break;
                        }
                        for (const auto & dEntry : std::filesystem::directory_iterator(entry.path)) {
                            if (config->dsiwareCount >= MAX_DSIWARE) {
                                Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                                break;
                            }
                            std::string filename = dEntry.path().filename();
                            if (filename[0]=='.' || !validExtension(dEntry.path().extension().c_str()))
                                continue;
                            std::string shortname = shorten(dEntry.path().filename().generic_string(),25);
                            Dialog(target,0,0,320,240,{gLang.getString("menu_installing"),shortname},{},0).handle();
                            ReturnResult* buildResult=nullptr;
                            std::string customTitle="";
                            bool randomTID = false;
                            bool forceInstall = false;
                            if (config!=nullptr) {
                                randomTID = config->randomTID;
                                forceInstall = config->forceInstall;
                                if (config->customTitle) {
                                    char customTitleBuffer[0x51] = {0};
                                    SwkbdState kbstate;
                                    swkbdInit(&kbstate,SWKBD_TYPE_NORMAL,2,0x50);
                                    swkbdSetHintText(&kbstate,shortname.c_str());
                                    swkbdSetFeatures(&kbstate,SWKBD_MULTILINE | SWKBD_DEFAULT_QWERTY);
                                    swkbdInputText(&kbstate,customTitleBuffer,0x51);
                                    customTitle=std::string(customTitleBuffer);
                                }
                                buildResult = builder->buildCIA(dEntry.path().generic_string(), randomTID, customTitle, forceInstall);
                            } else {
                                buildResult = builder->buildCIA(dEntry.path().generic_string());
                            }

                            if (buildResult != nullptr && (buildResult->code == ERROR_INSTALL_ALREADY_EXISTS)) {
                                if (Dialog(target,0,0,320,240,{gLang.getString("error_030102"),shortname,gLang.getString("menu_overwriteQ")},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                                    delete buildResult;
                                    buildResult = builder->buildCIA(dEntry.path().generic_string(), randomTID, customTitle, true);
                                }
                            }

                            if (!buildResult->isSuccess()) {
                                Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),shortname,gLang.getErrorString(buildResult->code),gLang.parseString("format_hex",(u32)buildResult->code)},{gLang.getString("menu_ok")}).handle();
                            }else{
                                config->dsiwareCount++;
                            }
                            delete buildResult;
                        }
                        Dialog(target,0,0,320,240,gLang.getString("menu_installComplete"),{gLang.getString("menu_ok")}).handle();
                    }
                    break;
                case OpenFolder:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateMenu(entry.path,this);
                case OpenSDBrowser:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateLocalMenu(this);
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
                case EditRommConfig:
                    gRomm.loadConfig();
                    if (gRomm.promptConfig())
                        Dialog(target,0,0,320,240,{"Saved.","Server: "+gRomm.host},{"OK"}).handle();
                    break;
                case OpenSettings:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateSettingsMenu(this, config);
                case SettingToggle: {
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
                        return generateServerMenu(this);
                    }
                    switch (entry.rommId) {
                        case SETTING_RANDOM_TID:   config->randomTID = !config->randomTID; break;
                        case SETTING_CUSTOM_TITLE: config->customTitle = !config->customTitle; break;
                        case SETTING_FORCE:        config->forceInstall = !config->forceInstall; break;
                        case SETTING_SHOW_3DS:     config->show3dsRoms = !config->show3dsRoms; break;
                        case SETTING_ART_NOTIFY:   config->artNotify = !config->artNotify; break;
                        case SETTING_GBA_SCREEN:   config->gbaScreen = (config->gbaScreen + 1) % GBA_SCREEN_COUNT; break;
                        case SETTING_MANAGE_ART:   config->manageIcons = !config->manageIcons; break;
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
                        case SETTING_TEMPLATE:
                            config->currentTemplate = (config->currentTemplate + 1) % config->templates.size();
                            break;
                        case SETTING_SERVER:
                            while (this->queue.size() > 0) this->queue.pop();
                            return generateServerMenu(this);
                    }
                    config->save();
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateSettingsMenu(this, config);
                }
                case RommInstall: {
                    bool is3ds = (entry.platformSlug == ROMM_SLUG_3DS);
                    bool isGba = (entry.platformSlug == ROMM_SLUG_GBA);
                    rlog.info("install: " + entry.fsName + " slug=" + entry.platformSlug +
                              " fileId=" + std::to_string(entry.fileId) + " installable=" + (entry.installable?"1":"0"));
                    if (is3ds && entry.fileId == -1) {
                        // file list still unresolved (library was opened offline)
                        showLoading(target, {"Checking files...", entry.title});
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
                    bool onSD = fileExists(is3ds ? dest : romPath);
                    bool needDownload = true;
                    bool pickArt = false;    // "+ Art"/"Change art": picker before the (re)install
                    if (onSD && is3ds) {
                        int c = Dialog(target,0,0,320,240,{"Already downloaded:",entry.fsName},{"Install","Redownload","Back"}).handle();
                        if (c==2 || c==-1) break;
                        needDownload = (c==1);
                    } else if (onSD) {
                        int c = Dialog(target,0,0,320,240,{"Already on SD:",entry.fsName},{"Install","Change art","Back"}).handle();
                        if (c==2 || c==-1) break;
                        pickArt = (c==1);
                        needDownload = false;
                    } else if (isGba) {
                        int c = Dialog(target,0,0,320,240,{"Install this game?",entry.fsName,humanSize(entry.sizeBytes)},{gLang.getString("menu_yes"),"Choose art",gLang.getString("menu_no")}).handle();
                        if (c==2 || c==-1) break;
                        pickArt = (c==1);
                    } else {
                        const char* q = "Install this game?";
                        if (Dialog(target,0,0,320,240,{q,entry.fsName,humanSize(entry.sizeBytes)},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                            break;
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
                                                       gbaArtEntry, gbaArt, pickArt, true);
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
                    int M = (int)items.size();
                    u64 total = 0; for (auto e : items) total += e->sizeBytes;
                    if (Dialog(target,0,0,320,240,
                               {"Install " + std::to_string(M) + " games?", "Total download: " + humanSize(total)},
                               {gLang.getString("menu_yes"), gLang.getString("menu_no")}).handle() != 0)
                        break;
                    bool anyCtr = false;
                    for (auto e : items) if (e->platformSlug != ROMM_SLUG_3DS) { anyCtr = true; break; }
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
                                                 items[i]->coverPath, artEntries[i], arts[i], false);
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
                                                           artEntries[i], arts[i], false, false);
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
                        // behind unless we take them too — offer that in one go
                        TitleExtras ex = findTitleExtras(entry.tid);
                        int c;
                        if (!ex.empty()) {
                            // Uninstall = everything (game + update/DLC);
                            // Extras only keeps the game, frees the extras
                            c = Dialog(target,0,0,320,240,
                                       {n3, "Installed - " + humanSize(entry.sizeBytes + ex.bytes) + " total",
                                        "extras: " + std::to_string(ex.updates + ex.dlc) +
                                        " update/DLC (" + humanSize(ex.bytes) + ")"},
                                       {"Uninstall","Extras only","Back"}).handle();
                        } else {
                            c = Dialog(target,0,0,320,240,
                                       {n3, "Installed - " + humanSize(entry.sizeBytes)},
                                       {"Uninstall","Back"}).handle();
                            if (c == 1) c = 2;   // "Back" is the second button here
                        }
                        if (c != 0 && c != 1) break;
                        bool extrasOnly = (c == 1);
                        u64 freed = extrasOnly ? ex.bytes : entry.sizeBytes + ex.bytes;
                        if (Dialog(target,0,0,320,240,
                                   {extrasOnly ? "Remove update/DLC only?" : "Uninstall game?",
                                    n3, "Frees " + humanSize(freed)},
                                   {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0) break;
                        showLoading(target, {"Uninstalling...", n3});
                        Result dr = 0;
                        if (!extrasOnly) {
                            dr = AM_DeleteTitle(MEDIATYPE_SD, entry.tid);
                            AM_DeleteTicket(entry.tid);
                        }
                        int extrasGone = 0;
                        if (R_SUCCEEDED(dr)) {
                            for (u64 xt : ex.tids) {
                                if (R_SUCCEEDED(AM_DeleteTitle(MEDIATYPE_SD, xt))) extrasGone++;
                                AM_DeleteTicket(xt);
                            }
                        }
                        installedTitlesInvalidate();
                        if (R_FAILED(dr)) Dialog(target,0,0,320,240,{"Uninstall failed",n3},{"OK"}).handle();
                        else if (extrasOnly)
                            Dialog(target,0,0,320,240,{"Extras removed.",n3,
                                   humanSize(ex.bytes)+" freed - the game stays installed."},{"OK"}).handle();
                        else Dialog(target,0,0,320,240,{"Uninstalled.",n3,
                                    extrasGone > 0 ? "Update/DLC went with it." : ""},{"OK"}).handle();
                        while (this->queue.size() > 0) this->queue.pop();
                        showLoading(target, {"Refreshing..."});
                        return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                    }
                    if (entry.platformSlug == ROMM_SLUG_GBA) {   // GBA rom on SD +/- installed inject
                        std::string ng = entry.title;
                        if (entry.installed) {
                            std::string romBase = entry.path.filename().generic_string();
                            bool weakArt = artStoreGet(romBase).weak;
                            int c = Dialog(target,0,0,320,240,{ng,weakArt?"Installed - using fallback art":"Installed"},{"Change art","Screen","Uninstall","Back"}).handle();
                            if (c==0) {
                                // rebuild in place: same TID keeps the HOME
                                // position and save data, only the art changes
                                int rc = changeArtGbaItem(target, config, romBase, ng,
                                                          entry.coverPath, entry.path.generic_string(), true);
                                if (rc == 0) break;   // picker cancelled — stay put
                                while (this->queue.size() > 0) this->queue.pop();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            }
                            if (c==1) {
                                // pick a preset (preselected on the Settings default),
                                // then re-bake in place — art untouched, save kept
                                int fc = pickGbaScreenPreset(target, config, ng,
                                                             artStoreGet(romBase).screen);
                                if (fc < 0) break;
                                applyGbaScreenItem(target, config, romBase, ng,
                                                   entry.coverPath, entry.path.generic_string(), true, fc);
                                while (this->queue.size() > 0) this->queue.pop();
                                showLoading(target, {"Refreshing..."});
                                return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                            }
                            if (c!=2) break;
                            // single-pass: uninstall removes the inject AND the ROM file
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
                            int c = Dialog(target,0,0,320,240,{ng,"Not installed."},{"Install","+ Art","Delete ROM","Back"}).handle();
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
                            int c = Dialog(target,0,0,320,240,{name,"Not installed.","A ready .cia is on your SD."},{"Install","Rebuild","Delete ROM","Back"}).handle();
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
                            ? Dialog(target,0,0,320,240,{name,"Not installed."},{"Install","Delete ROM","Back"}).handle()
                            : 0;
                        if (c==0) {
                            if (config->dsiwareCount >= MAX_DSIWARE) {
                                Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                                break;
                            }
                            if (buildForwarderFor(target, config, entry.path.generic_string(), entry.title, entry.coverPath))
                                Dialog(target,0,0,320,240,{"Installed!",entry.title},{"OK"}).handle();
                            while (this->queue.size() > 0) this->queue.pop();
                            gFwdReady = false; invalidateYanbfCache();
                            showLoading(target, {"Refreshing..."});
                            return generateManageMenu(this,config->dsiwareCount,this->platformSlug,target);
                        } else if (c==1) {
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
                        int c = Dialog(target,0,0,320,240,{name,fwdState},{"Change art","Uninstall","Back"}).handle();
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
                        if (Dialog(target,0,0,320,240,{name,fwdState},{"Uninstall","Back"}).handle()!=0) break;
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
                    if (Dialog(target,0,0,320,240,{all?"Install all games?":"Install selected games?",std::to_string(items.size())+" game(s)"},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                        break;
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
                                                 it->coverPath, aes[i], pcs[i], false);
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
                                      if (tid && it->rommId > 0) installed3dsRecord(it->rommId, tid); }
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
                    bool pickArt = false;
                    if (entry.installed) {
                        if (is3ds) {
                            if (Dialog(target,0,0,320,240,{name,"Installed"},{"Reinstall","Back"}).handle()!=0) break;
                        } else {
                            int c = Dialog(target,0,0,320,240,{name,"Installed"},{"Reinstall","Change art","Back"}).handle();
                            if (c==2 || c==-1) break;
                            pickArt = (c==1);
                        }
                    } else if (isGba) {
                        int c = Dialog(target,0,0,320,240,{"Install this game?",name,humanSize(entry.sizeBytes)},{gLang.getString("menu_yes"),"Choose art",gLang.getString("menu_no")}).handle();
                        if (c==2 || c==-1) break;
                        pickArt = (c==1);
                    } else {
                        if (Dialog(target,0,0,320,240,{"Install this game?",name,humanSize(entry.sizeBytes)},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0) break;
                    }
                    if (!is3ds && !ensureCtrBuilder(target)) break;
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
                        installed = installGbaInject(target, config, romPath, name, fname, ae, pieces);
                    } else {
                        installed = buildForwarderFor(target, config, romPath, name, entry.coverPath, pickArt);
                        if (installed) { gFwdReady = false; invalidateManagedRoms(); }
                    }
                    if (installed) {
                        Dialog(target,0,0,320,240,{"Installed!",name},{"OK"}).handle();
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
                    std::vector<MenuSelection*> items;
                    for (auto e : this->entries)
                        if (e->selected && e->action == ManageRom)
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
                    enum { A_INSTALL, A_UNINSTALL, A_CHANGEART, A_SCREEN, A_BACK } act = A_BACK;
                    if (!is3ds && notInstalled == M) {
                        int c = Dialog(target,0,0,320,240,{std::to_string(M)+" selected"},
                                       {"Install selected","Delete ROMs","Back"}).handle();
                        act = (c==0) ? A_INSTALL : (c==1) ? A_UNINSTALL : A_BACK;
                    } else if (notInstalled > 0) {
                        int c = Dialog(target,0,0,320,240,{std::to_string(M)+" selected",
                                       std::to_string(notInstalled)+" not installed"},
                                       {"Install","Uninstall","Back"}).handle();
                        act = (c==0) ? A_INSTALL : (c==1) ? A_UNINSTALL : A_BACK;
                    } else if (is3ds || rebuildable == 0) {
                        int c = Dialog(target,0,0,320,240,{std::to_string(M)+" selected"},
                                       {"Uninstall selected","Back"}).handle();
                        act = (c==0) ? A_UNINSTALL : A_BACK;
                    } else if (slug == ROMM_SLUG_GBA) {
                        int c = Dialog(target,0,0,320,240,{std::to_string(M)+" selected"},
                                       {"Uninstall","Change art","Screen","Back"}).handle();
                        act = (c==0) ? A_UNINSTALL : (c==1) ? A_CHANGEART : (c==2) ? A_SCREEN : A_BACK;
                    } else {
                        int c = Dialog(target,0,0,320,240,{std::to_string(M)+" selected"},
                                       {"Uninstall selected","Change art selected","Back"}).handle();
                        act = (c==0) ? A_UNINSTALL : (c==1) ? A_CHANGEART : A_BACK;
                    }
                    if (act == A_BACK) break;
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
                            showLoading(target, {"Screen filter "+std::to_string(i+1)+"/"+std::to_string(M), it->title});
                            std::string base = it->path.filename().generic_string();
                            if (applyGbaScreenItem(target, config, base, it->title, it->coverPath,
                                                   it->path.generic_string(), false, fc) == 1) okCount++;
                            else failed.push_back(it->title);
                        }
                        std::vector<std::string> msg;
                        msg.push_back("Screen filter applied to "+std::to_string(okCount)+" of "+std::to_string(M));
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
                        int N = (int)todo.size();
                        CoverCachePause coverPause;
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
                        std::vector<std::string> failed;
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
                        if (Dialog(target,0,0,320,240,{"Uninstall "+std::to_string(M)+" games?"},
                                   {gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                            break;
                        int okCount = 0;
                        std::vector<std::string> failed;
                        for (int i = 0; i < M; i++) {
                            showLoading(target, {"Uninstalling "+std::to_string(i+1)+"/"+std::to_string(M), items[i]->title});
                            if (uninstallManageItem(config, *items[i])) okCount++;
                            else failed.push_back(items[i]->title);
                        }
                        std::vector<std::string> msg;
                        msg.push_back("Uninstalled "+std::to_string(okCount)+" of "+std::to_string(M));
                        int shown = 0;
                        for (auto& f : failed) { if (shown++ >= 4) break; msg.push_back("x "+shorten(f,28)); }
                        if ((int)failed.size() > 4) msg.push_back("...and "+std::to_string((int)failed.size()-4)+" more");
                        Dialog(target,0,0,320,240, msg, {"OK"}).handle();
                    } else {   // A_CHANGEART: sequential picker per rebuildable item
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
                                ? changeArtGbaItem(target, config, base, it->title, it->coverPath, it->path.generic_string(), false)
                                : changeArtNdsRommItem(target, config, base, it->title, it->coverPath, it->path.generic_string(), it->rtid, false);
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
