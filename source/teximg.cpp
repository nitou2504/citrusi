#include <3ds.h>
#include <citro2d.h>
#include <cstring>
#include <cstdlib>
#include "teximg.hpp"

// stb_image implementation lives in boxart.cpp
#define STBI_NO_STDIO
#include "stb_image.h"

static inline int npow2(int v) {
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

static inline void bilinear(const unsigned char* src, int sw, int sh, float fx, float fy, unsigned char* out) {
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
        out[c] = (unsigned char)(v + 0.5f);
    }
}

std::vector<unsigned char> decodeImageRGBA(const std::string& bytes, int maxW, int maxH, int* w, int* h) {
    std::vector<unsigned char> out;
    int sw = 0, sh = 0, comp = 0;
    unsigned char* src = stbi_load_from_memory((const unsigned char*)bytes.data(), bytes.size(), &sw, &sh, &comp, 4);
    if (!src) return out;

    // scale (up or down) to fit, keeping aspect
    float scale = (float)maxW / sw;
    if ((float)maxH / sh < scale) scale = (float)maxH / sh;
    int dw = (int)(sw * scale), dh = (int)(sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > maxW) dw = maxW;
    if (dh > maxH) dh = maxH;

    out.resize(dw * dh * 4);
    bool bigDownscale = (sw > dw * 3 / 2);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            unsigned char* px = &out[(y * dw + x) * 4];
            if (bigDownscale) {
                // area (box) filter: bilinear alone aliases on strong downscales
                int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
                int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 > sw) x1 = sw;
                if (y1 > sh) y1 = sh;
                u32 acc[4] = {0,0,0,0};
                u32 cnt = (u32)((x1 - x0) * (y1 - y0));
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++)
                        for (int c = 0; c < 4; c++)
                            acc[c] += src[(sy * sw + sx) * 4 + c];
                for (int c = 0; c < 4; c++) px[c] = (unsigned char)(acc[c] / cnt);
            } else {
                bilinear(src, sw, sh, (float)x * sw / dw, (float)y * sh / dh, px);
            }
        }
    }
    stbi_image_free(src);
    *w = dw;
    *h = dh;
    return out;
}

#define TEX_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

bool texFromRGBA(const unsigned char* rgba, int w, int h, C2D_Image* out) {
    // GX display transfers need width >= 64 and height >= 16 — a smaller
    // transfer never completes and gspWaitForPPF() hangs forever (froze the
    // console on the art picker's 32x48 thumbs). Pad the texture, the
    // subtexture still crops to w x h.
    int tw = npow2(w < 64 ? 64 : w), th = npow2(h < 16 ? 16 : h);
    u32* linear = (u32*)linearAlloc(tw * th * 4);
    if (!linear) return false;
    memset(linear, 0, tw * th * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char* px = &rgba[(y * w + x) * 4];
            // GX RGBA8 transfer expects ABGR word order
            linear[y * tw + x] = ((u32)px[0] << 24) | ((u32)px[1] << 16) | ((u32)px[2] << 8) | (u32)px[3];
        }
    }
    C3D_Tex* tex = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    Tex3DS_SubTexture* sub = (Tex3DS_SubTexture*)malloc(sizeof(Tex3DS_SubTexture));
    if (!tex || !sub || !C3D_TexInit(tex, tw, th, GPU_RGBA8)) {
        free(tex); free(sub);
        linearFree(linear);
        return false;
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    GSPGPU_FlushDataCache(linear, tw * th * 4);
    C3D_SyncDisplayTransfer(linear, GX_BUFFER_DIM(tw, th),
                            (u32*)tex->data, GX_BUFFER_DIM(tw, th), TEX_TRANSFER_FLAGS);
    gspWaitForPPF();
    linearFree(linear);

    sub->width = (u16)w;
    sub->height = (u16)h;
    sub->left = 0.0f;
    sub->top = 1.0f;
    sub->right = (float)w / tw;
    sub->bottom = 1.0f - (float)h / th;
    out->tex = tex;
    out->subtex = sub;
    return true;
}

bool loadTexImage(const std::string& bytes, C2D_Image* out, int maxW, int maxH) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba = decodeImageRGBA(bytes, maxW, maxH, &w, &h);
    if (rgba.empty()) return false;
    return texFromRGBA(rgba.data(), w, h, out);
}

void freeTexImage(C2D_Image* img) {
    if (img->tex) {
        C3D_TexDelete(img->tex);
        free(img->tex);
        img->tex = nullptr;
    }
    if (img->subtex) {
        free((void*)img->subtex);
        img->subtex = nullptr;
    }
}
