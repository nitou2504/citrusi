#include <3ds.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include "ctrbuilder.hpp"
#include "ciainstall.hpp"
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
    // nds-bootstrap needs the "sd:" device prefix (verified against a working
    // YANBF launch: NDS_PATH = sd:/roms/nds/...)
    std::string launchPath = romPath;
    if (launchPath.rfind("sdmc:", 0) == 0) launchPath = "sd:" + launchPath.substr(5);
    std::ofstream o(cfg);
    o << launchPath;
    o.close();

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

// binary search gba_db.bin (sorted by first u64 of the SHA1); -1 = not found
static int gbaDbSaveType(const u8 sha1[20]) {
    std::string db = readEntireFile(GBA_TPL_DIR + "gba_db.bin");
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
                                      std::function<bool(u64,u64)> progress) {
    if (!parsed) return new ReturnResult(ERROR_TEMPLATE|ERROR_TEMPLATE_PARSE, "template not loaded");

    // --- template pieces (small, from romfs)
    std::string ncch  = readEntireFile(GBA_TPL_DIR + "ncchheader.bin");
    std::string exh   = readEntireFile(GBA_TPL_DIR + "exheader.bin");
    std::string icn   = readEntireFile(GBA_TPL_DIR + "icon.icn");
    std::string logo  = readEntireFile(GBA_TPL_DIR + "logo.darc.lz");
    std::string rfs   = readEntireFile(GBA_TPL_DIR + "romfs.bin");
    std::string cfg   = readEntireFile(GBA_TPL_DIR + "config_block.bin");
    std::string bnrT  = readEntireFile(GBA_TPL_DIR + "template.bnr");
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

    // --- pass 1: SHA1 (for the save DB) + signature scan in one read
    ctrLogger.info("gba: scan+sha1 pass, rom " + std::to_string(romSize));
    GbaSaveScan scan;
    StreamSha1 romSha1;
    if (!gbaRomPass(romPath, romSize, &scan, nullptr, "", &romSha1))
        return new ReturnResult(ERROR_PATH, "rom read failed (scan)");
    u8 sha1dig[20];
    romSha1.finish(sha1dig);
    int dbType = gbaDbSaveType(sha1dig);
    u8 saveType = (dbType >= 0) ? (u8)dbType : scan.result(romSize);
    {
        char sbuf[96];
        snprintf(sbuf, sizeof(sbuf), "gba: save type 0x%02X (%s)", saveType,
                 dbType >= 0 ? "gba_db" : (scan.found >= 0 ? gbaSigLut[scan.found].sig : "none"));
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
    }

    // --- banner (reuse the CBMD patcher; template sound is silent)
    std::string banner = buildBanner(bnrT, bannerTex, "");

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

    // --- space check + stale title cleanup
    FS_ArchiveResource sd = {};
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&sd, SYSTEM_MEDIATYPE_SD))) {
        u64 freeBytes = (u64)sd.freeClusters * sd.clusterSize;
        if (freeBytes < totalBytes)
            return new ReturnResult(ERROR_NOT_ENOUGH_SPACE, "SD full");
    }
    if (titleInstalledOn(MEDIATYPE_SD, tid)) {
        AM_DeleteTitle(MEDIATYPE_SD, tid);
        AM_DeleteTicket(tid);
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

    if (titleInstalledOn(MEDIATYPE_SD, tid)) {
        AM_DeleteTitle(MEDIATYPE_SD, tid);
        AM_DeleteTicket(tid);
    }
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

    ctrLogger.info("gba: installed tid ok");
    return new ReturnResult(ERROR_SUCCESS, "");
}
