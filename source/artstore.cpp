#include <3ds.h>
#include <cstdio>
#include <map>
#include <filesystem>
#include "artstore.hpp"
#include "settings.hpp"
#include "helpers.hpp"
#include "json.hpp"
#include "logger.hpp"

static Logger artLogger("artstore");

static std::map<std::string, ArtEntry> gEntries;
static bool gLoaded = false;

static void loadStore() {
    if (gLoaded) return;
    gLoaded = true;
    if (!fileExists(ART_JSON_PATH)) return;
    try {
        nlohmann::json j = nlohmann::json::parse(readEntireFile(ART_JSON_PATH));
        for (auto& [k, v] : j.items()) {
            ArtEntry e;
            e.valid = true;
            e.query = v.value("query", "");
            e.sgdbGameId = v.value("sgdbGameId", 0);
            e.weak = v.value("weak", false);
            if (v.contains("icon")) {
                e.iconSource = v["icon"].value("source", "");
                e.iconId = v["icon"].value("id", 0);
            }
            if (v.contains("banner")) {
                e.bannerSource = v["banner"].value("source", "");
                e.bannerId = v["banner"].value("id", 0);
                e.bannerName = v["banner"].value("name", "");
            }
            gEntries[k] = e;
        }
    } catch (...) {
        artLogger.error("bad art.json, starting fresh");
        gEntries.clear();
    }
}

static void saveStore() {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [k, e] : gEntries) {
        nlohmann::json v;
        if (!e.query.empty()) v["query"] = e.query;
        if (e.sgdbGameId) v["sgdbGameId"] = e.sgdbGameId;
        v["weak"] = e.weak;
        if (!e.iconSource.empty()) {
            v["icon"]["source"] = e.iconSource;
            if (e.iconId) v["icon"]["id"] = e.iconId;
        }
        if (!e.bannerSource.empty()) {
            v["banner"]["source"] = e.bannerSource;
            if (e.bannerId) v["banner"]["id"] = e.bannerId;
            if (!e.bannerName.empty()) v["banner"]["name"] = e.bannerName;
        }
        j[k] = v;
    }
    std::string tmp = ART_JSON_PATH + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { artLogger.error("art.json write failed"); return; }
    std::string body = j.dump(1);
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    remove(ART_JSON_PATH.c_str());
    rename(tmp.c_str(), ART_JSON_PATH.c_str());
}

ArtEntry artStoreGet(const std::string& fsName) {
    loadStore();
    auto it = gEntries.find(fsName);
    return (it == gEntries.end()) ? ArtEntry() : it->second;
}

void artStorePut(const std::string& fsName, const ArtEntry& entry) {
    loadStore();
    ArtEntry e = entry;
    e.valid = true;
    gEntries[fsName] = e;
    saveStore();
}

void artStoreRemove(const std::string& fsName) {
    loadStore();
    if (gEntries.erase(fsName)) saveStore();
}

// FNV-1a 64 over the key -> stable short file name
std::string artCachePath(const std::string& key) {
    u64 h = 0xcbf29ce484222325ull;
    for (unsigned char c : key) { h ^= c; h *= 0x100000001b3ull; }
    char name[32];
    snprintf(name, sizeof(name), "%016llx.img", (unsigned long long)h);
    return ART_CACHE_DIR + name;
}

std::string artCacheRead(const std::string& key) {
    std::string p = artCachePath(key);
    if (!fileExists(p)) return "";
    return readEntireFile(p);
}

void artCacheWrite(const std::string& key, const std::string& bytes) {
    if (bytes.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(ART_CACHE_DIR), ec);
    std::string p = artCachePath(key);
    std::string tmp = p + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
}
