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

#include "rg_etc1.h"

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

    // ETC1A4 encode, 3DS tiling (tex3ds layout)
    static bool etcInit = false;
    if (!etcInit) { rg_etc1::pack_etc1_block_init(); etcInit = true; }
    rg_etc1::etc1_pack_params params;
    params.clear();
    params.m_quality = rg_etc1::cLowQuality;

    std::string out;
    out.reserve(BA_W * BA_H); // 1 byte/px
    int tilesY = BA_H / 8, tilesX = BA_W / 8;
    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            for (int j = 0; j < 8; j += 4) {
                for (int i = 0; i < 8; i += 4) {
                    u8 inBlock[4*4*4];
                    u8 outAlpha[8] = {0};
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++) {
                            const u8* p = &canvas[((ty*8 + j + y)*BA_W + tx*8 + i + x)*4];
                            inBlock[y*16 + x*4 + 0] = p[0];
                            inBlock[y*16 + x*4 + 1] = p[1];
                            inBlock[y*16 + x*4 + 2] = p[2];
                            inBlock[y*16 + x*4 + 3] = 0xFF;
                            u8 a4 = p[3] >> 4;
                            if (y & 1) outAlpha[2*x + y/2] |= (a4 << 4);
                            else       outAlpha[2*x + y/2] |= a4;
                        }
                    }
                    u8 outBlock[8];
                    rg_etc1::pack_etc1_block(outBlock, (const unsigned int*)inBlock, params);
                    out.append((char*)outAlpha, 8);
                    for (int k = 7; k >= 0; k--) out += (char)outBlock[k];
                }
            }
        }
        if (progress && !progress(ty + 1, tilesY)) return "";
    }
    return out;
}
