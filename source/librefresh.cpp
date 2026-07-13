#include <3ds.h>
#include <atomic>
#include <map>
#include "librefresh.hpp"
#include "ciainstall.hpp"
#include "covercache.hpp"
#include "logger.hpp"

static Logger lrlog("libref");

static Thread gThread = nullptr;
static std::atomic<bool> gBusy(false);   // worker running
static std::atomic<bool> gDone(false);   // result waiting for take
static bool gOk = false;                 // worker-only until gDone
static bool gChanged = false;            // worker-only until gDone
static std::string gSlug;                // set before spawn, read-only during run
static std::vector<RommRom> gOld;        // list the UI currently shows
static std::vector<RommRom> gResult;     // worker-only until gDone
static RommClient gClient;               // worker-only copy
static LibSaveFn gSave = nullptr;

// did the refresh actually change the list? (order-sensitive on purpose)
static bool differs(const std::vector<RommRom>& a, const std::vector<RommRom>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].id != b[i].id || a[i].fsName != b[i].fsName ||
            a[i].fileId != b[i].fileId || a[i].name != b[i].name ||
            a[i].sizeBytes != b[i].sizeBytes || a[i].titleId != b[i].titleId ||
            a[i].coverPath != b[i].coverPath)
            return true;
    }
    return false;
}

static void worker(void*) {
    u64 t0 = osGetTime();
    std::vector<RommRom> fresh;
    bool ok = false, changed = false;
    int pid = gClient.findPlatform(gSlug);
    if (pid >= 0 && gClient.listRoms(pid, fresh, gSlug)) {
        ok = true;
        if (gSlug == ROMM_SLUG_3DS) {
            std::map<int, const RommRom*> old;
            for (auto& r : gOld) old[r.id] = &r;
            for (auto& r : fresh) {
                auto it = old.find(r.id);
                // fileId==-1: RomM >= 4.9.2 list response has no file lists.
                // Reuse the pick the UI already resolved, else fetch it here.
                if (r.fileId == -1 && it != old.end() && it->second->fileId != -1) {
                    r.fsName      = it->second->fsName;
                    r.fileId      = it->second->fileId;
                    r.sizeBytes   = it->second->sizeBytes;
                    r.installable = it->second->installable;
                }
                if (r.fileId == -1 && !gClient.resolveRomFile(r)) continue;
                if (it != old.end() && it->second->titleId) { r.titleId = it->second->titleId; continue; }
                if (!r.installable) continue;
                std::string hdr;
                if (gClient.fetchCiaHeader(r, hdr)) r.titleId = ciaBufferTitleId(hdr);
            }
        }
        changed = differs(gOld, fresh);
        // heavy SD work stays on the worker: json save + cover-miss cleanup
        // would otherwise stall the UI thread when the result lands
        if (changed) {
            if (gSave) gSave(gSlug, fresh);
            coverCacheClearMisses();
        }
        lrlog.info("refresh " + gSlug + ": " + std::to_string(fresh.size()) + " roms, " +
                   std::string(changed ? "changed" : "unchanged") + ", " +
                   std::to_string((unsigned long long)(osGetTime() - t0)) + "ms");
    } else {
        lrlog.error("refresh " + gSlug + " failed: " + gClient.lastError);
    }
    gResult = std::move(fresh);
    gOld.clear();
    gOk = ok;
    gChanged = changed;
    gDone = true;    // gBusy stays true until the result is taken
}

void libRefreshStart(const RommClient& client, const std::string& slug,
                     const std::vector<RommRom>& current, LibSaveFn save) {
    if (gBusy || gDone) return;
    if (gThread) { threadJoin(gThread, U64_MAX); threadFree(gThread); gThread = nullptr; }
    gClient.host = client.host;
    gClient.user = client.user;
    gClient.pass = client.pass;
    gClient.buildAuth();
    gSlug = slug;
    gOld = current;
    gSave = save;
    gOk = false;
    gChanged = false;
    gBusy = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    gThread = threadCreate(worker, nullptr, 128 * 1024, prio + 4, -1, false);
    if (!gThread) { lrlog.error("threadCreate failed"); gBusy = false; }
}

bool libRefreshRunning(const std::string& slug) {
    if (!gBusy && !gDone) return false;
    return slug.empty() || slug == gSlug;
}

bool libRefreshTake(std::string& slug, std::vector<RommRom>& roms, bool& ok, bool& changed) {
    if (!gDone) return false;
    slug = gSlug;
    roms = std::move(gResult);
    ok = gOk;
    changed = gChanged;
    gResult.clear();
    gDone = false;
    gBusy = false;
    return true;
}

void libRefreshStop() {
    if (gThread) { threadJoin(gThread, U64_MAX); threadFree(gThread); gThread = nullptr; }
    gBusy = false;
    gDone = false;
}
