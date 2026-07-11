#pragma once
#include <string>
#include <vector>
#include "romm.hpp"

// Background library refresh: a worker refetches one platform's rom list
// (plus missing 3DS title ids) while the UI keeps showing the SD cache.
// Serialize httpc use: start covers only after the refresh has been taken.

// saves a refreshed list to the on-SD cache; runs ON THE WORKER THREAD
typedef void (*LibSaveFn)(const std::string& slug, const std::vector<RommRom>& roms);

// no-op while a job is running or a result is pending. `current` is the list
// the UI shows now — used to diff, to reuse resolved 3DS title ids, and only
// if the list changed does the worker call `save` + clear cover misses.
void libRefreshStart(const RommClient& client, const std::string& slug,
                     const std::vector<RommRom>& current, LibSaveFn save);
// is a refresh running/pending for this slug? "" = any slug
bool libRefreshRunning(const std::string& slug);
// main thread: collect a finished result (returns true once per job).
// ok=false → fetch failed, keep the cached list. changed=false → identical.
bool libRefreshTake(std::string& slug, std::vector<RommRom>& roms, bool& ok, bool& changed);
void libRefreshStop();
