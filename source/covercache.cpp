#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <atomic>
#include "covercache.hpp"
#include "teximg.hpp"
#include "helpers.hpp"
#include "settings.hpp"
#include "logger.hpp"

static Logger cclog("cover");

#define RAW_CACHE_DIR (FORWARDER_DIR + std::string("/cache/"))

struct CoverJob { int id; std::string url; };

// One worker: libctru httpc's shared session isn't safe for many concurrent
// contexts (3 fetchers crashed the app). Parallelism must serialize the fetch;
// keep this at 1 until that's in place.
#define COVER_WORKERS 1

static Thread gWorkers[COVER_WORKERS] = {nullptr};
static std::atomic<bool> gRun(false);
static std::atomic<int> gWantId(-1);
static std::atomic<int> gPauseCount(0);
static std::atomic<bool> gBusy(false);
static LightLock gJobsLock;
static std::vector<CoverJob> gJobs;         // guarded by gJobsLock
static std::vector<int> gClaimed;           // ids currently being fetched, guarded by gJobsLock
static RommClient gWorkerClient;            // worker-only after start
static std::atomic<bool> gStarted(false);

static bool isClaimed(int id) {
    for (int c : gClaimed) if (c == id) return true;
    return false;
}

static std::string rawPath(int id) {
    char p[96];
    snprintf(p, sizeof(p), "%s%d.rc", RAW_CACHE_DIR.c_str(), id);
    return std::string(p);
}
static std::string missPath(int id) {
    char p[96];
    snprintf(p, sizeof(p), "%s%d.none", RAW_CACHE_DIR.c_str(), id);
    return std::string(p);
}

// converts fetched image bytes into the raw cache entry (atomic rename)
static bool storeRaw(int id, const std::string& bytes) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba = decodeImageRGBA(bytes, COVER_W, COVER_H, &w, &h);
    if (rgba.empty()) return false;
    std::string tmp = rawPath(id) + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    u16 dims[2] = {(u16)w, (u16)h};
    fwrite(dims, 2, 2, f);
    fwrite(rgba.data(), 1, rgba.size(), f);
    fclose(f);
    remove(rawPath(id).c_str());
    return rename(tmp.c_str(), rawPath(id).c_str()) == 0;
}

static void processJob(const CoverJob& job) {
    if (fileExists(rawPath(job.id)) || fileExists(missPath(job.id))) return;
    // migrate: an old png/jpg cache file may already exist
    std::string legacy = RAW_CACHE_DIR + std::to_string(job.id) + "_l.img";
    std::string bytes;
    if (fileExists(legacy)) {
        bytes = readEntireFile(legacy);
    } else if (job.url.empty() || !gWorkerClient.fetchUrl(job.url, bytes) || bytes.empty()) {
        // remember the miss so we don't hammer the server
        std::ofstream o(missPath(job.id));
        o << "x";
        return;
    }
    if (!storeRaw(job.id, bytes)) {
        std::ofstream o(missPath(job.id));
        o << "x";
    }
}

static void workerMain(void*) {
    while (gRun) {
        if (gPauseCount > 0) {                       // foreground owns the network
            svcSleepThread(50 * 1000 * 1000LL);
            continue;
        }
        CoverJob job = {-1, ""};
        int want = gWantId.exchange(-1);
        LightLock_Lock(&gJobsLock);
        // priority request first (if not already done/claimed)
        if (want >= 0 && !isClaimed(want) &&
            !fileExists(rawPath(want)) && !fileExists(missPath(want))) {
            for (auto& j : gJobs)
                if (j.id == want) { job = j; break; }
        }
        // otherwise the next unclaimed, undone job
        if (job.id < 0) {
            for (auto& j : gJobs) {
                if (!isClaimed(j.id) && !fileExists(rawPath(j.id)) && !fileExists(missPath(j.id))) {
                    job = j;
                    break;
                }
            }
        }
        if (job.id >= 0) gClaimed.push_back(job.id);   // claim so peers skip it
        LightLock_Unlock(&gJobsLock);
        if (job.id < 0) {
            svcSleepThread(150 * 1000 * 1000LL); // idle: 150ms
            continue;
        }
        gBusy = true;
        processJob(job);
        gBusy = false;
        LightLock_Lock(&gJobsLock);
        for (auto it = gClaimed.begin(); it != gClaimed.end(); ++it)
            if (*it == job.id) { gClaimed.erase(it); break; }
        LightLock_Unlock(&gJobsLock);
    }
}

void coverCacheStart(const RommClient& client, const std::vector<RommRom>& roms) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(RAW_CACHE_DIR), ec);
    if (!gStarted) {
        LightLock_Init(&gJobsLock);
        gStarted = true;
    }
    LightLock_Lock(&gJobsLock);
    gWorkerClient.host = client.host;
    gWorkerClient.user = client.user;
    gWorkerClient.pass = client.pass;
    gWorkerClient.buildAuth();
    gJobs.clear();
    gClaimed.clear();
    for (auto& r : roms) {
        CoverJob j;
        j.id = r.id;
        j.url = !r.coverPath.empty() ? r.coverPath : r.coverSmallPath;
        gJobs.push_back(j);
    }
    LightLock_Unlock(&gJobsLock);
    cclog.info("start: " + std::to_string(gJobs.size()) + " jobs, workers=" + std::to_string(COVER_WORKERS));
    if (!gWorkers[0]) {
        gRun = true;
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        for (int i = 0; i < COVER_WORKERS; i++) {
            gWorkers[i] = threadCreate(workerMain, nullptr, 128 * 1024, prio + 4, -1, false);
            if (!gWorkers[i]) cclog.error("threadCreate failed for worker " + std::to_string(i));
        }
    }
}

void coverCacheWant(int rommId) {
    gWantId = rommId;
}

bool coverCacheLoad(int rommId, C2D_Image* out) {
    std::string p = rawPath(rommId);
    if (!fileExists(p)) return false;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    u16 dims[2] = {0, 0};
    if (fread(dims, 2, 2, f) != 2 || dims[0] == 0 || dims[0] > 512 || dims[1] == 0 || dims[1] > 512) {
        fclose(f);
        remove(p.c_str());
        return false;
    }
    size_t n = (size_t)dims[0] * dims[1] * 4;
    std::vector<unsigned char> rgba(n);
    size_t got = fread(rgba.data(), 1, n, f);
    fclose(f);
    if (got != n) {
        remove(p.c_str());
        return false;
    }
    return texFromRGBA(rgba.data(), dims[0], dims[1], out);
}

bool coverCacheUnavailable(int rommId) {
    return fileExists(missPath(rommId));
}

void coverCacheClearMisses() {
    std::error_code ec;
    int n = 0;
    for (auto& e : std::filesystem::directory_iterator(RAW_CACHE_DIR, ec)) {
        if (e.path().extension() == ".none") { remove(e.path().c_str()); n++; }
    }
    cclog.info("cleared " + std::to_string(n) + " cover misses");
}

void coverCachePause() {
    gPauseCount++;
    while (gBusy) svcSleepThread(10 * 1000 * 1000LL);   // drain the in-flight job
}

void coverCacheResume() {
    if (gPauseCount > 0) gPauseCount--;
}

void coverCacheStop() {
    if (gWorkers[0]) {
        gRun = false;
        for (int i = 0; i < COVER_WORKERS; i++) {
            if (!gWorkers[i]) continue;
            threadJoin(gWorkers[i], U64_MAX);
            threadFree(gWorkers[i]);
            gWorkers[i] = nullptr;
        }
    }
}
