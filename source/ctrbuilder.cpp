#include <3ds.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <map>
#include <vector>
#include <algorithm>
#include "ctrbuilder.hpp"
#include "ciainstall.hpp"
#include "installedtitles.hpp"
#include "helpers.hpp"
#include "lz11.hpp"
#include "logger.hpp"
#include "settings.hpp"
#include "cwav.hpp"

static Logger ctrLogger("CtrBuilder");

// untile the baked 256x128 RGBA4444 banner into linear RGBA8 and keep it on
// SD (banners-cache/<tid>.raw, 128KB) so Manage can show what's actually
// installed. Only written on a successful (re)bake.
static void saveBannerPreview(u64 tid, const std::string& tex) {
    if (tex.size() != 256 * 128 * 2) return;
    std::error_code ec;
    std::filesystem::create_directories(FORWARDER_DIR + "/banners-cache", ec);
    std::string out(256 * 128 * 4, '\0');
    const u16* src = (const u16*)tex.data();
    u8* dst = (u8*)&out[0];
    for (u32 y = 0; y < 128; y++) {
        for (u32 x = 0; x < 256; x++) {
            u32 index = (((y >> 3) * (256 >> 3) + (x >> 3)) << 6) +
                        ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                         ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
            u16 c = src[index];
            u8* o = &dst[(y * 256 + x) * 4];
            o[0] = (u8)(((c >> 12) & 0xF) * 17);
            o[1] = (u8)(((c >>  8) & 0xF) * 17);
            o[2] = (u8)(((c >>  4) & 0xF) * 17);
            o[3] = (u8)(( c        & 0xF) * 17);
        }
    }
    std::string p = FORWARDER_DIR + "/banners-cache/";
    char name[24];
    snprintf(name, sizeof(name), "%016llX.raw", (unsigned long long)tid);
    FILE* f = fopen((p + name).c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

static std::string bannerPreviewPath(u64 tid) {
    char name[24];
    snprintf(name, sizeof(name), "%016llX.raw", (unsigned long long)tid);
    return FORWARDER_DIR + "/banners-cache/" + name;
}

static inline u32 rd32(const std::string& b, u32 o) { u32 v; memcpy(&v, b.data()+o, 4); return v; }
static inline void wr32(std::string& b, u32 o, u32 v) { memcpy(&b[o], &v, 4); }
static inline u64 rd64(const std::string& b, u32 o) { u64 v; memcpy(&v, b.data()+o, 8); return v; }
static inline void wr64(std::string& b, u32 o, u64 v) { memcpy(&b[o], &v, 8); }
static inline void wr64be(std::string& b, u32 o, u64 v) {
    for (int i = 0; i < 8; i++) b[o+i] = (char)((v >> (56 - i*8)) & 0xFF);
}
static inline u32 align40(u32 v) { return (v + 0x3F) & ~0x3F; }
static inline u32 align200(u32 v) { return (v + 0x1FF) & ~0x1FF; }

// GBA template pieces, read from romfs once at init — re-reading them per
// build meant a romfs open could fail mid-batch (and used to crash)
static std::string tplNcch, tplExh, tplIcn, tplLogo, tplRfs, tplCfg, tplBnr, tplGbaChime;
static void loadGbaTemplates() {
    if (!tplNcch.empty()) return;
    tplNcch = readEntireFile(GBA_TPL_DIR + "ncchheader.bin");
    tplExh  = readEntireFile(GBA_TPL_DIR + "exheader.bin");
    tplIcn  = readEntireFile(GBA_TPL_DIR + "icon.icn");
    tplLogo = readEntireFile(GBA_TPL_DIR + "logo.darc.lz");
    tplRfs  = readEntireFile(GBA_TPL_DIR + "romfs.bin");
    tplCfg      = readEntireFile(GBA_TPL_DIR + "config_block.bin");
    tplBnr      = readEntireFile(GBA_TPL_DIR + "template.bnr");
    tplGbaChime = wavToCwav(readEntireFile(GBA_TPL_DIR + "gba-chime.wav"));
}

ReturnResult* CtrBuilder::initialize() {
    if (!fileExists(CTR_TEMPLATE_PATH))
        return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_NDS_NOT_FOUND, "ctr template missing");
    this->tpl = readEntireFile(CTR_TEMPLATE_PATH);
    if (!parseTemplate())
        return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "ctr template parse failed");
    std::filesystem::create_directories(std::filesystem::path(CTR_CONFIG_DIR));
    loadGbaTemplates();
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

// ownership files (<TID>.txt in CTR_CONFIG_DIR) cached in RAM: tid lookups
// happen per library row (175+ per menu build) and a fileExists each was the
// dominant cost of opening the GBA library.
static std::map<u64, std::string>* gTidOwners = nullptr;
static void loadTidOwners() {
    if (gTidOwners) return;
    gTidOwners = new std::map<u64, std::string>();
    std::error_code ec;
    for (auto& de : std::filesystem::directory_iterator(CTR_CONFIG_DIR, ec)) {
        std::string name = de.path().filename().generic_string();
        if (name.size() != 20 || name.compare(16, 4, ".txt") != 0) continue;
        u64 tid = strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        if (tid) (*gTidOwners)[tid] = readEntireFile(de.path().generic_string());
    }
}
void ctrTidCacheInvalidate() {
    delete gTidOwners;
    gTidOwners = nullptr;
}

static bool ctrTidTaken(u64 tid, const std::string& forPath) {
    loadTidOwners();
    auto it = gTidOwners->find(tid);
    if (it == gTidOwners->end()) return false;
    return it->second.find(forPath) == std::string::npos; // taken by another rom
}

u64 CtrBuilder::allocateTIDIn(u32 base, u32 count, const std::string& fsName) {
    u32 uid = base + (fnv1a(fsName) % count);
    for (u32 tries = 0; tries < count; tries++) {
        u64 tid = 0x0004000000000000ULL | ((u64)uid << 8);
        if (!ctrTidTaken(tid, fsName)) return tid;
        uid = base + ((uid - base + 1) % count);
    }
    return 0;
}

u64 CtrBuilder::allocateTID(const std::string& fsName) {
    return allocateTIDIn(CTR_UID_BASE, CTR_UID_COUNT, fsName);
}

u64 CtrBuilder::allocateGbaTID(const std::string& fsName) {
    return allocateTIDIn(GBA_UID_BASE, GBA_UID_COUNT, fsName);
}

// read-only: the tid a GBA ROM would occupy (its own slot if already claimed,
// else the first free one). Lets the library show real install state via AM
// instead of "is the .gba still on SD".
u64 gbaTidForRom(const std::string& romBase) {
    u32 uid = GBA_UID_BASE + (fnv1a(romBase) % GBA_UID_COUNT);
    for (u32 tries = 0; tries < GBA_UID_COUNT; tries++) {
        u64 tid = 0x0004000000000000ULL | ((u64)uid << 8);
        if (!ctrTidTaken(tid, romBase)) return tid;
        uid = GBA_UID_BASE + ((uid - GBA_UID_BASE + 1) % GBA_UID_COUNT);
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
                                  const std::string& title, const std::string& icon48) {
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
    if (icon48.size() == 48*48*2) {
        // custom icon (art picker): 48x48 as-is + nearest 24x24
        const u16* lin48c = (const u16*)icon48.data();
        static u16 lin24c[24*24];
        for (u32 y = 0; y < 24; y++)
            for (u32 x = 0; x < 24; x++)
                lin24c[y*24+x] = lin48c[(y*48/24)*48 + (x*48/24)];
        tileIcon(lin24c, (u16*)&smdh[0x2040], 24);
        tileIcon(lin48c, (u16*)&smdh[0x24C0], 48);
        return smdh;
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

bool ensureBannerPreview(u64 tid) {
    // only our own template-based banners have the texture at the known
    // offset — retail CGFX layouts differ and would untile to noise
    if ((u32)(tid >> 32) != 0x00040000 || (tid & 0xFF)) return false;
    u32 u24 = (u32)((tid >> 8) & 0xFFFFFF);
    if (u24 < 0xFF400) return false;
    std::string full = bannerPreviewPath(tid);
    if (fileExists(full)) return true;
    // exefs:/banner of the installed SD title (readable cross-title; only
    // RomFS is blocked — same trick as the SMDH icon reads)
    u32 archPath[4] = { (u32)(tid & 0xFFFFFFFF), (u32)(tid >> 32), MEDIATYPE_SD, 0 };
    u32 filePath[5] = { 0, 0, 2, 0x6E6E6162 /*'bann'*/, 0x00007265 /*'er'*/ };
    FS_Path ap = { PATH_BINARY, sizeof(archPath), archPath };
    FS_Path fp = { PATH_BINARY, sizeof(filePath), filePath };
    Handle h = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0)))
        return false;
    u64 sz = 0;
    if (R_FAILED(FSFILE_GetSize(h, &sz)) || sz < 0x90 || sz > 4 * 1024 * 1024) {
        FSFILE_Close(h);
        return false;
    }
    std::string bnr((size_t)sz, '\0');
    u32 read = 0;
    Result rr = FSFILE_Read(h, &read, 0, &bnr[0], (u32)sz);
    FSFILE_Close(h);
    if (R_FAILED(rr) || read != sz) return false;
    if (memcmp(bnr.data(), "CBMD", 4) != 0) return false;
    u32 cwavOff = rd32(bnr, 0x84);
    if (cwavOff <= 0x88 || cwavOff > sz) return false;
    std::string cgfx = lz11Decompress(bnr.substr(0x88, cwavOff - 0x88));
    if (cgfx.size() < BANNER_TEX_OFFSET + BANNER_TEX_SIZE ||
        memcmp(cgfx.data(), "CGFX", 4) != 0)
        return false;
    saveBannerPreview(tid, cgfx.substr(BANNER_TEX_OFFSET, BANNER_TEX_SIZE));
    return fileExists(full);
}

ReturnResult* CtrBuilder::buildCIA(const std::string& romPath, const std::string& title,
                                   u64 tid, const std::string& etc1a4, const std::string& cwav,
                                   const std::string& icon48) {
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
        if (strcmp(ef.name, "icon") == 0)   newSmdh   = buildSmdh(data, romPath, title, icon48);
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
    // version bump on rebuilds — HOME caches icons per (tid, version)
    {
        u16 ver = 1;
        AM_TitleEntry te;
        u64 id = tid;
        if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &id, &te)))
            ver = (u16)(te.version + 1);
        tmd[0x1DC] = (char)(ver >> 8);
        tmd[0x1DD] = (char)(ver & 0xFF);
        tik[0x1E6] = (char)(ver >> 8);
        tik[0x1E7] = (char)(ver & 0xFF);
    }
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
    // overwrite-install when the tid is already there: keeps the save data
    // and dodges AM's "already exists" (0xC8E083FC) on back-to-back rebuilds
    auto streamCia = [&](bool ovw) -> Result {
        Handle h = {};
        Result ret = ovw ? AM_StartCiaInstallOverwrite(&h, MEDIATYPE_SD)
                         : AM_StartCiaInstall(MEDIATYPE_SD, &h);
        if (R_FAILED(ret)) return ret;
        u32 written = 0, offset = 0;
        while (offset < cia.size()) {
            u32 chunk = std::min((u32)0x10000, (u32)cia.size() - offset);
            ret = FSFILE_Write(h, &written, offset, cia.data() + offset, chunk, FS_WRITE_FLUSH);
            if (R_FAILED(ret)) {
                AM_CancelCIAInstall(h);
                return ret;
            }
            offset += written;
        }
        return AM_FinishCiaInstall(h);
    };
    Result ret = streamCia(titleInstalledOn(MEDIATYPE_SD, tid));
    if ((u32)ret == 0xC8E083FC) {   // stale title/ticket/pending: clear + retry once
        AM_DeleteTitle(MEDIATYPE_SD, tid);
        AM_DeleteTicket(tid);
        AM_DeletePendingTitle(MEDIATYPE_SD, tid);
        ret = streamCia(false);
    }
    if (R_FAILED(ret)) return new ReturnResult(ret, "CIA install failed");

    // --- path file for the payload
    char cfg[64];
    snprintf(cfg, sizeof(cfg), "%s%016llX.txt", CTR_CONFIG_DIR.c_str(), tid);
    // nds-bootstrap needs the "sd:" device prefix (verified against a working
    // YANBF launch: NDS_PATH = sd:/roms/nds/...)
    std::string launchPath = romPath;
    if (launchPath.rfind("sdmc:", 0) == 0) launchPath = "sd:" + launchPath.substr(5);
    std::ofstream o(cfg);
    o << launchPath;
    o.close();
    ctrTidCacheInvalidate();
    installedTitlesInvalidate();      // new title: refresh tally/icon caches
    saveBannerPreview(tid, etc1a4);   // same template texture layout as GBA
    titleIconInvalidate(tid);         // SMDH may carry new art

    return new ReturnResult(ERROR_SUCCESS, "");
}

// ---- GBA VC inject (vcoven layout, assembled in-memory + streamed) -------

#include "mbedtls/sha256.h"

struct StreamSha {
    mbedtls_sha256_context c;
    StreamSha() { mbedtls_sha256_init(&c); mbedtls_sha256_starts_ret(&c, 0); }
    void update(const void* d, size_t n) { mbedtls_sha256_update_ret(&c, (const u8*)d, n); }
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::string finish() {
        u8 h[32];
        mbedtls_sha256_finish_ret(&c, h);
        mbedtls_sha256_free(&c);
        return std::string((char*)h, 32);
    }
};

// Save-type detection ported from open_agb_firm (profi200): primary source is
// gba_db.bin (SHA1-keyed, 28-byte entries), fallback is the SDK version-string
// LUT. Types are the AGB_FIRM footer ids 0x0-0xF (lgy_common.h).
#define GBA_SAVE_NONE 0xFF   // config-block value for "no save"

#include "mbedtls/sha1.h"

struct StreamSha1 {
    mbedtls_sha1_context c;
    StreamSha1() { mbedtls_sha1_init(&c); mbedtls_sha1_starts_ret(&c); }
    void update(const void* d, size_t n) { mbedtls_sha1_update_ret(&c, (const u8*)d, n); }
    void finish(u8 out[20]) { mbedtls_sha1_finish_ret(&c, out); mbedtls_sha1_free(&c); }
};

static const struct { const char* sig; u8 type; } gbaSigLut[] = {
    // EEPROM — size is not knowable from the signature; per-version defaults
    // from open_agb_firm (common sizes for popular games / ROM hacks)
    {"EEPROM_V111", 0x0}, {"EEPROM_V120", 0x0}, {"EEPROM_V121", 0x2},
    {"EEPROM_V122", 0x0}, {"EEPROM_V124", 0x2}, {"EEPROM_V125", 0x0},
    {"EEPROM_V126", 0x0},
    // FLASH 512k — assume Panasonic with RTC
    {"FLASH_V120", 0x8}, {"FLASH_V121", 0x8}, {"FLASH_V123", 0x8},
    {"FLASH_V124", 0x8}, {"FLASH_V125", 0x8}, {"FLASH_V126", 0x8},
    {"FLASH512_V130", 0x8}, {"FLASH512_V131", 0x8}, {"FLASH512_V133", 0x8},
    // FLASH 1M — assume Macronix with RTC
    {"FLASH1M_V102", 0xA}, {"FLASH1M_V103", 0xA},
    // FRAM & SRAM
    {"SRAM_F_V100", 0xE}, {"SRAM_F_V102", 0xE}, {"SRAM_F_V103", 0xE},
    {"SRAM_V110", 0xE}, {"SRAM_V111", 0xE}, {"SRAM_V112", 0xE},
    {"SRAM_V113", 0xE},
};
#define GBA_SIG_COUNT (sizeof(gbaSigLut)/sizeof(gbaSigLut[0]))

// scan pass state: earliest versioned save signature in ROM order wins
struct GbaSaveScan {
    int found = -1;
    void feed(const char* buf, size_t n) {
        if (found >= 0) return;
        size_t best = (size_t)-1;
        for (u32 i = 0; i < GBA_SIG_COUNT; i++) {
            const void* p = memmem(buf, n, gbaSigLut[i].sig, strlen(gbaSigLut[i].sig));
            if (p && (size_t)((const char*)p - buf) < best) {
                best = (const char*)p - buf;
                found = (int)i;
            }
        }
    }
    u8 result(u32 romSize) const {
        if (found < 0) return GBA_SAVE_NONE;
        u8 t = gbaSigLut[found].type;
        // ROMs over 16 MiB need the "_2" EEPROM variants (save in upper 0x100)
        if ((t == 0x0 || t == 0x2) && romSize > 0x1000000) t++;
        return t;
    }
};

// gba_db.bin: 28-byte entries = SHA1[20] + serial[4] + attr[4], sorted by the
// first u64 of the SHA1. Load once, reuse for both lookups.
static const std::string& gbaDb() {
    static std::string db = readEntireFile(GBA_TPL_DIR + "gba_db.bin");
    return db;
}

// exact per-ROM lookup by SHA1 (binary search); -1 = not found
static int gbaDbSaveType(const u8 sha1[20]) {
    const std::string& db = gbaDb();
    if (db.size() < 28 || db.size() % 28) return -1;
    u64 key; memcpy(&key, sha1, 8);
    u32 l = 0, r = db.size() / 28;
    while (l < r) {
        u32 m = l + (r - l) / 2;
        u64 k; memcpy(&k, db.data() + m*28, 8);
        if (key > k) l = m + 1;
        else if (key < k) r = m;
        else {
            u32 attr; memcpy(&attr, db.data() + m*28 + 24, 4);
            return (int)(attr & 0xF);
        }
    }
    return -1;
}

// lookup by 4-char cart serial (linear scan). Translation/hack patches change
// the ROM bytes (SHA1 misses) but keep the serial, so the untranslated
// original's save type is recoverable. Only trust it when every entry sharing
// the serial agrees; -1 on miss or conflict.
static int gbaDbSaveTypeBySerial(const char serial[4]) {
    const std::string& db = gbaDb();
    if (db.size() < 28 || db.size() % 28) return -1;
    int found = -1;
    for (size_t o = 0; o + 28 <= db.size(); o += 28) {
        if (memcmp(db.data() + o + 20, serial, 4) != 0) continue;
        int t = db[o + 24] & 0xF;
        if (found < 0) found = t;
        else if (found != t) return -1;   // ambiguous serial → don't guess
    }
    return found;
}

#define GBA_CHUNK 0x40000
#define GBA_SIG_OVERLAP 16

// streams the ROM once: save-signature scan + optional hashes over [rom][tail]
static bool gbaRomPass(const std::string& romPath, u32 romSize, GbaSaveScan* scan,
                       StreamSha* sha, const std::string& tail, StreamSha1* sha1 = nullptr) {
    FILE* f = fopen(romPath.c_str(), "rb");
    if (!f) return false;
    static char buf[GBA_CHUNK + GBA_SIG_OVERLAP];
    size_t carry = 0;
    u32 left = romSize;
    while (left > 0) {
        u32 want = left < GBA_CHUNK ? left : GBA_CHUNK;
        size_t n = fread(buf + carry, 1, want, f);
        if (n != want) { fclose(f); return false; }
        if (scan) scan->feed(buf, carry + n);
        if (sha) sha->update(buf + carry, n);
        if (sha1) sha1->update(buf + carry, n);
        // keep the last bytes so signatures across chunk borders still match
        if (scan) {
            size_t keep = (carry + n) < GBA_SIG_OVERLAP ? (carry + n) : GBA_SIG_OVERLAP;
            memmove(buf, buf + carry + n - keep, keep);
            carry = keep;
        }
        left -= want;
    }
    fclose(f);
    if (sha && !tail.empty()) sha->update(tail);
    return true;
}

// .code tail after the ROM: [config 0x324][12 pad][2x16 descriptors][.CAA 0x10]
static std::string gbaCodeTail(const std::string& cfgTpl, u32 romSize, u8 saveType) {
    std::string cfg = cfgTpl;                    // 0x324
    wr32(cfg, 0x004, romSize);
    wr32(cfg, 0x008, saveType);
    std::string tail = cfg + std::string(12, '\0');
    char sec[32];
    u32 s0[4] = {0, 0, romSize, 0};              // type 0: ROM
    u32 s1[4] = {1, romSize, 0x324, 0};          // type 1: config
    memcpy(sec, s0, 16); memcpy(sec + 16, s1, 16);
    tail.append(sec, 32);
    char caa[16];
    u32 secOff = romSize + 0x324 + 12;
    memcpy(caa, ".CAA", 4);
    u32 v1 = 1, vN = 2 << 4;
    memcpy(caa + 4, &v1, 4); memcpy(caa + 8, &secOff, 4); memcpy(caa + 12, &vN, 4);
    tail.append(caa, 16);
    return tail;
}

ReturnResult* CtrBuilder::buildGbaCIA(const std::string& romPath, const std::string& title,
                                      u64 tid, const std::string& icon48,
                                      const std::string& bannerTex,
                                      int screenMode,
                                      std::function<bool(u64,u64)> progress) {
    if (!parsed) return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "template not loaded");

    // --- template pieces (cached at init; see loadGbaTemplates)
    loadGbaTemplates();
    std::string ncch = tplNcch, exh = tplExh, icn = tplIcn;
    std::string logo = tplLogo, rfs = tplRfs, cfg = tplCfg, bnrT = tplBnr;
    if (ncch.size() != 0x200 || exh.size() != 0x800 || cfg.size() != 0x324 ||
        icn.size() < 0x36C0 || logo.empty() || rfs.empty() || bnrT.empty())
        return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "gba template missing/bad");

    // --- ROM info
    FILE* rf = fopen(romPath.c_str(), "rb");
    if (!rf) return new ReturnResult(ERROR_PATH, "rom not found");
    fseek(rf, 0, SEEK_END);
    u32 romSize = (u32)ftell(rf);
    char gamecode[5] = {0};
    fseek(rf, 0xAC, SEEK_SET);
    fread(gamecode, 1, 4, rf);
    fclose(rf);
    for (int i = 0; i < 4; i++)
        if (gamecode[i] < 0x20 || gamecode[i] > 0x7E) { strcpy(gamecode, "HMBW"); break; }
    if (romSize < 0xC0 || romSize > 32*1024*1024)
        return new ReturnResult(ERROR_PATH, "bad rom size");
    std::string productCode = std::string("CTR-N-") + gamecode;

    // --- config block tweaks (agb_edit layout: sleepButtons @0x0E,
    // videoLUT @0x24 = 256 RGB triplets)
    {
        // lid-close button combo L+R+SELECT — activates the built-in sleep of
        // games that have one (NSUI sleep-patch default); harmless otherwise
        u16 sleepBtns = (1 << 9) | (1 << 8) | (1 << 2);
        memcpy(&cfg[0x0E], &sleepBtns, 2);
        if (screenMode != GBA_SCREEN_ORIGINAL) {
            // gamma curve y = x^(gIn/gOut), per agb_edit/open_agb_firm; the
            // donor's LUT was a flat 60% darken that crushed white to 153.
            double e = 1.0;                       // RAW: identity
            if (screenMode == GBA_SCREEN_GAMMA || screenMode == GBA_SCREEN_NIGHT)
                e = 2.2 / 1.54;                   // AGS-101 look
            else if (screenMode == GBA_SCREEN_BRIGHT)
                e = 2.2 / 1.7;                    // gentler correction
            // NIGHT adds a 3400K whitepoint (redshift table, via Luma3DS)
            double wp[3] = {1.0, 1.0, 1.0};
            if (screenMode == GBA_SCREEN_NIGHT) { wp[1] = 0.769; wp[2] = 0.524; }
            for (int i = 0; i < 256; i++) {
                double y = pow(i / 255.0, e);
                for (int c = 0; c < 3; c++)
                    cfg[0x24 + i*3 + c] = (u8)(255.0 * wp[c] * y + 0.5);
            }
        }
    }

    // --- pass 1: SHA1 (for the save DB) + signature scan in one read
    ctrLogger.info("gba: scan+sha1 pass, rom " + std::to_string(romSize));
    GbaSaveScan scan;
    StreamSha1 romSha1;
    if (!gbaRomPass(romPath, romSize, &scan, nullptr, "", &romSha1))
        return new ReturnResult(ERROR_PATH, "rom read failed (scan)");
    u8 sha1dig[20];
    romSha1.finish(sha1dig);
    // detection tiers: exact SHA1 → cart serial (catches translations/hacks
    // whose original is in the DB) → SDK version-string LUT → none
    int dbType = gbaDbSaveType(sha1dig);
    const char* srcName = "gba_db(sha1)";
    if (dbType < 0) {
        dbType = gbaDbSaveTypeBySerial(gamecode);
        if (dbType >= 0) srcName = "gba_db(serial)";
    }
    u8 saveType = (dbType >= 0) ? (u8)dbType : scan.result(romSize);
    {
        char sbuf[96];
        snprintf(sbuf, sizeof(sbuf), "gba: save type 0x%02X (%s)", saveType,
                 dbType >= 0 ? srcName : (scan.found >= 0 ? gbaSigLut[scan.found].sig : "none"));
        ctrLogger.info(sbuf);
    }
    std::string tail = gbaCodeTail(cfg, romSize, saveType);
    u32 codeSize = romSize + (u32)tail.size();
    StreamSha codeSha;
    if (!gbaRomPass(romPath, romSize, nullptr, &codeSha, tail))
        return new ReturnResult(ERROR_PATH, "rom read failed (hash)");
    std::string codeHash = codeSha.finish();

    // --- SMDH: titles in all 16 slots + optional icon art
    std::string smdh = icn;
    {
        std::string shortT = title.substr(0, 0x3F);
        for (u32 lang = 0; lang < 16; lang++) {
            u32 o = 0x8 + lang * 0x200;
            memset(&smdh[o], 0, 0x200);
            utf16Write(smdh, o,         shortT, 0x40);
            utf16Write(smdh, o + 0x80,  title.substr(0, 0x7F), 0x80);
            utf16Write(smdh, o + 0x180, "romm3ds", 0x40);
        }
        if (icon48.size() == 48*48*2) {
            static u16 lin24[24*24];
            const u16* l48 = (const u16*)icon48.data();
            for (u32 y = 0; y < 24; y++)
                for (u32 x = 0; x < 24; x++)
                    lin24[y*24+x] = l48[(y*2)*48 + (x*2)];
            tileIcon(lin24, (u16*)&smdh[0x2040], 24);
            tileIcon(l48,   (u16*)&smdh[0x24C0], 48);
        }
        // debug: keep the last baked SMDH on SD so icon issues can be
        // inspected off-device (fetched over ftpd)
        FILE* dbg = fopen("sdmc:/3ds/forwarder/last-gba-smdh.bin", "wb");
        if (dbg) { fwrite(smdh.data(), 1, smdh.size(), dbg); fclose(dbg); }
    }

    // --- banner (reuse the CBMD patcher). Use the bundled GBA startup chime;
    // fall back to the known-good silent CWAV if the asset cannot be converted.
    std::string gbaSound = tplGbaChime.empty() ? silentCwav() : tplGbaChime;
    std::string banner = buildBanner(bnrT, bannerTex, gbaSound);

    // --- ExeFS header: .code, banner, icon, logo (official VC order; logo
    // is required or AGB_FIRM refuses to boot)
    struct { const char* name; u32 size; std::string hash; const std::string* data; } ef[4] = {
        {".code", codeSize,          codeHash,                                nullptr},
        {"banner", (u32)banner.size(), sha256((u8*)banner.data(), banner.size()), &banner},
        {"icon",   (u32)smdh.size(),   sha256((u8*)smdh.data(),   smdh.size()),   &smdh},
        {"logo",   (u32)logo.size(),   sha256((u8*)logo.data(),   logo.size()),   &logo},
    };
    std::string exefsHdr(0x200, '\0');
    u32 off = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(&exefsHdr[i*16], ef[i].name, strlen(ef[i].name));
        wr32(exefsHdr, i*16 + 8, off);
        wr32(exefsHdr, i*16 + 12, ef[i].size);
        memcpy(&exefsHdr[0x200 - (i+1)*0x20], ef[i].hash.data(), 0x20);
        off += align200(ef[i].size);
    }
    u32 exefsDataSize = off;
    u32 exefsTotal = 0x200 + exefsDataSize;

    // --- NCCH header: layout + ids + hashes
    u32 exefsOffB = 0x200 + (u32)exh.size();          // 0xA00
    // 3dstool/official layout aligns RomFS to 0x1000
    u32 romfsOffB = (exefsOffB + exefsTotal + 0xFFF) & ~0xFFFu;
    u32 exefsGap = romfsOffB - (exefsOffB + exefsTotal);
    u32 romfsSizeB = align200((u32)rfs.size());
    u32 contentSize = romfsOffB + romfsSizeB;
    wr32(ncch, 0x104, contentSize / 0x200);
    wr64(ncch, 0x108, tid);                            // partition id
    wr64(ncch, 0x118, tid);                            // program id
    memset(&ncch[0x150], 0, 0x10);
    memcpy(&ncch[0x150], productCode.c_str(), productCode.size() < 0x10 ? productCode.size() : 0x10);
    ncch[0x18F] |= 0x04;                               // NoCrypto
    wr32(ncch, 0x1A0, exefsOffB / 0x200);
    wr32(ncch, 0x1A4, exefsTotal / 0x200);
    wr32(ncch, 0x1A8, 1);                              // exefs hash region: header
    wr32(ncch, 0x1B0, romfsOffB / 0x200);
    wr32(ncch, 0x1B4, romfsSizeB / 0x200);
    u32 rfsHashMu = rd32(ncch, 0x1B8);                 // keep template's region
    if (!rfsHashMu || rfsHashMu * 0x200 > romfsSizeB) { rfsHashMu = 1; wr32(ncch, 0x1B8, rfsHashMu); }

    // exheader: app-title tag (must be non-empty) + title ids. The signed
    // AccessDesc at 0x400 stays untouched (vcoven: CFW tolerates the mismatch).
    memset(&exh[0], 0, 8);
    memcpy(&exh[0], gamecode, 4);
    wr64(exh, 0x1C8, tid);                             // SCI jump id
    wr64(exh, 0x200, tid);                             // ACI program id

    std::string exhHash = sha256((u8*)exh.data(), 0x400);
    memcpy(&ncch[0x160], exhHash.data(), 0x20);
    std::string efsHash = sha256((u8*)exefsHdr.data(), 0x200);
    memcpy(&ncch[0x1C0], efsHash.data(), 0x20);
    std::string rfsPadded = rfs + std::string(romfsSizeB - rfs.size(), '\0');
    std::string rfsHash = sha256((u8*)rfsPadded.data(), rfsHashMu * 0x200);
    memcpy(&ncch[0x1E0], rfsHash.data(), 0x20);

    // --- pass 2: content hash for the TMD (AM verifies it at finish)
    StreamSha conSha;
    conSha.update(ncch);
    conSha.update(exh);
    conSha.update(exefsHdr);
    if (!gbaRomPass(romPath, romSize, nullptr, &conSha, tail))
        return new ReturnResult(ERROR_PATH, "rom read failed (content hash)");
    std::string codePad(align200(codeSize) - codeSize, '\0');
    conSha.update(codePad);
    for (int i = 1; i < 4; i++) {
        conSha.update(*ef[i].data);
        conSha.update(std::string(align200(ef[i].size) - ef[i].size, '\0'));
    }
    std::string gapPad(exefsGap, '\0');
    conSha.update(gapPad);
    conSha.update(rfsPadded);
    std::string conHash = conSha.finish();

    // --- ticket + TMD from the forwarder template shell
    std::string tik = tpl.substr(tikOff, tikSize);
    wr64be(tik, 0x1DC, tid);
    std::string tmd = tpl.substr(tmdOff, tmdSize);
    wr64be(tmd, 0x18C, tid);
    // HOME caches title icons per (tid, version): bump the version past the
    // installed one on rebuilds or a Change-art icon never shows on HOME
    {
        u16 ver = 1;
        AM_TitleEntry te;
        u64 id = tid;
        if (titleInstalledOn(MEDIATYPE_SD, tid) &&
            R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &id, &te)))
            ver = (u16)(te.version + 1);
        tmd[0x1DC] = (char)(ver >> 8);
        tmd[0x1DD] = (char)(ver & 0xFF);
        tik[0x1E6] = (char)(ver >> 8);
        tik[0x1E7] = (char)(ver & 0xFF);
        char vbuf[48];
        snprintf(vbuf, sizeof(vbuf), "gba: title version %u", ver);
        ctrLogger.info(vbuf);
    }
    // AM creates the SD save image from the TMD save size; must match the
    // exheader's demand (192K for GBA VC) or the title dies at launch with
    // an ErrDisp from the HOME menu.
    wr32(tmd, 0x19A, (u32)rd64(exh, 0x1C0));
    wr64be(tmd, 0xB04 + 0x8, (u64)contentSize);
    memcpy(&tmd[0xB04 + 0x10], conHash.data(), 0x20);
    std::string chunkHash = sha256((u8*)tmd.data() + 0xB04, 0x30);
    memcpy(&tmd[0x204 + 0x4], chunkHash.data(), 0x20);
    std::string infoHash = sha256((u8*)tmd.data() + 0x204, 0x900);
    memcpy(&tmd[0x1E4], infoHash.data(), 0x20);

    // --- meta (smdh copy at meta+0x400)
    std::string meta = tpl.substr(metaOff, metaSize);
    if (metaSize >= 0x400 + 0x36C0)
        meta.replace(0x400, 0x36C0, smdh.substr(0, 0x36C0));

    // --- CIA header
    std::string hdr = tpl.substr(0, rd32(tpl, 0x00));
    wr64(hdr, 0x18, (u64)contentSize);
    std::string preamble = aligned(hdr, 0x40) + aligned(tpl.substr(certOff, certSize), 0x40) +
                           aligned(tik, 0x40) + aligned(tmd, 0x40);
    u64 totalBytes = preamble.size() + contentSize + meta.size();

    // --- space check (the installed title is NOT deleted here: the install
    // below overwrites in place, which keeps the save data and avoids AM's
    // "already exists" on back-to-back rebuilds)
    FS_ArchiveResource sd = {};
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD))) {
        u64 freeBytes = (u64)sd.freeClusters * sd.clusterSize;
        if (freeBytes < totalBytes)
            return new ReturnResult(ERROR_NOT_ENOUGH_SPACE, "SD full");
    }

    // --- assemble the CIA into an SD temp file (incremental — ROM streamed,
    // never the whole CIA in RAM), then install via the proven installer.
    char lbuf[192];
    snprintf(lbuf, sizeof(lbuf),
             "gba: assemble tid=%016llX rom=%lu save=0x%02X code=%lu exefs=%lu content=%lu total=%llu pre=%lu meta=%lu",
             tid, romSize, saveType, codeSize, exefsTotal, contentSize, totalBytes,
             (u32)preamble.size(), metaSize);
    ctrLogger.info(lbuf);

    std::string ciaPath = romPath + ".cia";
    std::string tmpPath = ciaPath + ".part";
    FILE* out = fopen(tmpPath.c_str(), "wb");
    if (!out) return new ReturnResult(ERROR_PATH, "cannot create CIA on SD");
    static char iobuf[0x40000];
    setvbuf(out, iobuf, _IOFBF, sizeof(iobuf));

    const char* stage = "preamble";
    bool wok = true;
    auto putN = [&](const void* d, u32 n) -> bool {
        if (fwrite(d, 1, n, out) != n) {
            snprintf(lbuf, sizeof(lbuf), "gba: SD write failed stage=%s n=%lu", stage, n);
            ctrLogger.error(lbuf);
            return false;
        }
        return true;
    };
    auto putS = [&](const std::string& s) { return putN(s.data(), (u32)s.size()); };

    wok = putS(preamble);
    stage = "ncch-hdr";  wok = wok && putS(ncch);
    stage = "exheader";  wok = wok && putS(exh);
    stage = "exefs-hdr"; wok = wok && putS(exefsHdr);
    stage = "rom";
    if (wok) {   // stream the ROM from SD into the CIA
        FILE* f = fopen(romPath.c_str(), "rb");
        if (!f) { ctrLogger.error("gba: rom reopen failed for assemble"); wok = false; }
        else {
            static char buf[GBA_CHUNK];
            u32 left = romSize;
            while (wok && left > 0) {
                u32 want = left < GBA_CHUNK ? left : GBA_CHUNK;
                if (fread(buf, 1, want, f) != want) { ctrLogger.error("gba: rom read failed"); wok = false; break; }
                wok = putN(buf, want);
                left -= want;
            }
            fclose(f);
        }
    }
    stage = "code-tail"; wok = wok && putS(tail) && putS(codePad);
    for (int i = 1; wok && i < 4; i++) {
        stage = ef[i].name;
        wok = putS(*ef[i].data) &&
              putS(std::string(align200(ef[i].size) - ef[i].size, '\0'));
    }
    stage = "romfs"; wok = wok && putS(gapPad) && putS(rfsPadded);
    stage = "meta";  wok = wok && putS(meta);
    if (fclose(out) != 0) wok = false;

    if (!wok) {
        remove(tmpPath.c_str());
        return new ReturnResult(ERROR_PATH, "CIA assemble failed (SD full?)");
    }
    remove(ciaPath.c_str());
    if (rename(tmpPath.c_str(), ciaPath.c_str()) != 0) {
        remove(tmpPath.c_str());
        return new ReturnResult(ERROR_PATH, "CIA rename failed");
    }
    ctrLogger.info("gba: CIA assembled, installing via installCiaFromFile");

    std::string ierr;
    bool installed = installCiaFromFile(ciaPath, ierr, true, [&](u64 d, u64 t) {
        return progress ? progress(d, t) : true;
    });
    if (!installed) {
        // keep the CIA on SD for inspection when the install itself failed
        snprintf(lbuf, sizeof(lbuf), "gba: install failed: %s (kept %s)", ierr.c_str(), ciaPath.c_str());
        ctrLogger.error(lbuf);
        return new ReturnResult(ierr == "cancelled" ? ERROR_INSTALL : -1,
                                ierr == "cancelled" ? "cancelled" : ("install: " + ierr));
    }
    remove(ciaPath.c_str());   // free the CIA copy once installed

    // ownership file keeps the TID stable across reinstalls
    char cfgName[64];
    snprintf(cfgName, sizeof(cfgName), "%s%016llX.txt", CTR_CONFIG_DIR.c_str(), tid);
    std::ofstream o(cfgName);
    o << romPath;
    o.close();
    ctrTidCacheInvalidate();
    saveBannerPreview(tid, bannerTex);
    titleIconInvalidate(tid);         // SMDH carries the new icon now

    ctrLogger.info("gba: installed tid ok");
    return new ReturnResult(ERROR_SUCCESS, "");
}
