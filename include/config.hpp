#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <3ds.h>
#include <citro2d.h>

class Config {
    public:
    bool randomTID;
    bool customTitle;
    bool forceInstall;
    std::vector<std::string> templates;
    unsigned int currentTemplate;
    unsigned long dsiwareCount;
    int selectedLanguage;
    void draw(bool interactive=false);
    void interact(touchPosition *touch);
    void interactKey(u32 *key);
    void save();
    void load();
    const char* languageName();
    void cycleLanguage(int dir);
    Config();
};