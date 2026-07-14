#pragma once
#include <string>

// Persisted art choices (docs/ART-UX-SPEC.md §6): art.json next to the TID
// ownership files, keyed by the RomM fs_name. Reinstalls reuse the entry
// silently; weak=true drives the Manage ⚠ marker until the user picks art.
#define ART_JSON_PATH  (FORWARDER_DIR + std::string("/art.json"))
#define ART_CACHE_DIR  std::string("sdmc:/3ds/romm3ds/cache/art/")

struct ArtEntry {
    bool valid = false;       // entry exists in art.json
    std::string query;        // sanitized SGDB query that was used
    int sgdbGameId = 0;       // 0 = no SGDB match
    // icon piece: "sgdb" | "romm-cover" | "" (template/DS icon)
    std::string iconSource;
    int iconId = 0;           // sgdb asset id
    // banner piece: "libretro" | "sgdb" | "romm-cover" | "" (template/stamp chain)
    std::string bannerSource;
    int bannerId = 0;         // sgdb asset id
    std::string bannerName;   // libretro No-Intro name that hit
    bool weak = false;        // fallback art in use -> ⚠ in Manage
    int screen = -1;          // GBA_SCREEN_* baked into the installed inject (-1 = unknown)
};

// cached-on-first-use art.json accessors
ArtEntry artStoreGet(const std::string& fsName);
void artStorePut(const std::string& fsName, const ArtEntry& entry);
void artStoreRemove(const std::string& fsName);

// stable cache file path for a raw downloaded image (key: caller-chosen id)
std::string artCachePath(const std::string& key);
// read/write helpers for the raw image cache ("" = miss)
std::string artCacheRead(const std::string& key);
void artCacheWrite(const std::string& key, const std::string& bytes);
