#include <3ds.h>
#include <cstring>
#include <vector>
#include "boxart.hpp"
#include "logger.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

static Logger boxLogger("Boxart");

#define BA_W 256
#define BA_H 128

// bilinear sample from RGBA src
static inline void sampleBilinear(const u8* src, int sw, int sh, float fx, float fy, u8* out) {
    int x0 = (int)fx, y0 = (int)fy;
    if (x0 >= sw-1) x0 = sw-2;
    if (y0 >= sh-1) y0 = sh-2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    float dx = fx - x0, dy = fy - y0;
    for (int c = 0; c < 4; c++) {
        float v = src[(y0*sw+x0)*4+c]   * (1-dx)*(1-dy)
                + src[(y0*sw+x0+1)*4+c] * dx*(1-dy)
                + src[((y0+1)*sw+x0)*4+c]   * (1-dx)*dy
                + src[((y0+1)*sw+x0+1)*4+c] * dx*dy;
        out[c] = (u8)(v + 0.5f);
    }
}

// bannertool banner texture: 256x128 RGBA4444, 8x8 tiles, Morton order in-tile
std::string fetchBoxartEtc1a4(RommClient& client, const std::string& coverUrl,
                              std::function<bool(int,int)> progress) {
    if (coverUrl.empty()) return "";
    std::string img;
    if (!client.fetchUrl(coverUrl, img) || img.empty()) {
        boxLogger.info("cover fetch failed: " + client.lastError);
        return "";
    }
    int sw = 0, sh = 0, comp = 0;
    u8* src = stbi_load_from_memory((const u8*)img.data(), img.size(), &sw, &sh, &comp, 4);
    if (!src) {
        boxLogger.info("cover decode failed");
        return "";
    }

    // fit into 256x128: height 128, keep aspect, center horizontally (YANBF style)
    static u8 canvas[BA_W * BA_H * 4];
    memset(canvas, 0, sizeof(canvas)); // transparent padding
    int dw = BA_H * sw / sh;
    if (dw > BA_W) dw = BA_W;
    int xoff = (BA_W - dw) / 2;
    for (int y = 0; y < BA_H; y++)
        for (int x = 0; x < dw; x++)
            sampleBilinear(src, sw, sh,
                           (float)x * sw / dw, (float)y * sh / BA_H,
                           &canvas[(y*BA_W + xoff + x)*4]);
    stbi_image_free(src);

    // RGBA4444 tiled (bannertool image_data_to_tiles layout)
    std::string out(BA_W * BA_H * 2, '\0');
    u16* dst = (u16*)&out[0];
    for (u32 y = 0; y < BA_H; y++) {
        for (u32 x = 0; x < BA_W; x++) {
            u32 index = (((y >> 3) * (BA_W >> 3) + (x >> 3)) << 6) +
                        ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                         ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
            const u8* p = &canvas[(y*BA_W + x)*4];
            dst[index] = (u16)(((p[0] & ~0xF) << 8) | ((p[1] & ~0xF) << 4) |
                               (p[2] & ~0xF) | (p[3] >> 4));
        }
    }
    if (progress) progress(1, 1);
    return out;
}
