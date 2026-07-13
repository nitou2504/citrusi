#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <map>

struct ManagedRom {
    std::string path;     // sdmc:/roms/nds/xxx.nds
    std::string display;  // file name
    u64 tid;              // expected TWL forwarder title id (0 if unknown)
    bool installed;       // TWL forwarder present on NAND
    u64 yanbfTid;         // YANBF CTR forwarder title id on SD (0 if none)
    u64 rommTid;          // romm3ds CTR forwarder title id on SD (0 if none)
    std::string orphanCia; // forwarder .cia for this rom found on SD but not installed
    u64 sizeBytes;
};

// expected forwarder TID for a rom, replicating the builder's derivation
// (default template, non-random TID). 0 on failure.
u64 computeForwarderTID(const std::string& ndsPath);

std::vector<u64> getInstalledTwlTitles();
bool twlTitleInstalled(const std::vector<u64>& list, u64 tid);

// YANBF CTR forwarders on SD (TID 000400000FF40000-000400000FF7FFFF):
// maps lowercase rom filename (from the title's romfs path.txt) -> tid
std::map<std::string, u64> getYanbfForwarders();
void invalidateYanbfCache();

// forwarder .cia files found on SD whose title is NOT installed:
// maps lowercase rom filename -> cia path. filled by getYanbfForwarders().
std::map<std::string, std::string> getOrphanForwarderCias();

// session-cached; every install/delete/rom-change path must invalidate
std::vector<ManagedRom> scanManagedRoms(const std::string& romDir);
void invalidateManagedRoms();

// romm3ds CTR forwarders: maps lowercase rom filename -> tid
// (from sd:/3ds/forwarder/ctr/<tid>.txt bookkeeping, verified against AM)
std::map<std::string, u64> getRommCtrForwarders();

Result deleteForwarder(u64 tid);            // TWL, NAND
Result deleteYanbfForwarder(u64 tid);       // CTR, SD
Result deleteRommCtrForwarder(u64 tid);     // CTR, SD + path file


std::string humanSize(u64 bytes);
