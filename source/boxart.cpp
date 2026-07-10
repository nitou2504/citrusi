#include <3ds.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include "boxart.hpp"
#include "logger.hpp"
#include "helpers.hpp"
#include "settings.hpp"

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

// renders src region (rx,ry,rw,rh) into dst rect (dx,dy,dw,dh) of the canvas
static void renderRegion(const u8* src, int sw, int sh,
                         float rx, float ry, float rw, float rh,
                         u8* canvas, int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            sampleBilinear(src, sw, sh,
                           rx + (float)x * rw / dw,
                           ry + (float)y * rh / dh,
                           &canvas[((dy+y)*BA_W + dx+x)*4]);
}

static std::string tileRgba4444(const u8* canvas) {
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
    return out;
}

// decodes image bytes and renders to 256x128 canvas, YANBF-style:
// 512x256 assets get the animation-mask crop and fill the banner fully;
// anything else is height-fit and centered.
static bool renderImage(const std::string& img, u8* canvas) {
    int sw = 0, sh = 0, comp = 0;
    u8* src = stbi_load_from_memory((const u8*)img.data(), img.size(), &sw, &sh, &comp, 4);
    if (!src) return false;
    memset(canvas, 0, BA_W * BA_H * 4);
    if (sw == 512 && sh == 256) {
        renderRegion(src, sw, sh, 51, 27, 408, 204, canvas, 0, 0, BA_W, BA_H);
    } else {
        int dw = BA_H * sw / sh;
        if (dw > BA_W) dw = BA_W;
        renderRegion(src, sw, sh, 0, 0, sw, sh, canvas, (BA_W - dw) / 2, 0, dw, BA_H);
    }
    stbi_image_free(src);
    return true;
}

static bool readGamecode(const std::string& romPath, char gc[5]) {
    FILE* f = fopen(romPath.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0x0C, SEEK_SET);
    size_t n = fread(gc, 1, 4, f);
    fclose(f);
    gc[4] = 0;
    if (n != 4) return false;
    for (int i = 0; i < 4; i++)
        if (gc[i] < 0x20 || gc[i] > 0x7E) return false;
    return true;
}

// renders the ROM's own DS banner icon (32x32) as a clean white stamp on the
// 256x128 banner, RGBA4444 tiled. "" if the ROM has no banner.
std::string dsIconBanner(const std::string& romPath) {
    FILE* f = fopen(romPath.c_str(), "rb");
    if (!f) return "";
    u32 bannerOff = 0;
    fseek(f, 0x68, SEEK_SET);
    if (fread(&bannerOff, 4, 1, f) != 1 || !bannerOff) { fclose(f); return ""; }
    u8 icon[0x200]; u16 pal[16];
    fseek(f, bannerOff + 0x20, SEEK_SET);
    if (fread(icon, 1, 0x200, f) != 0x200) { fclose(f); return ""; }
    fseek(f, bannerOff + 0x220, SEEK_SET);
    if (fread(pal, 2, 16, f) != 16) { fclose(f); return ""; }
    fclose(f);

    // decode 32x32 (4bpp, 8x8 tiles) to linear RGBA on white
    static u8 icon32[32 * 32 * 4];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int tile = (y / 8) * 4 + (x / 8);
            int off = tile * 32 + (y % 8) * 4 + (x % 8) / 2;
            u8 px = icon[off];
            u8 idx = (x & 1) ? (px >> 4) : (px & 0xF);
            u8* o = &icon32[(y * 32 + x) * 4];
            if (idx == 0) { o[0] = o[1] = o[2] = 255; o[3] = 255; }
            else {
                u16 c = pal[idx];
                o[0] = ((c & 0x1F) << 3) | ((c >> 2) & 7);
                o[1] = (((c >> 5) & 0x1F) << 3) | ((c >> 7) & 7);
                o[2] = (((c >> 10) & 0x1F) << 3) | ((c >> 12) & 7);
                o[3] = 255;
            }
        }

    // white canvas, icon nearest-scaled 3x (96x96) centered — crisp pixel art
    static u8 canvas[BA_W * BA_H * 4];
    for (int i = 0; i < BA_W * BA_H; i++) { canvas[i*4]=255; canvas[i*4+1]=255; canvas[i*4+2]=255; canvas[i*4+3]=255; }
    int scale = 3, dim = 32 * scale;               // 96x96
    int ox = (BA_W - dim) / 2, oy = (BA_H - dim) / 2;
    for (int y = 0; y < dim; y++)
        for (int x = 0; x < dim; x++) {
            u8* s = &icon32[((y/scale) * 32 + (x/scale)) * 4];
            u8* d = &canvas[((oy + y) * BA_W + ox + x) * 4];
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=255;
        }
    boxLogger.info("banner art: DS icon stamp");
    return tileRgba4444(canvas);
}

std::string fetchBoxart(RommClient& client, const std::string& romPath,
                        const std::string& coverPath) {
    static u8 canvas[BA_W * BA_H * 4];
    char gc[5] = {0};
    bool haveGc = readGamecode(romPath, gc);
    std::string img;

    (void)coverPath;
    if (haveGc) {
        std::string gc4(gc, 4), gc3(gc, 3);
        // 1) local YANBF-asset mirror on SD (wide banner logos)
        std::string assetsDir = FORWARDER_DIR + std::string("/assets/");
        for (const std::string& c : {gc4, gc3}) {
            std::string p = assetsDir + c + "/" + c + ".png";
            if (fileExists(p) && renderImage(readEntireFile(p), canvas)) {
                boxLogger.info("banner art: SD assets " + c);
                return tileRgba4444(canvas);
            }
        }
        // 2) YANBF assets over the network (best effort; github TLS may fail)
        std::string base = "https://raw.githubusercontent.com/YANBForwarder/assets/main/assets/";
        if (client.fetchUrl(base + gc4 + "/" + gc4 + ".png", img) && renderImage(img, canvas)) {
            boxLogger.info("banner art: YANBF assets " + gc4);
            return tileRgba4444(canvas);
        }
        if (client.fetchUrl(base + gc3 + "/" + gc3 + ".png", img) && renderImage(img, canvas)) {
            boxLogger.info("banner art: YANBF assets " + gc3);
            return tileRgba4444(canvas);
        }
        // 3) GameTDB box art fallback — same as YANBF's generator. Region from
        //    the 4th game-code letter; art.gametdb.com serves plain http.
        const char* region = "EN";
        switch (gc[3]) {
            case 'D': region = "DE"; break; case 'E': region = "US"; break;
            case 'F': region = "FR"; break; case 'H': region = "NL"; break;
            case 'I': region = "IT"; break; case 'J': region = "JA"; break;
            case 'K': region = "KO"; break; case 'R': region = "RU"; break;
            case 'S': region = "ES"; break; case 'T': region = "US"; break;
            case 'U': region = "AU"; break;
        }
        std::string tdb = "http://art.gametdb.com/ds/coverM/";
        if (client.fetchUrl(tdb + region + "/" + gc4 + ".jpg", img) && renderImage(img, canvas)) {
            boxLogger.info("banner art: GameTDB " + std::string(region) + "/" + gc4);
            return tileRgba4444(canvas);
        }
        if (client.fetchUrl(tdb + "EN/" + gc4 + ".jpg", img) && renderImage(img, canvas)) {
            boxLogger.info("banner art: GameTDB EN/" + gc4);
            return tileRgba4444(canvas);
        }
        boxLogger.info("no asset/GameTDB art for " + gc4 + ", using DS icon");
    }
    // 4) last resort: the game's own DS icon as a clean white stamp
    std::string stamp = dsIconBanner(romPath);
    if (!stamp.empty()) return stamp;
    return "";
}
