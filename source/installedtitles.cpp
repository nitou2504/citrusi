#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include "installedtitles.hpp"
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
            gNames[it.key()] = it.value().get<std::string>();
    } catch (...) { gNames.clear(); }
}

static void saveNameCache() {
    if (!gNamesDirty) return;
    gNamesDirty = false;
    nlohmann::json j;
    for (auto& kv : gNames) j[kv.first] = kv.second;
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

// FBI-style: another title's ExeFS icon IS readable from userland (only its
// RomFS is blocked). archive 0x2345678A + filePath {0,0,2,'icon',0}.
static bool readSmdhName(u64 tid, FS_MediaType media, std::string& out) {
    u32 archPath[4] = { (u32)(tid & 0xFFFFFFFF), (u32)(tid >> 32), (u32)media, 0 };
    u32 filePath[5] = { 0, 0, 2, 0x6E6F6369 /* 'icon' */, 0 };
    FS_Path ap = { PATH_BINARY, sizeof(archPath), archPath };
    FS_Path fp = { PATH_BINARY, sizeof(filePath), filePath };
    Handle h = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0)))
        return false;
    u8 smdh[0x288];
    u32 read = 0;
    Result rc = FSFILE_Read(h, &read, 0, smdh, sizeof(smdh));
    FSFILE_Close(h);
    if (R_FAILED(rc) || read < sizeof(smdh) || memcmp(smdh, "SMDH", 4) != 0) return false;
    // title blocks: 16 languages x 0x200 from 0x08; [1] = English
    out = smdhTitleAt(smdh, 0x208);
    if (out.empty()) out = smdhTitleAt(smdh, 0x008);   // language 0 (Japanese)
    return !out.empty();
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

// every title on one media, with size + version filled in (no names)
static std::vector<InstalledTitle> enumerateMedia(FS_MediaType media) {
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
    int need = 0;
    for (auto& t : out)
        if (!gNames.count(nameKey(t.tid, t.version))) need++;
    u64 t0 = osGetTime();
    int done = 0;
    for (auto& t : out) {
        std::string key = nameKey(t.tid, t.version);
        auto hit = gNames.find(key);
        if (hit != gNames.end()) { t.name = hit->second; continue; }
        if (progress) progress(done, need);
        done++;
        std::string name;
        if (!readSmdhName(t.tid, t.media, name)) name = fallbackName(t.tid, t.media);
        gNames[key] = name;
        gNamesDirty = true;
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

void installedTitlesInvalidate() {
    gTallyOk = false;
    gNamesLoaded = false;   // a reinstall may have bumped a version
    gNames.clear();
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
