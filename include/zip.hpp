#pragma once
#include <string>
#include <vector>
#include <functional>

// extracts the first entry matching one of exts (lowercase, with dot) from
// zipPath into destDir. progress(done,total) is called as data is written;
// return false to cancel. on success returns true and sets outPath.
// forceName: write as destDir+forceName instead of the zip's inner file name
// (keeps SD names deterministic — the inner name is often a scene release).
bool extractFirstRom(const std::string& zipPath, const std::string& destDir,
                     const std::vector<std::string>& exts,
                     std::string& outPath, std::string& err,
                     std::function<bool(unsigned long long, unsigned long long)> progress = nullptr,
                     const std::string& forceName = "");

// rom extensions inside zips per platform slug ("3ds"/"nds"/"gba")
const std::vector<std::string>& zipRomExtsFor(const std::string& slug);

// peek: platform slug of the first installable rom inside a zip, from its inner
// file extension ("3ds" for .cia, "nds" for .nds/.srl/.ids, "gba" for .gba/.agb).
// Reads only the zip's central directory (no decompression). "" = nothing usable.
std::string zipInnerSlug(const std::string& zipPath);
