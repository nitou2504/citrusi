#include <3ds.h>
#include <string>
#include <fstream>
#include <cstdarg>
#include <json.hpp>
#include "lang.hpp"
#include "settings.hpp"
#include "helpers.hpp"
#include "logger.hpp"
#include "graphics.h"
Lang gLang=Lang();
Logger jsonLogger("JSON");
static char lang[13][3]=  {
    "jp","en","fr","de","it","es","zh","ko","nl","pt","ru","tw","en"
};

void Lang::loadStrings(u8 bLang) {
    if (bLang>12) {
        bLang=1;
    }
    setFontLanguage(bLang);
    FILE *f;
    std::string filename = ROMFS_LANG_DIR + "default.json";
    std::string langFile = std::string(lang[bLang]) + ".json";

    if (fileExists(SDCARD_LANG_DIR + langFile)) {
        filename = SDCARD_LANG_DIR + langFile;
    } else if (fileExists(ROMFS_LANG_DIR + langFile)) {
        filename = ROMFS_LANG_DIR + langFile;
    }


    f=fopen(filename.c_str(),"r");
    if(f) {
        this->_jLang=nlohmann::json::parse(f,nullptr,false);
        fclose(f);
    }else{
        this->_ready=false;
        return;
    }
    jsonLogger.debug(this->_jLang.dump());
    this->_ready=true;
    //jsonLogger.info(getString("config_foundTemplate"));
    //jsonLogger.info(this->_jLang["config_foundTemplate"]);
}
std::string Lang::parseString(std::string key,...) {
    /// I hate this
    va_list arglist;
    std::string parseThis = this->getString(key);
    char pt[255];
    va_start(arglist, key);
    vsnprintf(pt,255,parseThis.c_str(),arglist);
    va_end(arglist);
    return std::string(pt);
}
std::string Lang::getString(std::string key) {
    if (!this->isReady()) 
        return "unknown";
    if (this->_jLang.contains(key)) {
        return this->_jLang[key];
    }else{
        return key;
    }
}
std::string Lang::getErrorString(u32 code) {
    char key[20];
    sprintf(key, "error_%06lx", code);
    std::string res = this->getString(key);
    if (res == key) {
        if (code == 0xffffffff) {
            std::string ffff = this->getString("error_ffffffff");
            if (ffff != "error_ffffffff") return ffff;
        }
        return this->getString("dialog_checkLog");
    }
    return res;
}
bool Lang::isReady() {
    return this->_ready;
}