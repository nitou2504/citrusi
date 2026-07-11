#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <set>
#include "manage.hpp"
#include "helpers.hpp"
#include "ctrbuilder.hpp"
#include "logger.hpp"

static Logger mlog("manage");

u64 computeForwarderTID(const std::string& ndsPath) {
    FILE* f = fopen(ndsPath.c_str(), "rb");
    if (!f) return 0;
    u8 gameCode[4] = {0};
    u8 unitCode = 0;
    u8 tidLow[4] = {0};
    bool ok = true;
    ok = ok && fseek(f, 0x0C, SEEK_SET) == 0 && fread(gameCode, 1, 4, f) == 4;
    ok = ok && fseek(f, 0x12, SEEK_SET) == 0 && fread(&unitCode, 1, 1, f) == 1;
    if (ok && unitCode != 0x00) {
        ok = fseek(f, 0x230, SEEK_SET) == 0 && fread(tidLow, 1, 4, f) == 4;
    }
    fclose(f);
    if (!ok) return 0;

    // builder writes emagCode at srl+0x230: rev(gameCode) for NTR roms,
    // the rom's own tid_low for DSi-enhanced roms. readTWLTID then reverses
    // srl[0x230..0x234] into the low 5 TID bytes; srl[0x234] is the template's
    // DSiWare category byte (0x04).
    u8 low[4];
    if (unitCode == 0x00) {
        low[0] = gameCode[0]; low[1] = gameCode[1];
        low[2] = gameCode[2]; low[3] = gameCode[3];
    } else {
        low[0] = tidLow[3]; low[1] = tidLow[2];
        low[2] = tidLow[1]; low[3] = tidLow[0];
    }
    u64 tid = 0x0004800400000000ULL;
    tid |= ((u64)low[0] << 24) | ((u64)low[1] << 16) | ((u64)low[2] << 8) | (u64)low[3];
    return tid;
}

std::vector<u64> getInstalledTwlTitles() {
    std::vector<u64> out;
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_NAND, &count)) || count == 0) return out;
    std::vector<u64> titles(count);
    u32 read = 0;
    if (R_FAILED(AM_GetTitleList(&read, MEDIATYPE_NAND, count, titles.data()))) return out;
    for (u32 i = 0; i < read; i++) {
        u16 cat = (u16)((titles[i] >> 32) & 0xFFFF);
        if (cat == 0x8004 || cat == 0x8005) out.push_back(titles[i]);
    }
    return out;
}

bool twlTitleInstalled(const std::vector<u64>& list, u64 tid) {
    for (u64 t : list) if (t == tid) return true;
    return false;
}

static std::string basenameLower(std::string p) {
    size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    // strip trailing whitespace/newlines
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ' || p.back() == '\0'))
        p.pop_back();
    return toLowerCase(p);
}

// mounting each YANBF title's romfs is slow (~27 mounts), so cache it for
// the session. YANBF titles never change while the app runs; invalidated on
// our own YANBF delete.
static bool gYanbfCached = false;
static std::map<std::string, u64> gYanbfCache;

void invalidateYanbfCache() { gYanbfCached = false; }

std::map<std::string, u64> getYanbfForwarders() {
    if (gYanbfCached) return gYanbfCache;
    std::map<std::string, u64> out;
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count)) || count == 0) {
        gYanbfCache = out; gYanbfCached = true; return out;
    }
    std::vector<u64> titles(count);
    u32 read = 0;
    if (R_FAILED(AM_GetTitleList(&read, MEDIATYPE_SD, count, titles.data()))) return out;
    for (u32 i = 0; i < read; i++) {
        u64 tid = titles[i];
        if ((tid >> 32) != 0x00040000ULL) continue;
        u32 low = (u32)(tid & 0xFFFFFFFF);
        if (low < 0x0FF40000 || low > 0x0FF7FFFF) continue;
        // YANBF forwarder: romfs holds path.txt with the rom path
        if (R_FAILED(romfsMountFromTitle(tid, MEDIATYPE_SD, "yfwd"))) continue;
        FILE* f = fopen("yfwd:/path.txt", "rb");
        if (f) {
            char buf[512] = {0};
            fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            std::string name = basenameLower(std::string(buf));
            if (!name.empty()) out[name] = tid;
        }
        romfsUnmount("yfwd");
    }
    gYanbfCache = out;
    gYanbfCached = true;
    return out;
}

std::map<std::string, u64> getRommCtrForwarders() {
    std::map<std::string, u64> out;
    std::error_code ec;
    if (!std::filesystem::exists(CTR_CONFIG_DIR, ec)) return out;
    // installed titles (SD+NAND) so stale config files (deleted forwarders) don't count
    std::set<u64> installed;
    FS_MediaType medias[2] = {MEDIATYPE_SD, MEDIATYPE_NAND};
    for (int m = 0; m < 2; m++) {
        u32 count = 0;
        if (R_FAILED(AM_GetTitleCount(medias[m], &count)) || count == 0) continue;
        std::vector<u64> t(count); u32 rd = 0;
        if (R_FAILED(AM_GetTitleList(&rd, medias[m], count, t.data()))) continue;
        for (u32 i = 0; i < rd; i++) installed.insert(t[i]);
    }
    for (const auto& entry : std::filesystem::directory_iterator(CTR_CONFIG_DIR, ec)) {
        std::string fn = entry.path().filename();
        if (fn.size() != 20 || entry.path().extension() != ".txt") continue; // 16 hex + .txt
        u64 tid = strtoull(fn.substr(0, 16).c_str(), nullptr, 16);
        if (!tid || !installed.count(tid)) continue;   // skip stale (no longer installed)
        std::string content = readEntireFile(entry.path().generic_string());
        std::string name = basenameLower(content);
        if (!name.empty()) out[name] = tid;
    }
    return out;
}

std::vector<ManagedRom> scanManagedRoms(const std::string& romDir) {
    std::vector<ManagedRom> out;
    std::error_code ec;
    if (!std::filesystem::exists(romDir, ec)) return out;
    std::vector<u64> installed = getInstalledTwlTitles();
    std::map<std::string, u64> yanbf = getYanbfForwarders();
    std::map<std::string, u64> rommCtr = getRommCtrForwarders();
    mlog.info("scan: twl(NAND)=" + std::to_string(installed.size()) +
              " yanbf=" + std::to_string(yanbf.size()) +
              " rommCtr=" + std::to_string(rommCtr.size()));
    // dump every installed title that could be a forwarder, so we can see the real scheme
    {
        FS_MediaType M[2] = {MEDIATYPE_SD, MEDIATYPE_NAND};
        const char* MN[2] = {"SD", "NAND"};
        for (int m = 0; m < 2; m++) {
            u32 c = 0; if (R_FAILED(AM_GetTitleCount(M[m], &c)) || !c) continue;
            std::vector<u64> t(c); u32 rd = 0;
            if (R_FAILED(AM_GetTitleList(&rd, M[m], c, t.data()))) continue;
            for (u32 i = 0; i < rd; i++) {
                u32 hi = (u32)(t[i] >> 32), low = (u32)(t[i] & 0xFFFFFFFF);
                bool twl  = (hi == 0x00048004 || hi == 0x00048005);
                bool yan  = (hi == 0x00040000 && low >= 0x0FF40000 && low <= 0x0FF7FFFF);
                bool ctr  = (hi == 0x00040000 && low < 0x0FF40000 && (low >> 8) != 0);
                if (twl || yan || ctr) {
                    char b[48]; snprintf(b, sizeof(b), "%s %016llX %s", MN[m], (unsigned long long)t[i],
                                         twl?"TWL":yan?"YANBF":"CTR");
                    mlog.info(b);
                }
            }
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(romDir, ec)) {
        if (entry.is_directory()) continue;
        std::string filename = entry.path().filename();
        std::string ext = toLowerCase(entry.path().extension().generic_string());
        if (filename[0] == '.' || (ext != ".nds" && ext != ".srl" && ext != ".ids")) continue;
        ManagedRom r;
        r.path = entry.path().generic_string();
        r.display = filename;
        r.sizeBytes = fileSize(r.path);
        r.tid = computeForwarderTID(r.path);
        r.installed = r.tid != 0 && twlTitleInstalled(installed, r.tid);
        auto y = yanbf.find(toLowerCase(filename));
        r.yanbfTid = (y != yanbf.end()) ? y->second : 0;
        auto rc = rommCtr.find(toLowerCase(filename));
        r.rommTid = (rc != rommCtr.end()) ? rc->second : 0;
        {
            char b[128];
            snprintf(b, sizeof(b), "rom twl=%016llX inst=%d y=%d c=%d '%s'",
                     (unsigned long long)r.tid, r.installed?1:0, r.yanbfTid?1:0, r.rommTid?1:0,
                     r.display.substr(0, 44).c_str());
            mlog.info(b);
        }
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const ManagedRom& a, const ManagedRom& b) {
        return toLowerCase(a.display) < toLowerCase(b.display);
    });
    return out;
}

Result deleteForwarder(u64 tid) {
    Result res = AM_DeleteTitle(MEDIATYPE_NAND, tid);
    AM_DeleteTicket(tid);
    return res;
}

Result deleteYanbfForwarder(u64 tid) {
    Result res = AM_DeleteTitle(MEDIATYPE_SD, tid);
    AM_DeleteTicket(tid);
    invalidateYanbfCache();
    return res;
}

Result deleteRommCtrForwarder(u64 tid) {
    Result res = AM_DeleteTitle(MEDIATYPE_SD, tid);
    AM_DeleteTicket(tid);
    char cfg[64];
    snprintf(cfg, sizeof(cfg), "%s%016llX.txt", CTR_CONFIG_DIR.c_str(), tid);
    remove(cfg);
    return res;
}

std::string humanSize(u64 bytes) {
    char buf[32];
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1fGB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0 * 1024));
    else
        snprintf(buf, sizeof(buf), "%lluKB", bytes / 1024);
    return std::string(buf);
}

std::string storageSummary(unsigned long dsiwareCount) {
    FS_ArchiveResource sd = {};
    std::string sdPart = "SD: ?";
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD))) {
        u64 freeBytes = (u64)sd.freeClusters * sd.clusterSize;
        sdPart = "SD free: " + humanSize(freeBytes);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%s | DSiWare: %lu/40", sdPart.c_str(), dsiwareCount);
    return std::string(buf);
}
