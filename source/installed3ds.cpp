#include <3ds.h>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include "installed3ds.hpp"
#include "settings.hpp"   // FORWARDER_DIR
#include "json.hpp"

#define TIDMAP_FILE (FORWARDER_DIR + std::string("/installed3ds.json"))

static std::map<int, u64> gTidMap;   // rommId -> title id (persisted)
static std::set<u64> gAmSet;         // title ids currently installed on the console
static bool gLoaded = false;

static void loadMap() {
    if (gLoaded) return;
    gLoaded = true;
    std::ifstream in(TIDMAP_FILE);
    if (!in.good()) return;
    try {
        nlohmann::json j; in >> j;
        for (auto it = j.begin(); it != j.end(); ++it)
            gTidMap[std::stoi(it.key())] = std::stoull(it.value().get<std::string>(), nullptr, 16);
    } catch (...) {}
}

static void saveMap() {
    nlohmann::json j;
    char buf[24];
    for (auto& kv : gTidMap) {
        snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)kv.second);
        j[std::to_string(kv.first)] = std::string(buf);
    }
    std::ofstream o(TIDMAP_FILE);
    o << j.dump();
}

void installed3dsRecord(int rommId, unsigned long long titleId) {
    loadMap();
    gTidMap[rommId] = titleId;
    saveMap();
    gAmSet.insert(titleId);   // so it shows installed immediately
}

void installed3dsRefresh() {
    loadMap();
    gAmSet.clear();
    FS_MediaType medias[2] = {MEDIATYPE_SD, MEDIATYPE_NAND};
    for (int m = 0; m < 2; m++) {
        u32 count = 0;
        if (R_FAILED(AM_GetTitleCount(medias[m], &count)) || count == 0) continue;
        std::vector<u64> tids(count);
        u32 read = 0;
        if (R_FAILED(AM_GetTitleList(&read, medias[m], count, tids.data()))) continue;
        for (u32 i = 0; i < read; i++) gAmSet.insert(tids[i]);
    }
}

bool installed3dsIs(int rommId) {
    auto it = gTidMap.find(rommId);
    if (it == gTidMap.end()) return false;
    return gAmSet.count(it->second) > 0;
}
