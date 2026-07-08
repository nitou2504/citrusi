#include <3ds.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include "ctrbuilder.hpp"
#include "helpers.hpp"
#include "lz11.hpp"
#include "logger.hpp"
#include "settings.hpp"

static Logger ctrLogger("CtrBuilder");

static inline u32 rd32(const std::string& b, u32 o) { u32 v; memcpy(&v, b.data()+o, 4); return v; }
static inline void wr32(std::string& b, u32 o, u32 v) { memcpy(&b[o], &v, 4); }
static inline u64 rd64(const std::string& b, u32 o) { u64 v; memcpy(&v, b.data()+o, 8); return v; }
static inline void wr64(std::string& b, u32 o, u64 v) { memcpy(&b[o], &v, 8); }
static inline void wr64be(std::string& b, u32 o, u64 v) {
    for (int i = 0; i < 8; i++) b[o+i] = (char)((v >> (56 - i*8)) & 0xFF);
}
static inline u32 align40(u32 v) { return (v + 0x3F) & ~0x3F; }
static inline u32 align200(u32 v) { return (v + 0x1FF) & ~0x1FF; }

ReturnResult* CtrBuilder::initialize() {
    if (!fileExists(CTR_TEMPLATE_PATH))
        return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_NDS_NOT_FOUND, "ctr template missing");
    this->tpl = readEntireFile(CTR_TEMPLATE_PATH);
    if (!parseTemplate())
        return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "ctr template parse failed");
    std::filesystem::create_directories(std::filesystem::path(CTR_CONFIG_DIR));
    return new ReturnResult(ERROR_SUCCESS, "");
}

bool CtrBuilder::parseTemplate() {
    if (tpl.size() < 0x2040) return false;
    u32 hdrSize = rd32(tpl, 0x00);
    certSize = rd32(tpl, 0x08);
    tikSize  = rd32(tpl, 0x0C);
    tmdSize  = rd32(tpl, 0x10);
    metaSize = rd32(tpl, 0x14);
    conSize  = rd64(tpl, 0x18);
    u32 o = align40(hdrSize);
    certOff = o; o = align40(o + certSize);
    tikOff  = o; o = align40(o + tikSize);
    tmdOff  = o; o = align40(o + tmdSize);
    conOff  = o; o = align40(o + (u32)conSize);
    metaOff = o;
    if (memcmp(tpl.data()+conOff+0x100, "NCCH", 4) != 0) return false;
    exefsOff  = rd32(tpl, conOff+0x1A0) * 0x200;
    exefsSize = rd32(tpl, conOff+0x1A4) * 0x200;
    parsed = true;
    return true;
}

// ---- TID allocation ----------------------------------------------------

static u32 fnv1a(const std::string& s) {
    u32 h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

static bool ctrTidTaken(u64 tid, const std::string& forPath) {
    char name[64];
    snprintf(name, sizeof(name), "%s%016llX.txt", CTR_CONFIG_DIR.c_str(), tid);
    if (fileExists(name)) {
        std::string cur = readEntireFile(name);
        return cur.find(forPath) == std::string::npos; // taken by another rom
    }
    return false;
}

u64 CtrBuilder::allocateTID(const std::string& fsName) {
    u32 uid = CTR_UID_BASE + (fnv1a(fsName) % CTR_UID_COUNT);
    for (u32 tries = 0; tries < CTR_UID_COUNT; tries++) {
        u64 tid = 0x0004000000000000ULL | ((u64)uid << 8);
        if (!ctrTidTaken(tid, fsName)) return tid;
        uid = CTR_UID_BASE + ((uid - CTR_UID_BASE + 1) % CTR_UID_COUNT);
    }
    return 0;
}

// ---- SMDH --------------------------------------------------------------

static inline u16 bgr555ToRgb565(u16 c) {
    u16 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    return (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
}

static inline u32 mortonIdx(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// writes a WxW RGB565 image (linear) into SMDH tiled layout
static void tileIcon(const u16* linear, u16* out, u32 w) {
    u32 i = 0;
    for (u32 ty = 0; ty < w; ty += 8)
        for (u32 tx = 0; tx < w; tx += 8)
            for (u32 p = 0; p < 64; p++) {
                // find x,y for morton index p
                u32 x = (p & 1) | ((p & 4) >> 1) | ((p & 16) >> 2);
                u32 y = ((p & 2) >> 1) | ((p & 8) >> 2) | ((p & 32) >> 3);
                out[i++] = linear[(ty + y) * w + (tx + x)];
            }
}

// reads DS banner icon (32x32, 4bpp + 16 palette) as linear RGB565 on white
static bool dsIconRgb565(const std::string& ndsPath, u16* out32 /*32*32*/) {
    FILE* f = fopen(ndsPath.c_str(), "rb");
    if (!f) return false;
    u32 bannerOff = 0;
    fseek(f, 0x68, SEEK_SET);
    fread(&bannerOff, 4, 1, f);
    if (!bannerOff) { fclose(f); return false; }
    u8 icon[0x200]; u16 pal[16];
    fseek(f, bannerOff + 0x20, SEEK_SET);
    if (fread(icon, 1, 0x200, f) != 0x200) { fclose(f); return false; }
    fseek(f, bannerOff + 0x220, SEEK_SET);
    if (fread(pal, 2, 16, f) != 16) { fclose(f); return false; }
    fclose(f);
    for (u32 y = 0; y < 32; y++)
        for (u32 x = 0; x < 32; x++) {
            u32 tile = (y / 8) * 4 + (x / 8);
            u32 off = tile * 32 + (y % 8) * 4 + (x % 8) / 2;
            u8 px = icon[off];
            u8 idx = (x & 1) ? (px >> 4) : (px & 0xF);
            out32[y * 32 + x] = (idx == 0) ? 0xFFFF : bgr555ToRgb565(pal[idx]);
        }
    return true;
}

static void utf16Write(std::string& smdh, u32 off, const std::string& utf8, u32 maxChars) {
    u16 buf[0x100] = {0};
    utf8_to_utf16(buf, (const u8*)utf8.c_str(), maxChars - 1);
    memcpy(&smdh[off], buf, maxChars * 2);
}

std::string CtrBuilder::buildSmdh(const std::string& templateSmdh, const std::string& ndsPath,
                                  const std::string& title) {
    std::string smdh = templateSmdh;
    // titles: 16 language slots @0x8, each 0x200: short 0x80, long 0x100, publisher 0x80
    std::string shortT = title.substr(0, 0x3F);
    for (u32 lang = 0; lang < 16; lang++) {
        u32 o = 0x8 + lang * 0x200;
        memset(&smdh[o], 0, 0x200);
        utf16Write(smdh, o,          shortT, 0x40);
        utf16Write(smdh, o + 0x80,   title.substr(0, 0x7F), 0x80);
        utf16Write(smdh, o + 0x180,  "romm3ds", 0x40);
    }
    u16 lin32[32*32];
    if (dsIconRgb565(ndsPath, lin32)) {
        // large 48x48: center 32x32 on white
        static u16 lin48[48*48];
        for (u32 i = 0; i < 48*48; i++) lin48[i] = 0xFFFF;
        for (u32 y = 0; y < 32; y++)
            for (u32 x = 0; x < 32; x++)
                lin48[(y+8)*48 + (x+8)] = lin32[y*32+x];
        // small 24x24: nearest downscale of 32
        static u16 lin24[24*24];
        for (u32 y = 0; y < 24; y++)
            for (u32 x = 0; x < 24; x++)
                lin24[y*24+x] = lin32[(y*32/24)*32 + (x*32/24)];
        tileIcon(lin24, (u16*)&smdh[0x2040], 24);
        tileIcon(lin48, (u16*)&smdh[0x24C0], 48);
    }
    return smdh;
}

// ---- banner ------------------------------------------------------------

// bannertool banners have a constant 0x1580-byte CGFX header followed by the
// 256x128 RGBA4444 texture (0x10000 bytes) — see bannertool data.h
#define BANNER_TEX_OFFSET 0x1580
#define BANNER_TEX_SIZE   0x10000

std::string CtrBuilder::buildBanner(const std::string& templateBanner,
                                    const std::string& etc1a4, const std::string& cwav) {
    if (etc1a4.empty() && cwav.empty()) return templateBanner;
    u32 cwavOff = rd32(templateBanner, 0x84);
    std::string hdr = templateBanner.substr(0, 0x88);
    std::string cgfxLz = templateBanner.substr(0x88, cwavOff - 0x88);
    std::string sound = cwav.empty() ? templateBanner.substr(cwavOff) : cwav;

    if (!etc1a4.empty()) {
        std::string cgfx = lz11Decompress(cgfxLz);
        if (cgfx.size() >= BANNER_TEX_OFFSET + BANNER_TEX_SIZE &&
            memcmp(cgfx.data(), "CGFX", 4) == 0 &&
            etc1a4.size() == BANNER_TEX_SIZE) {
            cgfx.replace(BANNER_TEX_OFFSET, BANNER_TEX_SIZE, etc1a4);
            cgfxLz = lz11StoreCompress(cgfx);
        } else {
            ctrLogger.error("banner texture patch failed, keeping template art");
        }
    }
    u32 newCwavOff = 0x88 + (u32)cgfxLz.size();
    wr32(hdr, 0x84, newCwavOff);
    return hdr + cgfxLz + sound;
}

// ---- main build --------------------------------------------------------

static bool titleInstalledOn(FS_MediaType media, u64 tid) {
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(media, &count)) || count == 0) return false;
    std::vector<u64> titles(count);
    u32 read = 0;
    if (R_FAILED(AM_GetTitleList(&read, media, count, titles.data()))) return false;
    for (u32 i = 0; i < read; i++) if (titles[i] == tid) return true;
    return false;
}

ReturnResult* CtrBuilder::buildCIA(const std::string& romPath, const std::string& title,
                                   u64 tid, const std::string& etc1a4, const std::string& cwav) {
    if (!parsed) return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "template not loaded");

    // --- pull template pieces
    std::string content = tpl.substr(conOff, (u32)conSize);
    std::string exefsHdr = content.substr(exefsOff, 0x200);
    struct EFile { char name[9]; u32 off; u32 size; };
    std::vector<EFile> files;
    for (int i = 0; i < 10; i++) {
        EFile ef = {};
        memcpy(ef.name, exefsHdr.data() + i*16, 8);
        if (!ef.name[0]) continue;
        ef.off = rd32(exefsHdr, i*16 + 8);
        ef.size = rd32(exefsHdr, i*16 + 12);
        files.push_back(ef);
    }

    // --- new icon (SMDH) + banner
    std::string newSmdh, newBanner;
    for (auto& ef : files) {
        std::string data = content.substr(exefsOff + 0x200 + ef.off, ef.size);
        if (strcmp(ef.name, "icon") == 0)   newSmdh   = buildSmdh(data, romPath, title);
        if (strcmp(ef.name, "banner") == 0) newBanner = buildBanner(data, etc1a4, cwav);
    }
    if (newSmdh.empty() || newBanner.empty())
        return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "icon/banner missing in template");

    // --- rebuild exefs
    std::string exefsData;
    std::string newHdr(0x200, '\0');
    u32 cur = 0;
    for (size_t i = 0; i < files.size(); i++) {
        std::string data;
        if (strcmp(files[i].name, "icon") == 0)        data = newSmdh;
        else if (strcmp(files[i].name, "banner") == 0) data = newBanner;
        else data = content.substr(exefsOff + 0x200 + files[i].off, files[i].size);

        memcpy(&newHdr[i*16], files[i].name, 8);
        wr32(newHdr, i*16 + 8, cur);
        wr32(newHdr, i*16 + 12, (u32)data.size());
        std::string hash = sha256((u8*)data.c_str(), data.size());
        memcpy(&newHdr[0x200 - (i+1)*0x20], hash.data(), 0x20);

        exefsData += data;
        u32 padded = align200((u32)data.size());
        exefsData += std::string(padded - data.size(), '\0');
        cur += padded;
    }
    std::string newExefs = newHdr + exefsData;
    // pad exefs to media unit
    newExefs += std::string(align200((u32)newExefs.size()) - newExefs.size(), '\0');

    // --- rebuild NCCH content
    std::string ncch = content.substr(0, exefsOff) + newExefs;
    u32 newConSize = (u32)ncch.size();
    wr32(ncch, 0x104, newConSize / 0x200);          // content size (mu)
    wr64(ncch, 0x108, tid);                          // partition id
    wr64(ncch, 0x118, tid);                          // program id
    wr32(ncch, 0x1A4, (u32)newExefs.size() / 0x200); // exefs size (mu)
    // exheader ids
    wr64(ncch, 0x200 + 0x1C8, tid);                  // SCI jump id
    wr64(ncch, 0x200 + 0x200, tid);                  // ACI program id
    wr64(ncch, 0x200 + 0x600, tid);                  // AccessDesc ACI copy
    // exheader hash (first 0x400)
    std::string exhHash = sha256((u8*)ncch.data() + 0x200, 0x400);
    memcpy(&ncch[0x160], exhHash.data(), 0x20);
    // exefs superblock hash
    u32 hashRegion = rd32(ncch, 0x1A8) * 0x200;
    if (!hashRegion) hashRegion = 0x200;
    std::string efsHash = sha256((u8*)ncch.data() + exefsOff, hashRegion);
    memcpy(&ncch[0x1C0], efsHash.data(), 0x20);

    // --- ticket + tmd patches
    std::string tik = tpl.substr(tikOff, tikSize);
    wr64be(tik, 0x1DC, tid);
    std::string tmd = tpl.substr(tmdOff, tmdSize);
    wr64be(tmd, 0x18C, tid);
    wr64be(tmd, 0xB04 + 0x8, (u64)newConSize);       // chunk size (BE)
    std::string conHash = sha256((u8*)ncch.data(), ncch.size());
    memcpy(&tmd[0xB04 + 0x10], conHash.data(), 0x20);
    std::string chunkHash = sha256((u8*)tmd.data() + 0xB04, 0x30);
    memcpy(&tmd[0x204 + 0x4], chunkHash.data(), 0x20);
    std::string infoHash = sha256((u8*)tmd.data() + 0x204, 0x900);
    memcpy(&tmd[0x1E4], infoHash.data(), 0x20);

    // --- meta (smdh copy at meta+0x400)
    std::string meta = tpl.substr(metaOff, metaSize);
    if (metaSize >= 0x400 + 0x36C0)
        meta.replace(0x400, 0x36C0, newSmdh);

    // --- assemble CIA
    std::string hdr = tpl.substr(0, rd32(tpl, 0x00));
    wr64(hdr, 0x18, (u64)newConSize);
    std::string cia = aligned(hdr, 0x40) + aligned(tpl.substr(certOff, certSize), 0x40) +
                      aligned(tik, 0x40) + aligned(tmd, 0x40) + aligned(ncch, 0x40) + meta;

    // --- install to SD media
    FS_ArchiveResource sd = {};
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD))) {
        u64 freeBytes = (u64)sd.freeClusters * sd.clusterSize;
        if (freeBytes < cia.size())
            return new ReturnResult(ERROR_NOT_ENOUGH_SPACE, "SD full");
    }
    if (titleInstalledOn(MEDIATYPE_SD, tid)) {
        AM_DeleteTitle(MEDIATYPE_SD, tid);
        AM_DeleteTicket(tid);
    }
    Handle h = {};
    Result ret = AM_StartCiaInstall(MEDIATYPE_SD, &h);
    if (R_FAILED(ret)) return new ReturnResult(ret, "AM_StartCiaInstall failed");
    u32 written = 0, offset = 0;
    while (offset < cia.size()) {
        u32 chunk = std::min((u32)0x10000, (u32)cia.size() - offset);
        ret = FSFILE_Write(h, &written, offset, cia.data() + offset, chunk, FS_WRITE_FLUSH);
        if (R_FAILED(ret)) {
            AM_CancelCIAInstall(h);
            return new ReturnResult(ret, "CIA write failed");
        }
        offset += written;
    }
    ret = AM_FinishCiaInstall(h);
    if (R_FAILED(ret)) return new ReturnResult(ret, "AM_FinishCiaInstall failed");

    // --- path file for the payload
    char cfg[64];
    snprintf(cfg, sizeof(cfg), "%s%016llX.txt", CTR_CONFIG_DIR.c_str(), tid);
    // match the known-working YANBF format: "/roms/nds/x.nds" (no sd: prefix)
    std::string launchPath = romPath;
    if (launchPath.rfind("sdmc:", 0) == 0) launchPath = launchPath.substr(5);
    std::ofstream o(cfg);
    o << launchPath;
    o.close();

    return new ReturnResult(ERROR_SUCCESS, "");
}
