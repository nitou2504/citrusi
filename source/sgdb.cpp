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

bool SgdbClient::ensureSoc() {
    if (socReady) return true;
    socBuffer = (u32*)memalign(0x1000, SOC_BUFFERSIZE);
    if (!socBuffer) { lastError = "soc buffer alloc failed"; return false; }
    Result r = socInit(socBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(r)) {
        free(socBuffer); socBuffer = nullptr;
        lastError = "socInit failed";
        return false;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    socReady = true;
    return true;
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

bool SgdbClient::get(const std::string& url, std::string& out) {
    if (!ensureSoc()) return false;
    out.clear();
    CURL* c = curl_easy_init();
    if (!c) { lastError = "curl init failed"; return false; }
    struct curl_slist* hdrs = nullptr;
    if (!key.empty() && url.find("steamgriddb.com/api/") != std::string::npos)
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + key).c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "romm3ds/0.1");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    // matches the app's httpc SSLCOPT_DisableVerify posture; pinned roots TODO
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode res = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (res != CURLE_OK) {
        lastError = curl_easy_strerror(res);
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
    std::string body;
    if (!get("https://www.steamgriddb.com/api/v2/" + std::string(category) +
             "/game/" + std::to_string(gameId) + "?mimes=image/png", body)) return false;
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

bool SgdbClient::fetchImage(const std::string& url, std::string& out) {
    return get(url, out);
}
