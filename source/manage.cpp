#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
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
static std::map<std::string, std::string> gYanbfOrphans;

void invalidateYanbfCache() { gYanbfCached = false; }

std::map<std::string, std::string> getOrphanForwarderCias() {
    if (!gYanbfCached) getYanbfForwarders();
    return gYanbfOrphans;
}

// FS refuses to open another title's RomFS from userland (0xD9004676,
// "command not allowed"), even with a full-access exheader. probe whether the
// ExeFS icon is readable — if it ever is, an SMDH-name fallback becomes viable.
static Result probeExefsIcon(u64 tid) {
    u32 archPathData[4] = {(u32)tid, (u32)(tid >> 32), MEDIATYPE_SD, 0};
    u32 filePathData[5] = {0, 0, 2, 0x6E6F6369 /*'icon'*/, 0};
    FS_Path archPath = {PATH_BINARY, sizeof(archPathData), archPathData};
    FS_Path filePath = {PATH_BINARY, sizeof(filePathData), filePathData};
    Handle fd = 0;
    Result rc = FSUSER_OpenFileDirectly(&fd, ARCHIVE_SAVEDATA_AND_CONTENT,
                                        archPath, filePath, FS_OPEN_READ, 0);
    if (R_SUCCEEDED(rc)) svcCloseHandle(fd);
    return rc;
}

static bool readAt(FILE* f, long off, void* buf, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

// parse a plaintext (NoCrypto) forwarder .cia left behind by the YANBF PC
// generator: pull the NCCH program id and the rom filename stored in
// romfs:/path.txt. plain SD file reads — no FS title-content perms needed.
static bool parseForwarderCia(const std::string& ciaPath, u64& tidOut, std::string& nameOut) {
    FILE* f = fopen(ciaPath.c_str(), "rb");
    if (!f) return false;
    bool found = false;
    do {
        // cia header: 0x00 hdrSize, 0x08 certSize, 0x0C tikSize, 0x10 tmdSize;
        // sections are 0x40-aligned, content follows tmd
        u32 hdr[5];
        if (!readAt(f, 0, hdr, sizeof(hdr))) break;
        auto align64 = [](u32 v) { return (long)((v + 0x3F) & ~0x3Fu); };
        long ncch = align64(hdr[0]) + align64(hdr[2]) + align64(hdr[3]) + align64(hdr[4]);
        char magic[4];
        if (!readAt(f, ncch + 0x100, magic, 4) || memcmp(magic, "NCCH", 4) != 0) break;
        u8 flags[8];
        if (!readAt(f, ncch + 0x188, flags, 8) || !(flags[7] & 0x4)) break; // encrypted
        u64 tid = 0; u32 romfsOff = 0, romfsSize = 0;
        if (!readAt(f, ncch + 0x118, &tid, 8)) break;
        if (!readAt(f, ncch + 0x1B0, &romfsOff, 4) || !readAt(f, ncch + 0x1B4, &romfsSize, 4)) break;
        if (!romfsOff || !romfsSize) break;
        long romfs = ncch + (long)romfsOff * 0x200;
        if (!readAt(f, romfs, magic, 4) || memcmp(magic, "IVFC", 4) != 0) break;
        // romfs level3 header at +0x1000: [0]=0x28 len, [7]=fileMetaOff,
        // [8]=fileMetaLen, [9]=fileDataOff (all relative to level3)
        long lvl3 = romfs + 0x1000;
        u32 l3[10];
        if (!readAt(f, lvl3, l3, sizeof(l3)) || l3[0] != 0x28) break;
        u32 pos = 0;
        while (pos + 0x20 <= l3[8]) {
            u8 ent[0x20];
            if (!readAt(f, lvl3 + l3[7] + pos, ent, 0x20)) break;
            u64 dataOff, dataSize; u32 nameLen;
            memcpy(&dataOff, ent + 0x08, 8);
            memcpy(&dataSize, ent + 0x10, 8);
            memcpy(&nameLen, ent + 0x1C, 4);
            if (nameLen > 0x200) break;
            std::vector<u8> nm(nameLen);
            if (nameLen && !readAt(f, lvl3 + l3[7] + pos + 0x20, nm.data(), nameLen)) break;
            std::string ascii;
            for (u32 i = 0; i + 1 < nameLen; i += 2)
                if (nm[i + 1] == 0) ascii += (char)tolower(nm[i]);
            if (ascii == "path.txt" && dataSize > 0 && dataSize < 512) {
                std::vector<char> data(dataSize + 1, 0);
                if (readAt(f, lvl3 + l3[9] + (long)dataOff, data.data(), dataSize)) {
                    nameOut = basenameLower(std::string(data.data()));
                    tidOut = tid;
                    found = !nameOut.empty();
                }
                break;
            }
            pos += 0x20 + ((nameLen + 3) & ~3u);
        }
    } while (0);
    fclose(f);
    return found;
}

// fallback detection: match installed YANBF-range titles against the
// generator's leftover .cia files on SD. cias whose title is not installed
// at all are collected as orphans (installable later).
static void scanForwarderCias(const std::set<u64>& wanted, const std::set<u64>& installedSd,
                              std::map<std::string, u64>& out,
                              std::map<std::string, std::string>& orphans) {
    static const char* dirs[] = {"sdmc:/cias", "sdmc:/cia"};
    std::set<u64> matched;
    for (const char* d : dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(d, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(d, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it.depth() > 2 || it->is_directory(ec)) continue;
            std::string p = it->path().generic_string();
            if (toLowerCase(it->path().extension().generic_string()) != ".cia") continue;
            u64 tid = 0; std::string name;
            if (!parseForwarderCia(p, tid, name)) continue;
            u32 low = (u32)(tid & 0xFFFFFFFF);
            if ((tid >> 32) != 0x00040000ULL || low < 0x0FF40000 || low > 0x0FF7FFFF) continue;
            if (!installedSd.count(tid)) {
                if (!orphans.count(name)) {
                    orphans[name] = p;
                    char b[96];
                    snprintf(b, sizeof(b), "orphan cia %08lX '%s'", low, name.substr(0, 58).c_str());
                    mlog.info(b);
                }
                continue;
            }
            if (!wanted.count(tid) || matched.count(tid)) continue;
            matched.insert(tid);
            out[name] = tid;
            char b[96];
            snprintf(b, sizeof(b), "yanbf cia %08lX '%s'", low, name.substr(0, 60).c_str());
            mlog.info(b);
        }
    }
}

std::map<std::string, u64> getYanbfForwarders() {
    if (gYanbfCached) return gYanbfCache;
    std::map<std::string, u64> out;
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count)) || count == 0) {
        gYanbfOrphans.clear();
        scanForwarderCias({}, {}, out, gYanbfOrphans);
        gYanbfCache = out; gYanbfCached = true; return out;
    }
    std::vector<u64> titles(count);
    u32 read = 0;
    if (R_FAILED(AM_GetTitleList(&read, MEDIATYPE_SD, count, titles.data()))) return out;
    std::set<u64> unresolved;
    std::set<u64> allSd(titles.begin(), titles.begin() + read);
    for (u32 i = 0; i < read; i++) {
        u64 tid = titles[i];
        if ((tid >> 32) != 0x00040000ULL) continue;
        u32 low = (u32)(tid & 0xFFFFFFFF);
        if (low < 0x0FF40000 || low > 0x0FF7FFFF) continue;
        // YANBF forwarder: romfs holds path.txt with the rom path.
        // FS denies this from userland (0xD9004676) on stock+Luma; kept in
        // case some setup allows it, real work happens in the cia fallback.
        Result mrc = romfsMountFromTitle(tid, MEDIATYPE_SD, "yfwd");
        if (R_FAILED(mrc)) {
            if (unresolved.empty()) {
                char b[80];
                snprintf(b, sizeof(b), "yanbf mount %08lX rc=%08lX icon-probe rc=%08lX",
                         low, (unsigned long)mrc, (unsigned long)probeExefsIcon(tid));
                mlog.info(b);
            }
            unresolved.insert(tid);
            continue;
        }
        FILE* f = fopen("yfwd:/path.txt", "rb");
        if (f) {
            char buf[512] = {0};
            fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            std::string name = basenameLower(std::string(buf));
            if (!name.empty()) out[name] = tid;
            else unresolved.insert(tid);
        } else {
            unresolved.insert(tid);
        }
        romfsUnmount("yfwd");
    }
    gYanbfOrphans.clear();
    scanForwarderCias(unresolved, allSd, out, gYanbfOrphans);
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
    std::map<std::string, std::string> orphans = getOrphanForwarderCias();
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
        auto o = orphans.find(toLowerCase(filename));
        r.orphanCia = (o != orphans.end()) ? o->second : "";
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
