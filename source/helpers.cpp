#include <3ds.h>
#include <string>
#include <vector>
#include "aes.h"
#include <cstring>
#include <algorithm>
#include "helpers.hpp"
#include <sys/stat.h>

std::string sha256(u8* data, u32 size) 
{
    u8 buf[0x20]={0};
    Result res = FSUSER_UpdateSha256Context(data,size,buf);
    if (R_SUCCEEDED(res)) {
        return std::string((char*)buf,0x20);
    }
    return "";
}
// taken from pseudocode in GBATEK
u16 crc16Modbus(const void* data, u32 size) {
    u8* values = (u8*)data;
  unsigned short crc = 0xFFFF;
  for (unsigned int i=0; i < size; i++) {
    crc=crc ^ values[i];
    for (unsigned char j=0;j<8;j++) {
      bool carry = (crc & 0x0001) > 0;
      crc=crc >> 1;
      if (carry)
        crc=crc^ 0xA001;
    }
  }
  return crc;
}
void memcpyrev(void* dest, void* source, u32 size) {
    for (u32 i=0; i<size; i++) {
        ((u8*)dest)[i] = ((u8*)source)[(size-1)-i];
    }
}
Result sign(u8* hash, u8* mod, u32 modSize, u8* exp, u32 expSize, u8* signature) {
  psRSAContext ctx({.rsa_bitsize=2048});
  memcpy(ctx.exponent,exp,expSize);
  memcpy(ctx.modulo,exp,modSize);
  return PS_SignRsaSha256(hash,&ctx,signature);
}

void encryptAES128CBC(u8* out, u8* iv, u8* key, u8* data, u32 size) {
  AES_ctx ctx;
  u8 buf[size]={0};
  memcpy(buf,data,size);
  AES_init_ctx_iv(&ctx, key, iv);
  AES_CBC_encrypt_buffer(&ctx, buf, size);
  memcpy(out,buf,size);
}
std::string aligned(const void* data, u64 size, u64 padTo) {
    return aligned(std::string((char*)data,size),padTo);
}
std::string aligned(std::string data, u64 padTo) {
    return data + alignmentPadding(data.size(),padTo);
}
std::string alignmentPadding(u64 size, u64 padTo) {
    u64 count = padTo-(size % padTo);
    if (count > 0 && count < padTo) {
      char pad[count]={0};
      return std::string(pad,count);
    }
    return std::string();
}
// folds Latin-1/Latin-Ext-A diacritics to ASCII so the 3DS system font
// doesn't render '?' for characters like the macron O in Okamiden
std::string utf8FoldLatin(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            unsigned int cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F);
            i += 2;
            const char* r = nullptr;
            if (cp >= 0xC0 && cp <= 0xC5) r = "A";
            else if (cp == 0xC6) r = "AE";
            else if (cp == 0xC7) r = "C";
            else if (cp >= 0xC8 && cp <= 0xCB) r = "E";
            else if (cp >= 0xCC && cp <= 0xCF) r = "I";
            else if (cp == 0xD1) r = "N";
            else if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8) r = "O";
            else if (cp >= 0xD9 && cp <= 0xDC) r = "U";
            else if (cp == 0xDD) r = "Y";
            else if (cp == 0xDF) r = "ss";
            else if (cp >= 0xE0 && cp <= 0xE5) r = "a";
            else if (cp == 0xE6) r = "ae";
            else if (cp == 0xE7) r = "c";
            else if (cp >= 0xE8 && cp <= 0xEB) r = "e";
            else if (cp >= 0xEC && cp <= 0xEF) r = "i";
            else if (cp == 0xF1) r = "n";
            else if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) r = "o";
            else if (cp >= 0xF9 && cp <= 0xFC) r = "u";
            else if (cp == 0xFD || cp == 0xFF) r = "y";
            else if (cp >= 0x100 && cp <= 0x17F) {
                // Latin Extended-A: base letters, one char per codepoint
                static const char base[129] =
                    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh"
                    "IiIiIiIiIi" "Ii" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnn" "Nn"
                    "OoOoOoOo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu"
                    "Ww" "YyY" "ZzZzZz" "s";
                out += base[cp - 0x100];
                continue;
            }
            if (r) out += r;
            else { out += s[i-2]; out += s[i-1]; } // keep as-is
            continue;
        }
        // longer sequences (CJK etc): keep untouched
        int len = ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
        for (int k = 0; k < len && i < s.size(); k++) out += s[i++];
    }
    return out;
}

std::string toLowerCase(std::string s) {
    std::string ret=s;
    std::transform(ret.begin(), ret.end(), ret.begin(),[](unsigned char c){ return std::tolower(c); });
    return ret;
}
std::string readEntireFile(const std::string& path) {
  FILE * f = fopen(path.c_str(),"rb");
  fseek(f,0,SEEK_END);
  size_t s = ftell(f);
  fseek(f,0,SEEK_SET);
  char *buf=(char*)calloc(1,s);
  fread(buf,1,s,f);
  fclose(f);
  std::string ret(buf,s);
  free(buf);
  return ret;
}

bool fileExists (const std::string& name) {
  struct stat buffer;   
  return (stat (name.c_str(), &buffer) == 0); 
}
unsigned long fileSize (const std::string& name) {
  struct stat buffer;   
  if (stat (name.c_str(), &buffer)==0) {
    return buffer.st_size;
  }
  return 0;
}
