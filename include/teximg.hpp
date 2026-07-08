#pragma once
#include <citro2d.h>
#include <string>
#include <vector>

// decodes png/jpg bytes and scales (box-filtered) to fit maxW x maxH.
// returns empty vector on failure; out dims in *w,*h. RGBA8.
std::vector<unsigned char> decodeImageRGBA(const std::string& bytes, int maxW, int maxH, int* w, int* h);

// uploads RGBA8 pixels as a C2D image (GPU transfer; main thread only)
bool texFromRGBA(const unsigned char* rgba, int w, int h, C2D_Image* out);

// decode + upload in one go
bool loadTexImage(const std::string& bytes, C2D_Image* out, int maxW, int maxH);
void freeTexImage(C2D_Image* img);
