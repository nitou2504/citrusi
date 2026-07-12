#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <cstring>
#include <fstream>
#include "sgdb.hpp"
#include "helpers.hpp"
#include "romm.hpp"   // urlEncodePath
#include "json.hpp"
#include "logger.hpp"

static Logger sgdbLogger("SGDB");

#define SOC_BUFFERSIZE 0x100000
static u32* socBuffer = nullptr;
static bool gNetReady = false;
static bool gNetTried = false;
// one shared easy handle: keep-alive to the same hosts skips a full TLS
// handshake per request (seconds of CPU each on the 3DS)
static CURL* gCurl = nullptr;

// called once from main() init, BEFORE any UI/network runs: soc:u + curl.
// Lazy init mid-flow (first SGDB use) hard-froze the console.
bool sgdbNetBoot() {
    if (gNetTried) return gNetReady;
    gNetTried = true;
    socBuffer = (u32*)memalign(0x1000, SOC_BUFFERSIZE);
    if (!socBuffer) { sgdbLogger.error("soc buffer alloc failed"); return false; }
    Result r = socInit(socBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(r)) {
        free(socBuffer); socBuffer = nullptr;
        char b[48];
        snprintf(b, sizeof(b), "socInit failed %08lX", (unsigned long)r);
        sgdbLogger.error(b);
        return false;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    gNetReady = true;
    sgdbLogger.info("soc+curl ready");
    return true;
}

void sgdbNetExit() {
    if (!gNetReady) return;
    if (gCurl) { curl_easy_cleanup(gCurl); gCurl = nullptr; }
    curl_global_cleanup();
    socExit();
    free(socBuffer);
    socBuffer = nullptr;
    gNetReady = false;
}

bool SgdbClient::loadKey() {
    if (!fileExists(SGDB_ENV_PATH)) return false;
    std::ifstream f(SGDB_ENV_PATH);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find("STEAMGRIDDB_API_KEY=");
        if (eq == std::string::npos) continue;
        std::string v = line.substr(eq + strlen("STEAMGRIDDB_API_KEY="));
        // strip inline comment and whitespace (compose files carry both)
        size_t h = v.find('#');
        if (h != std::string::npos) v = v.substr(0, h);
        size_t b = v.find_first_not_of(" \t\"");
        size_t e = v.find_last_not_of(" \t\r\n\"");
        key = (b == std::string::npos) ? "" : v.substr(b, e - b + 1);
        break;
    }
    if (key.empty()) sgdbLogger.error("no STEAMGRIDDB_API_KEY in sgdb.env");
    return !key.empty();
}

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    ((std::string*)ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

bool SgdbClient::get(const std::string& url, std::string& out, const std::string& extraHeader) {
    if (!sgdbNetBoot()) { lastError = "network not ready"; return false; }
    out.clear();
    // log a compact form of the url (no query keys carry secrets — auth is a header)
    std::string shortUrl = url.substr(0, 100);
    sgdbLogger.info("GET " + shortUrl);
    u64 t0 = osGetTime();
    if (!gCurl) gCurl = curl_easy_init();
    CURL* c = gCurl;
    if (!c) { lastError = "curl init failed"; return false; }
    struct curl_slist* hdrs = nullptr;
    if (!key.empty() && url.find("steamgriddb.com/api/") != std::string::npos)
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + key).c_str());
    if (!extraHeader.empty())
        hdrs = curl_slist_append(hdrs, extraHeader.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "romm3ds/0.1");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // matches the app's httpc SSLCOPT_DisableVerify posture; pinned roots TODO
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode res = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, nullptr);
    char done[96];
    snprintf(done, sizeof(done), "GET done res=%d http=%ld %ums %uB",
             (int)res, status, (unsigned)(osGetTime() - t0), (unsigned)out.size());
    sgdbLogger.info(done);
    if (res != CURLE_OK) {
        lastError = curl_easy_strerror(res);
        // drop the handle: a timed-out connection shouldn't poison reuse
        curl_easy_cleanup(gCurl);
        gCurl = nullptr;
        return false;
    }
    if (status != 200) {
        lastError = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool SgdbClient::search(const std::string& query, std::vector<SgdbGame>& out) {
    out.clear();
    std::string body;
    if (!get("https://www.steamgriddb.com/api/v2/search/autocomplete/" +
             urlEncodePath(query), body)) return false;
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        if (!j.value("success", false)) { lastError = "api error"; return false; }
        for (auto& g : j["data"])
            out.push_back({g["id"].get<int>(), g["name"].get<std::string>()});
    } catch (...) {
        lastError = "bad JSON from search";
        return false;
    }
    return true;
}

bool SgdbClient::assets(const char* category, int gameId, std::vector<SgdbAsset>& out) {
    out.clear();
    // icons come unfiltered: the .ico assets often carry native 48x48/24x24
    // frames (exactly the SMDH sizes); logos stay PNG-only; grids only in the
    // horizontal capsule sizes (~2.14:1, near the 256x128 banner shape)
    std::string filter = (strcmp(category, "icons") == 0) ? "" :
                         (strcmp(category, "grids") == 0)
                             ? "?dimensions=460x215,920x430&mimes=image/png"
                             : "?mimes=image/png";
    std::string body;
    if (!get("https://www.steamgriddb.com/api/v2/" + std::string(category) +
             "/game/" + std::to_string(gameId) + filter, body)) return false;
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        if (!j.value("success", false)) { lastError = "api error"; return false; }
        for (auto& i : j["data"]) {
            SgdbAsset ic;
            ic.id = i["id"].get<int>();
            ic.width = i.value("width", 0);
            ic.height = i.value("height", 0);
            ic.url = i.value("url", "");
            ic.thumbUrl = i.value("thumb", ic.url);
            out.push_back(ic);
        }
    } catch (...) {
        lastError = std::string("bad JSON from ") + category;
        return false;
    }
    return true;
}

bool SgdbClient::icons(int gameId, std::vector<SgdbIcon>& out) {
    return assets("icons", gameId, out);
}

bool SgdbClient::logos(int gameId, std::vector<SgdbAsset>& out) {
    return assets("logos", gameId, out);
}

bool SgdbClient::grids(int gameId, std::vector<SgdbAsset>& out) {
    return assets("grids", gameId, out);
}

bool SgdbClient::fetchImage(const std::string& url, std::string& out) {
    return get(url, out);
}

bool SgdbClient::fetchUrl(const std::string& url, std::string& out,
                          const std::string& headerLine) {
    return get(url, out, headerLine);
}
