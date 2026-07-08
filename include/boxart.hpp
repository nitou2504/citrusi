#pragma once
#include <string>
#include <functional>
#include "romm.hpp"

// fetches the RomM cover, decodes (png/jpg), fits into 256x128 with
// transparent padding, encodes as 3DS ETC1A4 (0x8000 bytes).
// progress(done,total) over encoded tile rows; return false to cancel.
// returns "" on any failure (caller falls back to template art).
std::string fetchBoxartEtc1a4(RommClient& client, const std::string& coverUrl,
                              std::function<bool(int,int)> progress = nullptr);
