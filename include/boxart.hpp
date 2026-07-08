#pragma once
#include <string>
#include <functional>
#include "romm.hpp"

// banner art for a game, as bannertool-layout 256x128 RGBA4444 tiles (0x10000 bytes).
// sources in order: YANBF assets (wide banner art, by gamecode) -> GameTDB coverM
// -> RomM cover (coverPath). "" on total failure (caller keeps template art).
std::string fetchBoxart(RommClient& client, const std::string& romPath,
                        const std::string& coverPath);
