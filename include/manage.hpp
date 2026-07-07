#pragma once
#include <3ds.h>
#include <string>
#include <vector>

struct ManagedRom {
    std::string path;     // sdmc:/roms/nds/xxx.nds
    std::string display;  // file name
    u64 tid;              // expected forwarder title id (0 if unknown)
    bool installed;       // forwarder title present on NAND
    u64 sizeBytes;
};

// expected forwarder TID for a rom, replicating the builder's derivation
// (default template, non-random TID). 0 on failure.
u64 computeForwarderTID(const std::string& ndsPath);

std::vector<u64> getInstalledTwlTitles();
bool twlTitleInstalled(const std::vector<u64>& list, u64 tid);

std::vector<ManagedRom> scanManagedRoms(const std::string& romDir);

Result deleteForwarder(u64 tid);

std::string humanSize(u64 bytes);
// "SD: 12.3GB free | DSiWare: 27/40"
std::string storageSummary(unsigned long dsiwareCount);
