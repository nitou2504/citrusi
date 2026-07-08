#pragma once
#include <string>
#include <functional>

// extracts the first .nds/.srl/.ids entry from zipPath into destDir.
// progress(done,total) is called as data is written; return false to cancel.
// on success returns true and sets outPath to the extracted file.
bool extractFirstNds(const std::string& zipPath, const std::string& destDir,
                     std::string& outPath, std::string& err,
                     std::function<bool(unsigned long long, unsigned long long)> progress = nullptr);
