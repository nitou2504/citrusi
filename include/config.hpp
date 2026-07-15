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
    bool artNotify=true;     // notify at install when art is missing (off = silent RomM-cover fallback)
    bool manageIcons=true;   // Manage lists: true = titles' own HOME icons, false = RomM covers
    bool deleteAfterInstall=true;  // Browse SD: delete the .cia source after a 3DS install (it's a title-DB duplicate). .nds/.gba are kept — forwarders/injects re-read them.
    int gbaScreen=3;         // GBA_SCREEN_*: default GBA_SCREEN_BRIGHT (pops like 3DS games)
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