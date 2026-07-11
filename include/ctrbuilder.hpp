#pragma once
#include <3ds.h>
#include <string>
#include <functional>
#include "error.hpp"

#define CTR_TEMPLATE_PATH "romfs:/ctr/template.cia"
#define CTR_CONFIG_DIR std::string("sdmc:/3ds/forwarder/ctr/")
// unique-id range for romm3ds CTR forwarders (YANBF uses 0xFF400-0xFF7FF)
#define CTR_UID_BASE  0xFF800
#define CTR_UID_COUNT 0x400
// GBA VC injects get their own range above the forwarders
#define GBA_UID_BASE  0xFFC00
#define GBA_UID_COUNT 0x400
#define GBA_TPL_DIR   std::string("romfs:/gba/")

class CtrBuilder {
    private:
    std::string tpl;        // template CIA bytes
    // section offsets in template
    u32 certOff=0, certSize=0;
    u32 tikOff=0,  tikSize=0;
    u32 tmdOff=0,  tmdSize=0;
    u32 conOff=0;  u64 conSize=0;
    u32 metaOff=0, metaSize=0;
    // within NCCH
    u32 exefsOff=0, exefsSize=0;
    bool parsed=false;

    bool parseTemplate();
    std::string buildSmdh(const std::string& templateSmdh, const std::string& ndsPath,
                          const std::string& title);
    std::string buildBanner(const std::string& templateBanner,
                            const std::string& etc1a4, const std::string& cwav);
    u64 allocateTIDIn(u32 base, u32 count, const std::string& fsName);
    public:
    ReturnResult* initialize();
    // returns free forwarder TID for this rom path, stable across reinstalls
    u64 allocateTID(const std::string& fsName);
    u64 allocateGbaTID(const std::string& fsName);
    // builds + installs the CTR forwarder to SD media.
    // etc1a4: 0x8000-byte 256x128 ETC1A4 boxart ("" = keep template banner art)
    // cwav:   full CWAV blob ("" = keep template sound)
    ReturnResult* buildCIA(const std::string& romPath, const std::string& title,
                           u64 tid, const std::string& etc1a4, const std::string& cwav);
    // builds + installs a GBA VC inject (ROM baked into .code with the
    // AGB_FIRM .CAA footer; template pieces from romfs:/gba, vcoven layout).
    // Streams the ROM from SD — never holds the whole CIA in RAM (32 MB max).
    // icon48: 48*48*2 linear RGB565 ("" = keep template icon art)
    // bannerTex: 0x10000-byte 256x128 RGBA4444 ("" = keep template banner art)
    // progress(done,total) over installed bytes; return false to cancel.
    ReturnResult* buildGbaCIA(const std::string& romPath, const std::string& title,
                              u64 tid, const std::string& icon48,
                              const std::string& bannerTex,
                              std::function<bool(u64,u64)> progress = nullptr);
};
