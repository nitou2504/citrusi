#pragma once
#include <string>
#include <functional>
#include "romm.hpp"

// banner art for a game, as bannertool-layout 256x128 RGBA4444 tiles (0x10000 bytes).
// sources in order: SD assets -> YANBF assets (by gamecode) — the purpose-made
// wide banners only. "" on miss; the caller runs the logo tiers, then
// gametdbBoxart(), then the RomM-cover/stamp fallbacks (ART-UX-SPEC).
// sourceOut (optional): "assets" | "yanbf" | "".
std::string fetchBoxart(RommClient& client, const std::string& romPath,
                        const std::string& coverPath, std::string* sourceOut = nullptr);

// GameTDB coverM box art by gamecode ("" on miss). A box cover like the RomM
// one — sits just above it in the chain, below the logo tiers.
std::string gametdbBoxart(RommClient& client, const std::string& romPath);

// the ROM's own DS icon as a clean white 256x128 stamp ("" if no banner)
std::string dsIconBanner(const std::string& romPath);
