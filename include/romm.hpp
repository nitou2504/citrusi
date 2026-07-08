#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <functional>

#define ROMM_CONFIG_FILE FORWARDER_DIR+std::string("/romm.json")
#define ROMM_ROM_DIR std::string("sdmc:/roms/nds/")
#define ROMM_PLATFORM_SLUG "nds"

struct RommRom {
    int id;
    std::string name;    // display name (metadata name or fs_name)
    std::string fsName;  // file name on the server
    std::string coverPath; // server path of large cover ("" if none)
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
    bool hasConfig();
    void buildAuth();

    // returns platform id for ROMM_PLATFORM_SLUG or -1
    int findNdsPlatform();
    bool listRoms(int platformId, std::vector<RommRom>& out);
    // downloads to destPath; progress(downloaded,total) called between chunks,
    // return false from it to cancel (sets lastError="cancelled")
    bool download(const RommRom& rom, const std::string& destPath,
                  std::function<bool(u64,u64)> progress);
    // GET into memory. Server-relative paths (starting '/') get the host prefix
    // + Basic auth; absolute http(s) URLs are fetched WITHOUT credentials.
    bool fetchUrl(const std::string& url, std::string& out);
};

std::string urlEncodePath(const std::string& s);
std::string base64Encode(const std::string& s);
