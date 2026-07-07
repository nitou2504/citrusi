#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include "manage.hpp"
#include "helpers.hpp"

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

std::vector<ManagedRom> scanManagedRoms(const std::string& romDir) {
    std::vector<ManagedRom> out;
    std::error_code ec;
    if (!std::filesystem::exists(romDir, ec)) return out;
    std::vector<u64> installed = getInstalledTwlTitles();
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
