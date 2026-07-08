#pragma once
#include <3ds.h>
#include <string>
#include "error.hpp"

#define CTR_TEMPLATE_PATH "romfs:/ctr/template.cia"
#define CTR_CONFIG_DIR std::string("sdmc:/3ds/forwarder/ctr/")
// unique-id range for romm3ds CTR forwarders (YANBF uses 0xFF400-0xFF7FF)
#define CTR_UID_BASE  0xFF800
#define CTR_UID_COUNT 0x400

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
    public:
    ReturnResult* initialize();
    // returns free forwarder TID for this rom path, stable across reinstalls
    u64 allocateTID(const std::string& fsName);
    // builds + installs the CTR forwarder to SD media.
    // etc1a4: 0x8000-byte 256x128 ETC1A4 boxart ("" = keep template banner art)
    // cwav:   full CWAV blob ("" = keep template sound)
    ReturnResult* buildCIA(const std::string& romPath, const std::string& title,
                           u64 tid, const std::string& etc1a4, const std::string& cwav);
};
