#pragma once
#include <string>
#include <vector>
#include "romm.hpp"

// Background library refresh: a worker refetches one platform's rom list
// (plus missing 3DS title ids) while the UI keeps showing the SD cache.
// Serialize httpc use: start covers only after the refresh has been taken.

// no-op while a job is running or a result is pending. `current` seeds the
// known rommId -> titleId map so already-resolved tids aren't refetched.
void libRefreshStart(const RommClient& client, const std::string& slug,
                     const std::vector<RommRom>& current);
// is a refresh running/pending for this slug? "" = any slug
bool libRefreshRunning(const std::string& slug);
// main thread: collect a finished result (returns true once per job).
// ok=false means the fetch failed — keep the cached list.
bool libRefreshTake(std::string& slug, std::vector<RommRom>& roms, bool& ok);
void libRefreshStop();
