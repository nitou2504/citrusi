#pragma once
#include <string>

// Nintendo LZ11 (0x11) codec, enough for CBMD banner CGFX handling.
std::string lz11Decompress(const std::string& in);
// emits a valid LZ11 stream using literals only (fast, size = n + n/8 + 4)
std::string lz11StoreCompress(const std::string& in);
