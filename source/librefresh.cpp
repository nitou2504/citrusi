#include <3ds.h>
#include <atomic>
#include <map>
#include "librefresh.hpp"
#include "ciainstall.hpp"
#include "logger.hpp"

static Logger lrlog("libref");

static Thread gThread = nullptr;
static std::atomic<bool> gBusy(false);   // worker running
static std::atomic<bool> gDone(false);   // result waiting for take
static bool gOk = false;                 // worker-only until gDone
static std::string gSlug;                // set before spawn, read-only during run
static std::vector<RommRom> gResult;     // worker-only until gDone
static RommClient gClient;               // worker-only copy
static std::map<int, u64> gKnownTids;

static void worker(void*) {
    u64 t0 = osGetTime();
    std::vector<RommRom> fresh;
    bool ok = false;
    int pid = gClient.findPlatform(gSlug);
    if (pid >= 0 && gClient.listRoms(pid, fresh, gSlug)) {
        ok = true;
        if (gSlug == ROMM_SLUG_3DS) {
            for (auto& r : fresh) {
                auto it = gKnownTids.find(r.id);
                if (it != gKnownTids.end()) { r.titleId = it->second; continue; }
                if (!r.installable) continue;
                std::string hdr;
                if (gClient.fetchCiaHeader(r, hdr)) r.titleId = ciaBufferTitleId(hdr);
            }
        }
        lrlog.info("refresh " + gSlug + ": " + std::to_string(fresh.size()) + " roms, " +
                   std::to_string((unsigned long long)(osGetTime() - t0)) + "ms");
    } else {
        lrlog.error("refresh " + gSlug + " failed: " + gClient.lastError);
    }
    gResult = std::move(fresh);
    gOk = ok;
    gDone = true;    // gBusy stays true until the result is taken
}

void libRefreshStart(const RommClient& client, const std::string& slug,
                     const std::vector<RommRom>& current) {
    if (gBusy || gDone) return;
    if (gThread) { threadJoin(gThread, U64_MAX); threadFree(gThread); gThread = nullptr; }
    gClient.host = client.host;
    gClient.user = client.user;
    gClient.pass = client.pass;
    gClient.buildAuth();
    gSlug = slug;
    gKnownTids.clear();
    for (auto& r : current) if (r.titleId) gKnownTids[r.id] = r.titleId;
    gOk = false;
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

bool libRefreshTake(std::string& slug, std::vector<RommRom>& roms, bool& ok) {
    if (!gDone) return false;
    slug = gSlug;
    roms = std::move(gResult);
    ok = gOk;
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
