#include <3ds.h>
#include <string>
#include "lz11.hpp"

std::string lz11Decompress(const std::string& in) {
    std::string out;
    if (in.size() < 4 || (u8)in[0] != 0x11) return out;
    u32 size = ((u8)in[1]) | ((u8)in[2] << 8) | ((u8)in[3] << 16);
    out.reserve(size);
    size_t pos = 4;
    while (out.size() < size && pos < in.size()) {
        u8 flags = in[pos++];
        for (int bit = 7; bit >= 0 && out.size() < size && pos < in.size(); bit--) {
            if (!(flags & (1 << bit))) {
                out += in[pos++];
                continue;
            }
            u8 b0 = in[pos];
            u32 len, disp;
            if ((b0 >> 4) == 0) {          // 8-bit length
                len = ((b0 << 4) | ((u8)in[pos+1] >> 4)) + 0x11;
                disp = (((in[pos+1] & 0xF) << 8) | (u8)in[pos+2]) + 1;
                pos += 3;
            } else if ((b0 >> 4) == 1) {   // 16-bit length
                len = (((b0 & 0xF) << 12) | ((u8)in[pos+1] << 4) | ((u8)in[pos+2] >> 4)) + 0x111;
                disp = (((in[pos+2] & 0xF) << 8) | (u8)in[pos+3]) + 1;
                pos += 4;
            } else {                        // 4-bit length
                len = (b0 >> 4) + 1;
                disp = (((b0 & 0xF) << 8) | (u8)in[pos+1]) + 1;
                pos += 2;
            }
            for (u32 i = 0; i < len; i++)
                out += out[out.size() - disp];
        }
    }
    return out;
}

std::string lz11StoreCompress(const std::string& in) {
    std::string out;
    out.reserve(in.size() + in.size() / 8 + 8);
    out += (char)0x11;
    out += (char)(in.size() & 0xFF);
    out += (char)((in.size() >> 8) & 0xFF);
    out += (char)((in.size() >> 16) & 0xFF);
    for (size_t i = 0; i < in.size(); i += 8) {
        out += (char)0x00; // 8 literal flags
        for (size_t j = i; j < i + 8 && j < in.size(); j++)
            out += in[j];
    }
    return out;
}
