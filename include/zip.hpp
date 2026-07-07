#pragma once
#include <string>

// extracts the first .nds/.srl/.ids entry from zipPath into destDir.
// on success returns true and sets outPath to the extracted file.
bool extractFirstNds(const std::string& zipPath, const std::string& destDir,
                     std::string& outPath, std::string& err);
