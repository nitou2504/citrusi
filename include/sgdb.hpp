#pragma once
#include <string>
#include <vector>

// SteamGridDB client over curl+mbedtls (TLS 1.2 — the 3DS sslc tops out at
// TLS 1.1 and can't reach SGDB's ECDSA-only cert, see docs/GBA-PLAN.md).
// Key is user-supplied, read from sd, never embedded.
#define SGDB_ENV_PATH "sdmc:/3ds/romm3ds/sgdb.env"

struct SgdbGame {
    int id;
    std::string name;
};

struct SgdbIcon {
    int id;
    int width, height;
    std::string url;      // cdn2.steamgriddb.com png
    std::string thumbUrl;
};
// logos share the same shape (id/dims/url/thumb)
typedef SgdbIcon SgdbAsset;

// init soc:u + curl once, at app boot (doing it lazily mid-UI froze the
// console alongside the app's other services). false = SGDB disabled.
bool sgdbNetBoot();
void sgdbNetExit();

class SgdbClient {
    private:
    std::string key;      // Bearer token from sgdb.env
    bool get(const std::string& url, std::string& out, const std::string& extraHeader = "");
    bool assets(const char* category, int gameId, std::vector<SgdbAsset>& out);
    public:
    std::string lastError;

    // parses STEAMGRIDDB_API_KEY=... from SGDB_ENV_PATH (strips comments/space)
    bool loadKey();
    bool hasKey() const { return !key.empty(); }

    // GET /search/autocomplete/{query}
    bool search(const std::string& query, std::vector<SgdbGame>& out);
    // GET /icons/game/{id}?mimes=image/png
    bool icons(int gameId, std::vector<SgdbIcon>& out);
    // GET /logos/game/{id}?mimes=image/png (wide clear-logos, banner source)
    bool logos(int gameId, std::vector<SgdbAsset>& out);
    // fetch an icon/thumb png into memory (no auth needed for cdn)
    bool fetchImage(const std::string& url, std::string& out);
    // generic GET over curl (http or https) with an optional full header
    // line (e.g. "Authorization: Basic ..."). The art path uses this for
    // EVERYTHING — libretro + RomM covers included — because httpc requests
    // from the art flow hung the app while curl kept working.
    bool fetchUrl(const std::string& url, std::string& out,
                  const std::string& headerLine = "");
};
