#include <3ds.h>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "settings.hpp"
#include "romm.hpp"
#include "helpers.hpp"
#include "json.hpp"
#include "logger.hpp"

static Logger rommLogger("RomM");

std::string base64Encode(const std::string& s) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < s.size()) {
        u32 n = ((u8)s[i] << 16) | ((u8)s[i+1] << 8) | (u8)s[i+2];
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
        i += 3;
    }
    if (i + 1 == s.size()) {
        u32 n = (u8)s[i] << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == s.size()) {
        u32 n = ((u8)s[i] << 16) | ((u8)s[i+1] << 8);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63]; out += "=";
    }
    return out;
}

std::string urlEncodePath(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        }
    }
    return out;
}

bool RommClient::hasConfig() {
    return !this->host.empty() && !this->user.empty();
}

bool RommClient::loadConfig() {
    if (!fileExists(ROMM_CONFIG_FILE)) return false;
    try {
        std::ifstream i(ROMM_CONFIG_FILE);
        nlohmann::json j;
        i >> j;
        if (j.contains("host")) this->host = j["host"];
        if (j.contains("user")) this->user = j["user"];
        if (j.contains("pass")) this->pass = j["pass"];
    } catch (...) {
        return false;
    }
    buildAuth();
    return hasConfig();
}

void RommClient::saveConfig() {
    nlohmann::json j;
    j["host"] = this->host;
    j["user"] = this->user;
    j["pass"] = this->pass;
    std::ofstream o(ROMM_CONFIG_FILE);
    o << j.dump(4) << std::endl;
}

static bool swkbdPrompt(const char* hint, std::string& out, bool password=false) {
    char buf[256] = {0};
    SwkbdState state;
    swkbdInit(&state, SWKBD_TYPE_NORMAL, 2, 255);
    swkbdSetHintText(&state, hint);
    swkbdSetFeatures(&state, SWKBD_DEFAULT_QWERTY);
    if (password) swkbdSetPasswordMode(&state, SWKBD_PASSWORD_HIDE_DELAY);
    if (!out.empty()) swkbdSetInitialText(&state, out.c_str());
    SwkbdButton btn = swkbdInputText(&state, buf, sizeof(buf));
    if (btn != SWKBD_BUTTON_CONFIRM) return false;
    out = std::string(buf);
    return true;
}

bool RommClient::promptConfig() {
    if (this->host.empty()) this->host = "http://";
    if (!swkbdPrompt("RomM server (http://ip[:port])", this->host)) return false;
    // strip trailing slash
    while (!this->host.empty() && this->host.back() == '/') this->host.pop_back();
    if (!swkbdPrompt("RomM username", this->user)) return false;
    if (!swkbdPrompt("RomM password", this->pass, true)) return false;
    buildAuth();
    saveConfig();
    return hasConfig();
}

bool RommClient::promptOne(int field) {
    bool ok = false;
    if (field == 0) {
        if (this->host.empty()) this->host = "http://";
        ok = swkbdPrompt("RomM server (http://ip[:port])", this->host);
        while (ok && !this->host.empty() && this->host.back() == '/') this->host.pop_back();
    } else if (field == 1) {
        ok = swkbdPrompt("RomM username", this->user);
    } else if (field == 2) {
        ok = swkbdPrompt("RomM password", this->pass, true);
    }
    if (ok) {
        buildAuth();
        saveConfig();
    }
    return ok;
}

void RommClient::buildAuth() {
    this->authHeader = "Basic " + base64Encode(this->user + ":" + this->pass);
}

bool RommClient::fetchUrl(const std::string& url, std::string& out) {
    std::string full = url;
    bool external = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    if (!external) full = this->host + url;
    // never send RomM credentials to external hosts
    std::string saved = this->authHeader;
    if (external) this->authHeader = "";
    Result res = get(full, out);
    if (external) this->authHeader = saved;
    return R_SUCCEEDED(res);
}

Result RommClient::get(const std::string& url, std::string& out, u32* statusOut) {
    httpcContext ctx;
    out.clear();
    Result res = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1);
    if (R_FAILED(res)) { lastError = "httpcOpenContext failed"; return res; }
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "romm3ds/0.1");
    if (!this->authHeader.empty())
        httpcAddRequestHeaderField(&ctx, "Authorization", this->authHeader.c_str());
    httpcAddRequestHeaderField(&ctx, "Connection", "Keep-Alive");
    res = httpcBeginRequest(&ctx);
    if (R_FAILED(res)) { lastError = "httpcBeginRequest failed"; httpcCloseContext(&ctx); return res; }
    u32 status = 0;
    res = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(res)) { lastError = "no response from server"; httpcCloseContext(&ctx); return res; }
    if (statusOut) *statusOut = status;
    if (status != 200) {
        char buf[64];
        snprintf(buf, sizeof(buf), "HTTP %lu", status);
        lastError = buf;
        httpcCloseContext(&ctx);
        return -1;
    }
    static u8 chunk[0x20000];
    u32 readSize = 0;
    try {
        do {
            res = httpcDownloadData(&ctx, chunk, sizeof(chunk), &readSize);
            out.append((char*)chunk, readSize);
        } while (res == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
    } catch (...) {
        lastError = "out of memory reading response";
        httpcCloseContext(&ctx);
        return -1;
    }
    httpcCloseContext(&ctx);
    if (R_FAILED(res)) { lastError = "download failed mid-transfer"; return res; }
    return 0;
}

int RommClient::findNdsPlatform() {
    std::string body;
    u32 status = 0;
    if (R_FAILED(get(this->host + "/api/platforms", body, &status))) {
        if (status == 401 || status == 403) lastError = "auth failed (check user/pass)";
        return -1;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        for (auto& p : j) {
            if (p.contains("slug") && p["slug"] == ROMM_PLATFORM_SLUG)
                return p["id"].get<int>();
        }
        lastError = "no 'nds' platform on server";
    } catch (...) {
        lastError = "bad JSON from /api/platforms";
    }
    return -1;
}

bool RommClient::listRoms(int platformId, std::vector<RommRom>& out) {
    out.clear();
    char q[128];
    snprintf(q, sizeof(q), "/api/roms?platform_id=%d&limit=10000&with_char_index=false", platformId);
    std::string body;
    if (R_FAILED(get(this->host + q, body))) return false;
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        if (!j.contains("items")) { lastError = "unexpected rom list shape"; return false; }
        for (auto& r : j["items"]) {
            RommRom rom;
            rom.id = r["id"].get<int>();
            rom.fsName = r.value("fs_name", "");
            rom.name = (r.contains("name") && !r["name"].is_null()) ? r["name"].get<std::string>() : rom.fsName;
            if (rom.name.empty()) rom.name = rom.fsName;
            rom.name = utf8FoldLatin(rom.name);
            rom.sizeBytes = r.value("fs_size_bytes", (u64)0);
            if (r.contains("path_cover_large") && !r["path_cover_large"].is_null()) {
                rom.coverPath = r["path_cover_large"].get<std::string>();
                // strip ?ts= cache-buster: raw datetime breaks nginx (HTTP 400)
                size_t q = rom.coverPath.find('?');
                if (q != std::string::npos) rom.coverPath = rom.coverPath.substr(0, q);
            }
            if (r.contains("path_cover_small") && !r["path_cover_small"].is_null()) {
                rom.coverSmallPath = r["path_cover_small"].get<std::string>();
                size_t q = rom.coverSmallPath.find('?');
                if (q != std::string::npos) rom.coverSmallPath = rom.coverSmallPath.substr(0, q);
            }
            if (r.contains("summary") && !r["summary"].is_null())
                rom.summary = utf8FoldLatin(r["summary"].get<std::string>());
            if (r.contains("metadatum") && r["metadatum"].is_object()) {
                auto& m = r["metadatum"];
                if (m.contains("genres") && m["genres"].is_array()) {
                    for (auto& g : m["genres"]) {
                        if (!rom.genres.empty()) rom.genres += ", ";
                        rom.genres += g.get<std::string>();
                        if (rom.genres.size() > 40) break;
                    }
                }
                if (m.contains("first_release_date") && m["first_release_date"].is_number()) {
                    long long ts = m["first_release_date"].get<long long>();
                    if (ts > 100000000000LL) ts /= 1000; // some sources store ms
                    if (ts > 0) rom.year = 1970 + (int)(ts / 31556952LL);
                }
                if (m.contains("average_rating") && m["average_rating"].is_number())
                    rom.rating = m["average_rating"].get<float>();
            }
            rom.multiFile = r.value("multi", false) || r.value("has_multiple_files", false);
            // multi-part roms download as a zip, not a playable .nds — skip
            if (rom.fsName.empty() || rom.multiFile) continue;
            out.push_back(rom);
        }
    } catch (...) {
        lastError = "bad JSON from /api/roms";
        return false;
    }
    std::sort(out.begin(), out.end(), [](const RommRom& a, const RommRom& b) {
        return toLowerCase(a.name) < toLowerCase(b.name);
    });
    return true;
}

bool RommClient::download(const RommRom& rom, const std::string& destPath,
                          std::function<bool(u64,u64)> progress) {
    std::string url = this->host + "/api/roms/" + std::to_string(rom.id) +
                      "/content/" + urlEncodePath(rom.fsName);
    httpcContext ctx;
    Result res = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1);
    if (R_FAILED(res)) { lastError = "httpcOpenContext failed"; return false; }
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "romm3ds/0.1");
    httpcAddRequestHeaderField(&ctx, "Authorization", this->authHeader.c_str());
    res = httpcBeginRequest(&ctx);
    if (R_FAILED(res)) { lastError = "httpcBeginRequest failed"; httpcCloseContext(&ctx); return false; }
    u32 status = 0;
    res = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(res) || status != 200) {
        char buf[64];
        snprintf(buf, sizeof(buf), "HTTP %lu", status);
        lastError = R_FAILED(res) ? "no response from server" : buf;
        httpcCloseContext(&ctx);
        return false;
    }

    std::string tmpPath = destPath + ".part";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) { lastError = "cannot create file on SD"; httpcCloseContext(&ctx); return false; }

    u32 total = 0, downloaded = 0;
    httpcGetDownloadSizeState(&ctx, &downloaded, &total);
    u64 expected = (total > 0) ? total : rom.sizeBytes;

    static u8 chunk[0x40000];
    u32 readSize = 0;
    u64 written = 0;
    do {
        res = httpcDownloadData(&ctx, chunk, sizeof(chunk), &readSize);
        if (readSize > 0) {
            if (fwrite(chunk, 1, readSize, f) != readSize) {
                lastError = "SD write failed (full?)";
                fclose(f);
                remove(tmpPath.c_str());
                httpcCloseContext(&ctx);
                return false;
            }
            written += readSize;
            if (progress && !progress(written, expected)) {
                lastError = "cancelled";
                fclose(f);
                remove(tmpPath.c_str());
                httpcCancelConnection(&ctx);
                httpcCloseContext(&ctx);
                return false;
            }
        }
    } while (res == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
    fclose(f);
    httpcCloseContext(&ctx);
    if (R_FAILED(res)) {
        lastError = "download failed mid-transfer";
        remove(tmpPath.c_str());
        return false;
    }
    remove(destPath.c_str());
    if (rename(tmpPath.c_str(), destPath.c_str()) != 0) {
        lastError = "rename failed";
        return false;
    }
    return true;
}
