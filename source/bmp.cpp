#include <3ds.h>
#include <string>
#include <fstream>
#include "bmp.hpp"
#include "logger.hpp"
#include "lang.hpp"

Logger bmplogger("BmpHandler");
Result loadBmpAsIcon(std::string filename, tNDSBannerEx* banner) {
    BMPFileHeader bmpFileHeader={0};
    BMPInfoHeader bmpInfoHeader={0};
    std::ifstream f(filename, std::ios::binary);
    if (f.fail()) {
        bmplogger.error(gLang.parseString("bmp_failedToOpen",filename.c_str()));
        return -1;
    }
    f.read((char*)&bmpFileHeader, sizeof(bmpFileHeader));
    if (bmpFileHeader.magic != 0x4D42) {
        f.close();
        bmplogger.error(gLang.parseString("bmp_invalid",filename.c_str()));
        bmplogger.error(std::to_string(bmpFileHeader.magic));
        return -1;
    }

    u32 infoHeaderSize = 0;
    f.read((char*)&infoHeaderSize, sizeof(u32));
    f.seekg(-4, std::ios::cur);
    if (infoHeaderSize < sizeof(BMPInfoHeader)) {
        f.read((char*)&bmpInfoHeader, infoHeaderSize);
    } else {
        f.read((char*)&bmpInfoHeader, sizeof(BMPInfoHeader));
    }

    if (bmpInfoHeader.width != 32 || (bmpInfoHeader.height != 32 && bmpInfoHeader.height != -32)) {
        bmplogger.error(gLang.parseString("bmp_invalidSize",filename.c_str()));
        bmplogger.error(gLang.parseString("bmp_XxY",filename.c_str(),bmpInfoHeader.width,bmpInfoHeader.height));
        return -1;
    }
    bool topToBottom = bmpInfoHeader.height < 0;

    if (bmpInfoHeader.bpp != 4) {
        bmplogger.error(gLang.parseString("bmp_invalidDepth",filename.c_str()));
        return -1;
    }
    if (bmpInfoHeader.compression != 0) {
        bmplogger.error(gLang.parseString("bmp_isCompressed",filename.c_str()));
    }

    u32 numColors = bmpInfoHeader.colors;
    if (numColors == 0 || numColors > 16) numColors = 16;

    u32 BGR888_palette[16]={0};
    u16 palette[16]={0};
    f.seekg(14 + infoHeaderSize);
    f.read((char*)BGR888_palette, sizeof(u32) * numColors);

    f.seekg(bmpFileHeader.dataOffset);
    u8 BGR888_data[0x200]={0};
    f.read((char*)BGR888_data, 0x200);
    f.close();

    convertColors(palette, BGR888_palette);
    u8 image[0x200]={0};
    convertImage(image, BGR888_data, topToBottom);
    memcpy(banner->icon, image, 0x200);
    memcpy(banner->palette, palette, 32);
    return 0;
}

void convertColors(u16 palette[16], u32 BGR888_palette[16]) {
    for (u8 i=0;i<16;i++) {
        u32 color = BGR888_palette[i];
        u16 r = (color >> 19) & 0b11111;
        u16 g = (color >> 11) & 0b11111;
        u16 b = (color >> 3) & 0b11111;
        palette[i] = b<<10|g<<5|r;
    }
}

void convertImage(u8* store, u8* source, bool topToBottom) {
  toDSPixelOrder(store, source, topToBottom);
}

void toDSPixelOrder(u8* store, u8* source, bool topToBottom) {
    
//   // ds icons are 8x8 pixel boxes, with 4bit width. 4x8 byte boxes
//   // they are arranged in a 4 wide by 4 tall pattern of boxes to 32x32 pixels
   u8 tmp[0x200];
   int offset = 0;
   for (int i=0;i<0x200;i++) {
       source[i]=(source[i]<<4)|(source[i]>>4);
   }

  if (topToBottom) {
      for (int yy=0;yy<4;yy++) {
          for(int y=0;y<8;y++) {
              for (int cc=0;cc<4;cc++) {
                      memcpy(&tmp[y*4+cc*32+yy*128],source+offset,4);
                      offset+=4;
              }
          }
      }
  } else {
      // start at the bottom set of boxes
      for (int yy=3;yy>=0;yy--) {
        // start at the bottom row of the bottom set of boxes
          for(int y=7;y>=0;y--) {
              for (int cc=0;cc<4;cc++) {
                      memcpy(&tmp[y*4+cc*32+yy*128],source+offset,4);
                      offset+=4;
              }
          }
      }
  }
  memcpy(store,tmp,0x200);
}
