#pragma once
#include <string>
#include <functional>
#include "romm.hpp"

// banner art for a game, as bannertool-layout 256x128 RGBA4444 tiles (0x10000 bytes).
// sources in order: SD assets -> YANBF assets (by gamecode) -> GameTDB coverM.
// "" on miss — the caller runs the SGDB/RomM-cover/stamp tiers (ART-UX-SPEC).
// sourceOut (optional): "assets" | "yanbf" | "gametdb" | "".
std::string fetchBoxart(RommClient& client, const std::string& romPath,
                        const std::string& coverPath, std::string* sourceOut = nullptr);

// the ROM's own DS icon as a clean white 256x128 stamp ("" if no banner)
std::string dsIconBanner(const std::string& romPath);
