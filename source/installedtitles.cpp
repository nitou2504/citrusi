#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include "installedtitles.hpp"
#include "romm.hpp"        // ROMM_NDS_DIR / ROMM_GBA_DIR
#include "ciainstall.hpp"  // ciaFileTitleId
#include "installed3ds.hpp"
#include "ctrbuilder.hpp"   // CTR_UID_BASE / GBA_UID_BASE
#include "helpers.hpp"      // utf8FoldLatin, toLowerCase
#include "settings.hpp"     // FORWARDER_DIR
#include "logger.hpp"
#include "json.hpp"

static Logger tlog("titles");

// unique id of this app (cia/app.rsf) — never listed, never deleted
#define SELF_UID 0xFF3FF

// ---- SMDH name cache (sd:/3ds/forwarder/smdhnames.json) --------------------
// keyed "<tid>|<version>": a title's name only changes when its version does.

#define SMDH_CACHE_FILE (FORWARDER_DIR + std::string("/smdhnames.json"))
static std::map<std::string, std::string> gNames;
static std::map<std::string, u32> gRegions;   // same key; SMDH region lockout
static bool gNamesLoaded = false;
static bool gNamesDirty = false;

static void loadNameCache() {
    if (gNamesLoaded) return;
    gNamesLoaded = true;
    std::ifstream in(SMDH_CACHE_FILE);
    if (!in.good()) return;
    try {
        nlohmann::json j; in >> j;
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (it.key() == "_regions" && it.value().is_object()) {
                for (auto rit = it.value().begin(); rit != it.value().end(); ++rit)
                    if (rit.value().is_number()) gRegions[rit.key()] = rit.value().get<u32>();
                continue;
            }
            if (!it.value().is_string()) continue;
            std::string v = it.value().get<std::string>();
            // drop the fallbacks the first build cached (CTR-P-XXXX / raw tid)
            bool looksLikeCode = (v.size() == 10 && v[3] == '-' && v[5] == '-') || v.size() == 16;
            if (!looksLikeCode) gNames[it.key()] = v;
        }
    } catch (...) { gNames.clear(); gRegions.clear(); }
}

static void saveNameCache() {
    if (!gNamesDirty) return;
    gNamesDirty = false;
    nlohmann::json j;
    for (auto& kv : gNames) j[kv.first] = kv.second;
    for (auto& kv : gRegions) j["_regions"][kv.first] = kv.second;
    std::ofstream o(SMDH_CACHE_FILE);
    o << j.dump();
}

static std::string nameKey(u64 tid, u16 version) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llX|%u", (unsigned long long)tid, (unsigned)version);
    return std::string(buf);
}

// ---- SMDH reads ------------------------------------------------------------

// one 0x80-byte UTF-16 short title -> folded utf8
static std::string smdhTitleAt(const u8* smdh, size_t off) {
    u16 in[0x41];
    memcpy(in, smdh + off, 0x80);
    in[0x40] = 0;
    u8 out[0x120] = {0};
    if (utf16_to_utf8(out, in, sizeof(out) - 1) <= 0) return "";
    std::string s(reinterpret_cast<char*>(out));
    for (auto& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    // collapse the double spaces the newline swap leaves behind, then trim
    std::string t;
    for (char c : s) {
        if (c == ' ' && (t.empty() || t.back() == ' ')) continue;
        t += c;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return utf8FoldLatin(t);
}

// Reads a title's own SMDH (its ExeFS "icon" file) exactly the way FBI does:
// archive 0x2345678A + filePath {0,0,2,'icon',0}, and — this is the part that
// matters — the WHOLE 0x36C0 struct in one read. A short read silently fails
// on retail titles (homebrew happened to work), which is why every retail game
// used to fall back to its product code.
#define SMDH_SIZE 0x36C0
#define SMDH_ICON48 0x24C0          // large icon: 48x48 RGB565, 8x8 morton tiles
static u8 gSmdhBuf[SMDH_SIZE];

#define ICON_DIR (FORWARDER_DIR + std::string("/titleicons/"))

static std::string iconPath(u64 tid) {
    char b[32];
    snprintf(b, sizeof(b), "%016llX.raw", (unsigned long long)tid);
    return ICON_DIR + b;
}

// untile the SMDH icon into plain RGBA8 and keep it next to the name cache —
// the SMDH is only read once per title, so this is the moment to grab the art
static void saveTitleIcon(u64 tid, const u8* smdh) {
    std::error_code ec;
    std::filesystem::create_directories(ICON_DIR, ec);
    const u16* src = (const u16*)(smdh + SMDH_ICON48);
    std::string out(48 * 48 * 4, '\0');
    u8* dst = (u8*)&out[0];
    u32 i = 0;
    for (u32 ty = 0; ty < 48; ty += 8)
        for (u32 tx = 0; tx < 48; tx += 8)
            for (u32 p = 0; p < 64; p++, i++) {
                u32 x = (p & 1) | ((p & 4) >> 1) | ((p & 16) >> 2);
                u32 y = ((p & 2) >> 1) | ((p & 8) >> 2) | ((p & 32) >> 3);
                u16 c = src[i];
                u8* o = &dst[(((ty + y) * 48) + tx + x) * 4];
                o[0] = (u8)(((c >> 11) & 0x1F) << 3);
                o[1] = (u8)(((c >> 5) & 0x3F) << 2);
                o[2] = (u8)((c & 0x1F) << 3);
                o[3] = 255;
            }
    FILE* f = fopen(iconPath(tid).c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

static bool readSmdhName(u64 tid, FS_MediaType media, std::string& out);

// RAM cache (misses included): the Manage list re-asks on every selection
// settle, and a per-ask SD stat/read is exactly the scroll hitch we killed.
// Cleared by installedTitlesInvalidate(): a cached MISS from a moment when
// the title wasn't installed (e.g. between uninstall and reinstall) would
// otherwise blank that game's icon until the app restarts.
static std::map<u64, std::string> gIconRam;

std::string titleIconRGBA(u64 tid) {
    auto it = gIconRam.find(tid);
    if (it != gIconRam.end()) return it->second;
    std::string p = iconPath(tid);
    if (!fileExists(p)) {
        // on demand: GBA injects / NDS forwarders aren't covered by the 3DS
        // name pass — read their SMDH now (readSmdhName saves the icon)
        std::string dummy;
        readSmdhName(tid, MEDIATYPE_SD, dummy);
    }
    std::string d = fileExists(p) ? readEntireFile(p) : std::string();
    if (gIconRam.size() > 256) gIconRam.clear();   // ~9KB each; caps at ~2.3MB
    gIconRam[tid] = d;
    return d;
}

static bool readSmdhName(u64 tid, FS_MediaType media, std::string& out) {
    u32 archPath[4] = { (u32)(tid & 0xFFFFFFFF), (u32)(tid >> 32), (u32)media, 0 };
    u32 filePath[5] = { 0, 0, 2, 0x6E6F6369 /* 'icon' */, 0 };
    FS_Path ap = { PATH_BINARY, sizeof(archPath), archPath };
    FS_Path fp = { PATH_BINARY, sizeof(filePath), filePath };
    Handle h = 0;
    Result rc = FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0);
    if (R_FAILED(rc)) {
        char b[64];
        snprintf(b, sizeof(b), "icon open failed %016llX rc=%08lX",
                 (unsigned long long)tid, (unsigned long)rc);
        tlog.info(b);
        return false;
    }
    u32 read = 0;
    rc = FSFILE_Read(h, &read, 0, gSmdhBuf, SMDH_SIZE);
    FSFILE_Close(h);
    if (R_FAILED(rc) || read != SMDH_SIZE || memcmp(gSmdhBuf, "SMDH", 4) != 0) {
        char b[80];
        snprintf(b, sizeof(b), "icon read failed %016llX rc=%08lX read=%lu",
                 (unsigned long long)tid, (unsigned long)rc, (unsigned long)read);
        tlog.info(b);
        return false;
    }
    // titles[16] start at 0x08, 0x200 each: system language first (FBI's
    // smdh_select_title), then English, then whatever slot is filled
    u8 lang = 1;   // CFG_LANGUAGE_EN
    CFGU_GetSystemLanguage(&lang);
    if (lang > 15) lang = 1;
    out = smdhTitleAt(gSmdhBuf, 0x08 + (size_t)lang * 0x200);
    if (out.empty()) out = smdhTitleAt(gSmdhBuf, 0x208);
    for (int i = 0; out.empty() && i < 16; i++)
        out = smdhTitleAt(gSmdhBuf, 0x08 + (size_t)i * 0x200);
    saveTitleIcon(tid, gSmdhBuf);   // free art for the Manage list
    return !out.empty();
}

// region lockout of the last SMDH readSmdhName parsed (0x2018, u32)
static u32 lastSmdhRegion() {
    u32 r = 0;
    memcpy(&r, gSmdhBuf + 0x2018, 4);
    return r;
}

static std::string fallbackName(u64 tid, FS_MediaType media) {
    char code[16] = {0};
    if (R_SUCCEEDED(AM_GetTitleProductCode(media, tid, code)) && code[0])
        return std::string(code);
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
    return std::string(buf);
}

// ---- enumeration -----------------------------------------------------------

static TitleKind classify(u64 tid, FS_MediaType media, bool& prot) {
    u16 cat = (u16)((tid >> 32) & 0xFFFF);
    u32 uid = (u32)((tid >> 8) & 0xFFFFFF);
    prot = false;
    if (cat == 0x8004 || cat == 0x8005) return TK_DSIWARE;   // TWL, NAND
    if (cat == 0x000E) return TK_UPDATE;
    if (cat == 0x008C) return TK_DLC;
    if (cat != 0x0000 && cat != 0x0002) { prot = true; return TK_SYSTEM; }  // 0010+ system
    if (media != MEDIATYPE_SD) { prot = true; return TK_SYSTEM; }           // apps on NAND are system
    if (uid == SELF_UID) { prot = true; return TK_SELF; }
    if (uid >= 0xFF400 && uid < CTR_UID_BASE) return TK_YANBF;
    if (uid >= CTR_UID_BASE && uid < CTR_UID_BASE + CTR_UID_COUNT) return TK_NDS_FWD;
    if (uid >= GBA_UID_BASE && uid < GBA_UID_BASE + GBA_UID_COUNT) return TK_GBA_INJECT;
    return (cat == 0x0002) ? TK_DEMO : TK_APP;
}

// every title on one media, with size + version filled in (no names).
// The SD list is cached in RAM: findTitleExtras runs from drawBottom on
// every Manage->3DS selection change, and a full AM enumeration per scroll
// step was what made that list crawl. Invalidated with the tally.
static std::vector<InstalledTitle> gSdEnum;
static bool gSdEnumOk = false;
static std::vector<InstalledTitle> enumerateMedia(FS_MediaType media) {
    if (media == MEDIATYPE_SD && gSdEnumOk) return gSdEnum;
    std::vector<InstalledTitle> out;
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(media, &count)) || count == 0) return out;
    std::vector<u64> tids(count);
    u32 read = 0;
    if (R_FAILED(AM_GetTitleList(&read, media, count, tids.data())) || read == 0) return out;
    tids.resize(read);
    std::vector<AM_TitleEntry> info(read);
    // one batch call fills size + version for all of them
    bool haveInfo = R_SUCCEEDED(AM_GetTitleInfo(media, read, tids.data(), info.data()));
    out.reserve(read);
    for (u32 i = 0; i < read; i++) {
        InstalledTitle t;
        t.tid = tids[i];
        t.media = media;
        t.sizeBytes = haveInfo ? info[i].size : 0;
        t.version = haveInfo ? info[i].version : 0;
        t.kind = classify(t.tid, media, t.protectedTitle);
        out.push_back(t);
    }
    if (media == MEDIATYPE_SD) { gSdEnum = out; gSdEnumOk = true; }
    return out;
}

std::vector<InstalledTitle> listInstalledApps(bool includeDemos, bool sortBySize,
                                              const std::function<void(int,int)>& progress) {
    loadNameCache();
    std::vector<InstalledTitle> all = enumerateMedia(MEDIATYPE_SD);
    std::vector<InstalledTitle> out;
    for (auto& t : all) {
        if (t.kind == TK_UPDATE || t.kind == TK_DLC || t.kind == TK_SYSTEM || t.kind == TK_DSIWARE)
            continue;
        if (t.kind == TK_DEMO && !includeDemos) continue;
        out.push_back(t);
    }
    // how many still need an SMDH read (the rest come straight from the cache)
    // an SMDH read gives us BOTH the name and the icon: a title whose name is
    // cached but whose icon was never saved still needs one (the icon cache was
    // added after the name cache, so every earlier title was missing art)
    auto needsRead = [](const InstalledTitle& t) {
        return !gNames.count(nameKey(t.tid, t.version)) || !fileExists(iconPath(t.tid)) ||
               !gRegions.count(nameKey(t.tid, t.version));   // region cache added later
    };
    int need = 0;
    for (auto& t : out)
        if (needsRead(t)) need++;
    u64 t0 = osGetTime();
    int done = 0;
    for (auto& t : out) {
        std::string key = nameKey(t.tid, t.version);
        auto hit = gNames.find(key);
        auto rhit = gRegions.find(key);
        if (rhit != gRegions.end()) t.region = rhit->second;
        if (hit != gNames.end() && !needsRead(t)) { t.name = hit->second; continue; }
        if (progress) progress(done, need);
        done++;
        std::string name;
        if (readSmdhName(t.tid, t.media, name)) {   // also saves the icon
            gNames[key] = name;         // only real names are cached: a product-code
            gNamesDirty = true;         // fallback must never become permanent
            t.region = lastSmdhRegion();
            gRegions[key] = t.region;
        } else if (hit != gNames.end()) {
            name = hit->second;         // icon read failed, keep the cached name
        } else {
            name = fallbackName(t.tid, t.media);
        }
        t.name = name;
    }
    if (gNamesDirty) {
        saveNameCache();
        tlog.info("smdh pass: " + std::to_string(done) + " read, " +
                  std::to_string((unsigned long long)(osGetTime() - t0)) + "ms");
    }
    if (sortBySize) {
        std::sort(out.begin(), out.end(), [](const InstalledTitle& a, const InstalledTitle& b) {
            if (a.sizeBytes != b.sizeBytes) return a.sizeBytes > b.sizeBytes;
            return toLowerCase(a.name) < toLowerCase(b.name);
        });
    } else {
        std::sort(out.begin(), out.end(), [](const InstalledTitle& a, const InstalledTitle& b) {
            return toLowerCase(a.name) < toLowerCase(b.name);
        });
    }
    return out;
}

TitleExtras findTitleExtras(u64 appTid) {
    TitleExtras ex;
    u32 uid = (u32)((appTid >> 8) & 0xFFFFFF);
    if (!appTid) return ex;
    for (auto& t : enumerateMedia(MEDIATYPE_SD)) {
        if (t.kind != TK_UPDATE && t.kind != TK_DLC) continue;
        if ((u32)((t.tid >> 8) & 0xFFFFFF) != uid) continue;
        ex.tids.push_back(t.tid);
        ex.bytes += t.sizeBytes;
        if (t.kind == TK_UPDATE) ex.updates++; else ex.dlc++;
    }
    return ex;
}

// ---- storage tally ---------------------------------------------------------

static bool gTallyOk = false;
static StorageTally gTally;

// after an install/uninstall the numbers moved. names stay valid: they are
// keyed by tid|version, so a new/updated title simply misses the cache.
void installedTitlesInvalidate() { gTallyOk = false; gSdEnumOk = false; gIconRam.clear(); }

// a rebake changed the title's SMDH: the on-SD icon copy is stale, drop it
// (and the RAM copy) so the next ask re-reads the new SMDH
void titleIconInvalidate(u64 tid) {
    remove(iconPath(tid).c_str());
    gIconRam.erase(tid);
}
bool storageTallyCached() { return gTallyOk; }

std::vector<CiaFile> listCiaFiles() {
    std::vector<CiaFile> out;
    static const char* dirs[] = {"sdmc:/cias", "sdmc:/cia"};
    std::error_code ec;
    for (const char* d : dirs) {
        for (auto& de : std::filesystem::directory_iterator(d, ec)) {
            if (!de.is_regular_file(ec)) continue;
            std::string p = de.path().generic_string();
            std::string ext = de.path().extension().generic_string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext != ".cia") continue;
            CiaFile f;
            f.path = p;
            f.name = de.path().stem().generic_string();
            f.sizeBytes = (u64)de.file_size(ec);
            f.tid = ciaFileTitleId(p);                 // header+TMD only
            f.installed = f.tid && installed3dsHasTitle(f.tid);
            out.push_back(f);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const CiaFile& a, const CiaFile& b) { return a.sizeBytes > b.sizeBytes; });
    return out;
}

StorageTally computeStorageTally() {
    if (gTallyOk) return gTally;
    StorageTally s;
    u64 t0 = osGetTime();
    for (auto& t : enumerateMedia(MEDIATYPE_SD)) {
        switch (t.kind) {
            case TK_NDS_FWD:    s.ndsFwdBytes += t.sizeBytes; s.ndsFwdCount++; break;
            case TK_YANBF:      s.yanbfBytes  += t.sizeBytes; s.yanbfCount++;  break;
            case TK_GBA_INJECT: s.gbaBytes    += t.sizeBytes; s.gbaCount++;    break;
            case TK_APP:
            case TK_DEMO:       s.appBytes    += t.sizeBytes; s.appCount++;    break;
            case TK_UPDATE:
            case TK_DLC:        s.extraBytes  += t.sizeBytes; s.extraCount++;  break;
            default: break;   // self / system: not the user's storage to manage
        }
    }
    for (auto& t : enumerateMedia(MEDIATYPE_NAND)) {
        if (t.kind != TK_DSIWARE) continue;   // TWL forwarders + real DSiWare
        s.dsiwareBytes += t.sizeBytes;
        s.dsiwareCount++;
    }
    // the rom files themselves (a forwarder is a few hundred KB; its rom is not)
    auto sumRoms = [](const std::string& dir, u64& bytes, u32& count) {
        std::error_code ec;
        for (auto& de : std::filesystem::directory_iterator(dir, ec)) {
            if (!de.is_regular_file(ec)) continue;
            bytes += (u64)de.file_size(ec);
            count++;
        }
    };
    sumRoms(ROMM_NDS_DIR, s.ndsRomBytes, s.ndsRomCount);
    sumRoms(ROMM_GBA_DIR, s.gbaRomBytes, s.gbaRomCount);
    for (auto& c : listCiaFiles()) {
        s.ciaBytes += c.sizeBytes;
        s.ciaCount++;
        if (c.installed) { s.ciaDoneBytes += c.sizeBytes; s.ciaDoneCount++; }
    }
    FS_ArchiveResource sd = {};
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD)))
        s.sdFreeBytes = (u64)sd.freeClusters * sd.clusterSize;
    char b[128];
    snprintf(b, sizeof(b), "tally: nds=%lu gba=%lu app=%lu extra=%lu twl=%lu %llums",
             (unsigned long)s.dsCount(), (unsigned long)s.gbaCount, (unsigned long)s.appCount,
             (unsigned long)s.extraCount, (unsigned long)s.dsiwareCount,
             (unsigned long long)(osGetTime() - t0));
    tlog.info(b);
    gTally = s;
    gTallyOk = true;
    return s;
}
