#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <functional>

#define ROMM_CONFIG_FILE FORWARDER_DIR+std::string("/romm.json")
// Multi-platform: NDS installs as a forwarder from roms/nds; 3DS installs the
// .cia; GBA downloads to roms/gba for the VC inject (build not wired yet).
#define ROMM_NDS_DIR std::string("sdmc:/roms/nds/")
#define ROMM_GBA_DIR std::string("sdmc:/roms/gba/")
#define ROMM_CIA_DIR std::string("sdmc:/cia/")
#define ROMM_SLUG_NDS "nds"
#define ROMM_SLUG_3DS "3ds"
#define ROMM_SLUG_GBA "gba"

// SD download/scan dir for a platform slug
inline std::string rommDirFor(const std::string& slug) {
    if (slug == ROMM_SLUG_3DS) return ROMM_CIA_DIR;
    if (slug == ROMM_SLUG_GBA) return ROMM_GBA_DIR;
    return ROMM_NDS_DIR;
}

struct RommRom {
    int id;
    std::string name;    // display name (metadata name or fs_name)
    std::string fsName;  // file name to download (single-file: the rom; multi-file: the chosen .cia)
    int fileId=0;        // >0 = specific file id within a multi-file rom (download via ?file_ids=)
    u64 titleId=0;       // 3ds: resolved from the cia's TMD (0 = not resolved) for install detection
    std::string platformSlug;   // "nds" / "3ds"
    bool installable=true;      // 3ds: true only when a .cia is available; nds: always true
    std::string coverPath;      // server path of large cover ("" if none)
    std::string coverSmallPath; // server path of small cover ("" if none)
    std::string summary;
    std::string genres;  // joined, e.g. "Strategy, Tactical"
    int year=0;          // first release year (0 = unknown)
    float rating=0;      // 0-100 average rating (0 = unknown)
    u64 sizeBytes;
    bool multiFile;
};

class RommClient {
    private:
    std::string authHeader;
    Result get(const std::string& url, std::string& out, u32* statusOut=nullptr);
    public:
    std::string host; // e.g. http://192.168.1.50
    std::string user;
    std::string pass;
    std::string lastError;

    bool loadConfig();
    void saveConfig();
    // prompts with the software keyboard for host/user/pass; returns false if cancelled
    bool promptConfig();
    // prompts a single field (0=host, 1=user, 2=pass); saves on confirm
    bool promptOne(int field);
    bool hasConfig();
    void buildAuth();

    // returns platform id for a slug (e.g. "nds", "3ds") or -1
    int findPlatform(const std::string& slug);
    int findNdsPlatform() { return findPlatform(ROMM_SLUG_NDS); } // back-compat
    // lists roms; tags each with platformSlug + installable
    bool listRoms(int platformId, std::vector<RommRom>& out, const std::string& slug = "");
    // downloads to destPath; progress(downloaded,total) called between chunks,
    // return false from it to cancel (sets lastError="cancelled")
    bool download(const RommRom& rom, const std::string& destPath,
                  std::function<bool(u64,u64)> progress);
    // fetches just the first 16KB of a rom's .cia (header+TMD) into `out`, for title-id resolution
    bool fetchCiaHeader(const RommRom& rom, std::string& out);
    // GET into memory. Server-relative paths (starting '/') get the host prefix
    // + Basic auth; absolute http(s) URLs are fetched WITHOUT credentials.
    bool fetchUrl(const std::string& url, std::string& out);
};

std::string urlEncodePath(const std::string& s);
std::string base64Encode(const std::string& s);
