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

#define RAW_CACHE_DIR (FORWARDER_DIR + std::string("/cache/"))

struct CoverJob { int id; std::string url; };

static Thread gWorker = nullptr;
static std::atomic<bool> gRun(false);
static std::atomic<int> gWantId(-1);
static LightLock gJobsLock;
static std::vector<CoverJob> gJobs;         // guarded by gJobsLock
static RommClient gWorkerClient;            // worker-only after start
static std::atomic<bool> gStarted(false);

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
        CoverJob job = {-1, ""};
        int want = gWantId.exchange(-1);
        LightLock_Lock(&gJobsLock);
        if (want >= 0) {
            for (auto& j : gJobs)
                if (j.id == want) { job = j; break; }
        }
        if (job.id < 0) {
            for (auto it = gJobs.begin(); it != gJobs.end(); ++it) {
                if (!fileExists(rawPath(it->id)) && !fileExists(missPath(it->id))) {
                    job = *it;
                    break;
                }
            }
        }
        LightLock_Unlock(&gJobsLock);
        if (job.id < 0) {
            svcSleepThread(200 * 1000 * 1000LL); // idle: 200ms
            continue;
        }
        processJob(job);
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
    for (auto& r : roms) {
        CoverJob j;
        j.id = r.id;
        j.url = !r.coverPath.empty() ? r.coverPath : r.coverSmallPath;
        gJobs.push_back(j);
    }
    LightLock_Unlock(&gJobsLock);
    if (!gWorker) {
        gRun = true;
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        gWorker = threadCreate(workerMain, nullptr, 128 * 1024, prio + 4, -1, false);
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

void coverCacheStop() {
    if (gWorker) {
        gRun = false;
        threadJoin(gWorker, U64_MAX);
        threadFree(gWorker);
        gWorker = nullptr;
    }
}
