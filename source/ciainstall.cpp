#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include "ciainstall.hpp"
#include "logger.hpp"

static Logger cilog("ciainst");

// Parse the title id out of a CIA's TMD (big-endian u64 at TMD header +0x4C).
static u64 readCiaTitleId(FILE* f) {
    unsigned char hdr[0x20];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 0x20, f) != 0x20) return 0;
    auto rd32 = [&](int o) -> u32 {
        return (u32)hdr[o] | ((u32)hdr[o+1] << 8) | ((u32)hdr[o+2] << 16) | ((u32)hdr[o+3] << 24);
    };
    u32 headerSize = rd32(0x00), certSize = rd32(0x08), tikSize = rd32(0x0C), tmdSize = rd32(0x10);
    (void)tmdSize;
    auto al = [](u64 v) -> u64 { return (v + 63) & ~((u64)63); };
    u64 certOff = al(headerSize);
    u64 tikOff  = al(certOff + certSize);
    u64 tmdOff  = al(tikOff + tikSize);

    unsigned char st[4];
    if (fseek(f, (long)tmdOff, SEEK_SET) != 0 || fread(st, 1, 4, f) != 4) return 0;
    u32 sigType = ((u32)st[0] << 24) | ((u32)st[1] << 16) | ((u32)st[2] << 8) | st[3];
    u32 sigSize;
    switch (sigType) {
        case 0x00010000: case 0x00010003: sigSize = 0x200 + 0x3C; break; // RSA-4096 / ECDSA? -> 0x200
        case 0x00010001: case 0x00010004: sigSize = 0x100 + 0x3C; break; // RSA-2048
        default:                          sigSize = 0x3C  + 0x40; break; // ECDSA
    }
    u64 tmdHdr = tmdOff + 4 + sigSize;
    unsigned char tid[8];
    if (fseek(f, (long)(tmdHdr + 0x4C), SEEK_SET) != 0 || fread(tid, 1, 8, f) != 8) return 0;
    u64 t = 0;
    for (int i = 0; i < 8; i++) t = (t << 8) | tid[i];
    return t;
}

unsigned long long ciaFileTitleId(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    u64 tid = readCiaTitleId(f);
    fclose(f);
    return tid;
}

unsigned long long ciaBufferTitleId(const std::string& b) {
    if (b.size() < 0x20) return 0;
    auto rd32 = [&](size_t o) -> u32 {
        return (u32)(u8)b[o] | ((u32)(u8)b[o+1] << 8) | ((u32)(u8)b[o+2] << 16) | ((u32)(u8)b[o+3] << 24);
    };
    u32 headerSize = rd32(0x00), certSize = rd32(0x08), tikSize = rd32(0x0C);
    auto al = [](u64 v) -> u64 { return (v + 63) & ~((u64)63); };
    u64 certOff = al(headerSize);
    u64 tikOff  = al(certOff + certSize);
    u64 tmdOff  = al(tikOff + tikSize);
    if (tmdOff + 4 > b.size()) return 0;
    u32 sigType = ((u32)(u8)b[tmdOff] << 24) | ((u32)(u8)b[tmdOff+1] << 16) |
                  ((u32)(u8)b[tmdOff+2] << 8) | (u8)b[tmdOff+3];
    u32 sigSize;
    switch (sigType) {
        case 0x00010000: case 0x00010003: sigSize = 0x200 + 0x3C; break;
        case 0x00010001: case 0x00010004: sigSize = 0x100 + 0x3C; break;
        default:                          sigSize = 0x3C  + 0x40; break;
    }
    u64 h = tmdOff + 4 + sigSize;
    if (h + 0x4C + 8 > b.size()) return 0;
    u64 tid = 0;
    for (int i = 0; i < 8; i++) tid = (tid << 8) | (u8)b[h + 0x4C + i];
    return tid;
}

// AM "already exists": the title db still holds a title/ticket/pending import
// for this tid. Seen on rebuild-over-a-just-installed inject (GBA Change art
// twice in a row) — the fix is to clear every trace and stream again.
#define AM_ERR_ALREADY_EXISTS 0xC8E083FCu

static bool amTitleExists(FS_MediaType media, u64 tid) {
    AM_TitleEntry te;
    u64 id = tid;
    return tid && R_SUCCEEDED(AM_GetTitleInfo(media, 1, &id, &te));
}

// one full streaming pass; 0 = installed, 1 = fatal, 2 = "already exists"
// (worth a cleanup + one retry). overwrite = reinstall in place, keeps the
// SD save data — required when the tid is still installed or AM rejects the
// ticket/TMD import with AM_ERR_ALREADY_EXISTS.
static int installAttempt(FILE* f, long long sz, FS_MediaType media, bool overwrite,
                          std::string& err,
                          std::function<bool(unsigned long long, unsigned long long)>& progress) {
    fseek(f, 0, SEEK_SET);
    Handle h = 0;
    Result r = overwrite ? AM_StartCiaInstallOverwrite(&h, media)
                         : AM_StartCiaInstall(media, &h);
    if (R_FAILED(r)) {
        char b[64]; snprintf(b, sizeof(b), "StartCiaInstall%s 0x%08lX",
                             overwrite ? "Overwrite" : "", (unsigned long)r);
        err = b;
        return ((u32)r == AM_ERR_ALREADY_EXISTS) ? 2 : 1;
    }

    std::vector<unsigned char> buf(256 * 1024);
    u64 off = 0;
    int rc = 0;
    while (off < (u64)sz) {
        size_t rd = fread(buf.data(), 1, buf.size(), f);
        if (rd == 0) break;
        u32 written = 0;
        r = FSFILE_Write(h, &written, off, buf.data(), (u32)rd, FS_WRITE_FLUSH);
        if (R_FAILED(r)) {
            char b[96]; snprintf(b, sizeof(b), "write 0x%08lX", (unsigned long)r);
            err = b;
            char lb[128]; snprintf(lb, sizeof(lb), "write failed 0x%08lX at offset %llu/%lld ovw=%d",
                                   (unsigned long)r, (unsigned long long)off, sz, overwrite ? 1 : 0);
            cilog.error(lb);
            rc = ((u32)r == AM_ERR_ALREADY_EXISTS) ? 2 : 1;
            break;
        }
        off += rd;
        if (progress && !progress(off, (u64)sz)) { err = "cancelled"; rc = 1; break; }
    }

    if (rc != 0) { AM_CancelCIAInstall(h); return rc; }

    r = AM_FinishCiaInstall(h);
    if (R_FAILED(r)) {
        char b[64]; snprintf(b, sizeof(b), "FinishCiaInstall 0x%08lX", (unsigned long)r);
        err = b;
        return ((u32)r == AM_ERR_ALREADY_EXISTS) ? 2 : 1;
    }
    return 0;
}

bool installCiaFromFile(const std::string& path, std::string& err, bool force,
                        std::function<bool(unsigned long long, unsigned long long)> progress) {
    struct stat stt;
    if (stat(path.c_str(), &stt) != 0 || stt.st_size <= 0) { err = "file not found"; return false; }
    long long sz = (long long)stt.st_size;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "open failed"; return false; }

    u64 tid = readCiaTitleId(f);
    u32 high = (u32)(tid >> 32);
    // game/update/dlc -> SD; everything else (system, TWL) -> NAND
    FS_MediaType media = (high == 0x00040000 || high == 0x0004000E || high == 0x0004008C)
                         ? MEDIATYPE_SD : MEDIATYPE_NAND;
    (void)force;   // an installed tid always takes the overwrite path (keeps the save)

    bool overwrite = amTitleExists(media, tid);
    int rc = installAttempt(f, sz, media, overwrite, err, progress);
    if (rc == 2 && tid) {
        // stale title/ticket/pending import in the AM db: clear all three and
        // stream once more (fresh install — the save data is already gone or
        // was never there if we land here on a non-overwrite path)
        Result dt = AM_DeleteTitle(media, tid);
        Result dk = AM_DeleteTicket(tid);
        Result dp = AM_DeletePendingTitle(media, tid);
        char lb[128];
        snprintf(lb, sizeof(lb), "already-exists cleanup tid=%016llX delTitle=%08lX delTicket=%08lX delPending=%08lX",
                 (unsigned long long)tid, (unsigned long)dt, (unsigned long)dk, (unsigned long)dp);
        cilog.warn(lb);
        rc = installAttempt(f, sz, media, false, err, progress);
        if (rc == 0) cilog.info("retry after cleanup: installed ok");
    }
    fclose(f);
    return rc == 0;
}
