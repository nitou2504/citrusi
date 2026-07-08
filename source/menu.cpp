#include <3ds.h>
#include <citro2d.h>
#include <filesystem>
#include <vector>
#include <algorithm>
#include "menu.hpp"
extern "C" {
#include "graphics.h"
#include "nds.h"
}
#include "builder.hpp"
#include "dialog.hpp"
#include "settings.hpp"
#include "config.hpp"
#include "helpers.hpp"
#include "lang.hpp"
#include "manage.hpp"
#include "zip.hpp"

#define MAX_DSIWARE 40

static bool isZipName(const std::string& n) {
    std::string l = toLowerCase(n);
    return l.size() > 4 && l.rfind(".zip") == l.size()-4;
}
// expected local playable rom path for a server file name
// (zips extract to <stem>.nds; inner name may differ, best effort)
static std::string rommLocalPath(const std::string& fsName) {
    if (!isZipName(fsName)) return ROMM_ROM_DIR + fsName;
    return ROMM_ROM_DIR + fsName.substr(0, fsName.size()-4) + ".nds";
}

RommClient gRomm;

//class MenuSelection {

    MenuSelection::MenuSelection(std::string s,std::filesystem::path p) {
        this->display=s;
        this->path=p;
    }
    MenuSelection::MenuSelection(MenuSelection* old) {
        this->display=old->display;
        this->path=old->path;
        this->action=old->action;
        this->rommId=old->rommId;
        this->fsName=old->fsName;
        this->sizeBytes=old->sizeBytes;
        this->tid=old->tid;
        this->ytid=old->ytid;
        this->installed=old->installed;
    }
    MenuSelection* MenuSelection::setPath(std::filesystem::path p) {
        this->path=p;
        return this;
    }
    MenuSelection* MenuSelection::setDisplay(std::string s) {
        this->display=s;
        return this;
    }


    Menu::~Menu() {
        for ( auto item : this->entries ) delete item;
        this->entries.clear();
    }
    Menu::Menu() {

    }
    Menu::Menu(std::vector<MenuSelection*> entries) {
        this->entries=entries;
    }
    Menu* Menu::addEntry(MenuSelection* s) {
        this->entries.push_back(s);
        return this;
    }
    void Menu::refreshStrings() {
        for (auto entry : this->entries) {
            if (entry->action == Install_All) {
                entry->display = gLang.getString("menu_installAll");
            }
        }
    }
    void Menu::drawMenu() {
				//draw
            std::string title = this->heading.empty() ? shorten(this->currentDirectory.generic_string(),30) : shorten(this->heading,40);
			drawPanelWithTitle(0,0,0.50, 400,240,MENU_BORDER_HEIGHT,0, BORDER_COLOR,title.c_str(),BORDER_FOREGROUND);
            u16 offset = 0;
            u8 counter=0;
            for (std::vector<MenuSelection*>::iterator entry=this->top;entry!=this->entries.end() && counter < MAX_ENTRY_COUNT;entry++) {
                C2D_DrawRectSolid(MENU_BORDER_HEIGHT,MENU_HEADING_HEIGHT+offset,0,400-MENU_BORDER_HEIGHT,ENTRY_HEIGHT,(entry==this->selection)?HIGHLIGHT_BGCOLOR:BGColor);
                C2DExtra_DrawRectHollow(0,MENU_HEADING_HEIGHT+offset,0,400,ENTRY_HEIGHT,1,BORDER_COLOR);
                C2D_TextBuf buf = C2D_TextBufNew(4096);
                C2D_Text text;
                C2D_Font font = getFont();
                if (font) {
                    C2D_TextFontParse(&text, font, buf, &(*entry)->display.c_str()[0]);
                } else {
                    C2D_TextParse(&text,buf,&(*entry)->display.c_str()[0]);
                }
                float scale = getFontScale(0.67);
                float textheight=0;
                C2D_TextGetDimensions(&text,scale,scale,NULL,&textheight);
                C2D_TextOptimize(&text);
                C2D_DrawText(&text, C2D_WithColor,40,MENU_HEADING_HEIGHT+offset+(ENTRY_HEIGHT/2)-(textheight/2),0,scale,scale,(entry==this->selection)?HIGHLIGHT_FOREGROUND:FOREGROUND_COLOR);
                C2D_TextBufDelete(buf);
                offset+=ENTRY_HEIGHT;
                counter++;
            }

    }
    bool validExtension(const char* extension) {
        char extensions[][5] = {".nds", ".srl", ".ids"};
        for (int i=0;i<3;i++) {
            if (strcasecmp(extension,extensions[i])==0) return true;
        }
        return false;
    }
    Menu* generateMenu(std::filesystem::path path, Menu* prev) {
        delete prev;
        std::vector<MenuSelection*> entries;

        bool ndsFilesVisible=false;
        for (const auto & entry : std::filesystem::directory_iterator(path)) {
            std::string filename = entry.path().filename();
            if (
                filename[0]=='.' ||
                !(entry.is_directory() || validExtension(entry.path().extension().c_str())) ||
                (filename=="_nds" && path.generic_string()=="/")
            )
                continue;
            MenuSelection* menuEntry = new MenuSelection();
            menuEntry->path=entry.path();
            menuEntry->display=filename;
            if (entry.is_directory())  {
                menuEntry->action=OpenFolder;

            }else{
                menuEntry->action=Install;
                ndsFilesVisible=true;

            }
            entries.push_back(menuEntry);
        }
        std::sort(entries.begin(), entries.end(), sortMenuSelections);

        if (path.has_parent_path() && path.parent_path().compare(path)) {
            MenuSelection* prevFolder = new MenuSelection();
            prevFolder->display="..";
            prevFolder->action=OpenFolder;
            prevFolder->path=path.parent_path();
            entries.insert(entries.begin(),prevFolder);
        }
        if (ndsFilesVisible) {
            MenuSelection* installAll = new MenuSelection();
            installAll->action=Install_All;
            installAll->display=gLang.getString("menu_installAll");
            installAll->path=path;
            entries.insert(entries.begin(),installAll);
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory=path.generic_string();
        menu->type=MENU_SD;
        menu->init();
        return menu;
    }
    Menu* generateMainMenu(Menu* prev) {
        delete prev;
        std::vector<MenuSelection*> entries;
        MenuSelection* romm = new MenuSelection();
        romm->display="RomM Library (NDS)";
        romm->action=OpenRommLibrary;
        entries.push_back(romm);
        MenuSelection* manage = new MenuSelection();
        manage->display="Manage Installed";
        manage->action=OpenManage;
        entries.push_back(manage);
        MenuSelection* sd = new MenuSelection();
        sd->display="SD Card Browser";
        sd->action=OpenSDBrowser;
        entries.push_back(sd);
        MenuSelection* cfg = new MenuSelection();
        cfg->display="RomM Server Settings";
        cfg->action=EditRommConfig;
        entries.push_back(cfg);
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_MAIN;
        menu->heading="romm3ds " VERSION;
        menu->init();
        return menu;
    }
    Menu* generateRommMenu(Menu* prev, C3D_RenderTarget* target) {
        if (!gRomm.hasConfig() && !gRomm.loadConfig()) {
            if (!gRomm.promptConfig()) {
                return (prev!=nullptr)?prev:generateMainMenu(nullptr);
            }
        }
        Dialog(target,0,0,320,240,{"Connecting to RomM...",gRomm.host},{},0).handle();
        int platformId = gRomm.findNdsPlatform();
        if (platformId < 0) {
            Dialog(target,0,0,320,240,{"RomM error",gRomm.lastError},{"OK"}).handle();
            return (prev!=nullptr)?prev:generateMainMenu(nullptr);
        }
        Dialog(target,0,0,320,240,{"Loading NDS library..."},{},0).handle();
        std::vector<RommRom> roms;
        if (!gRomm.listRoms(platformId, roms)) {
            Dialog(target,0,0,320,240,{"RomM error",gRomm.lastError},{"OK"}).handle();
            return (prev!=nullptr)?prev:generateMainMenu(nullptr);
        }
        delete prev;
        std::vector<MenuSelection*> entries;
        for (auto& rom : roms) {
            MenuSelection* e = new MenuSelection();
            bool onSD = fileExists(rommLocalPath(rom.fsName));
            e->display=(onSD?"* ":"  ")+rom.name+" ("+humanSize(rom.sizeBytes)+")";
            e->action=RommInstall;
            e->rommId=rom.id;
            e->fsName=rom.fsName;
            e->sizeBytes=rom.sizeBytes;
            e->path=std::filesystem::path(ROMM_ROM_DIR + rom.fsName);
            entries.push_back(e);
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_ROMM;
        menu->heading="RomM: "+std::to_string(roms.size())+" games (* = on SD)";
        menu->init();
        return menu;
    }
    Menu* generateManageMenu(Menu* prev, unsigned long dsiwareCount) {
        delete prev;
        std::vector<MenuSelection*> entries;
        std::vector<ManagedRom> roms = scanManagedRoms(ROMM_ROM_DIR);
        for (auto& rom : roms) {
            MenuSelection* e = new MenuSelection();
            std::string marker = "[ ] ";
            if (rom.installed && rom.yanbfTid) marker="[+Y] ";
            else if (rom.installed) marker="[+] ";
            else if (rom.yanbfTid) marker="[Y] ";
            e->display=marker+rom.display+" ("+humanSize(rom.sizeBytes)+")";
            e->action=ManageRom;
            e->path=std::filesystem::path(rom.path);
            e->tid=rom.tid;
            e->ytid=rom.yanbfTid;
            e->installed=rom.installed;
            entries.push_back(e);
        }
        Menu* menu = new Menu(entries);
        menu->currentDirectory=std::filesystem::path("/");
        menu->type=MENU_MANAGE;
        menu->heading=storageSummary(dsiwareCount)+" +=fwd Y=YANBF";
        menu->init();
        return menu;
    }
    void Menu::init() {
        this->top=entries.begin();
        this->selection=entries.begin();
    }
    void Menu::down() {
        if (this->entries.size()==0) return;
        if (this->selection+1 == this->entries.end()) {
            this->selection=this->entries.begin();
            this->top=this->entries.begin();
        }else{
            if (this->selection+1==this->top+MAX_ENTRY_COUNT) {
                top++;
            }
            this->selection++;
        }
    }
    void Menu::up() {
        if (this->entries.size()==0) return;
        if (this->selection == this->entries.begin()) {
            this->selection=this->entries.end()-1;
            if (this->entries.size() > MAX_ENTRY_COUNT)
                this->top=this->entries.end()-MAX_ENTRY_COUNT;
        }else{
            if (this->selection==this->top) {
                this->top--;
            }
            this->selection--;
        }
    }
    void Menu::action() {
        if (this->entries.size()==0) return;
        this->queue.push(MenuSelection(*this->selection));
    }
    void Menu::pageDown() {
        if (this->entries.size()==0) return;
        if (this->entries.size() <= MAX_ENTRY_COUNT || this->top+MAX_ENTRY_COUNT == this->entries.end()) {

            this->selection=this->entries.end()-1;

        } else {

            for (int i=0;i < MAX_ENTRY_COUNT && this->top+MAX_ENTRY_COUNT != this->entries.end(); i++) {
                this->top++;
                this->selection++;
            }
        }
     }
    void Menu::pageUp() {
        if (this->entries.size()==0) return;
        if (this->top==this->entries.begin()) {
            this->selection=this->entries.begin();
        }else{
            for (int i=0;i<MAX_ENTRY_COUNT && this->top!=this->entries.begin();i++) {
                this->top--;
                this->selection--;
            }
        }
    }
    Menu* Menu::back() {
        switch (this->type) {
            case MENU_MAIN:
                return this;
            case MENU_ROMM:
            case MENU_MANAGE:
                return generateMainMenu(this);
            case MENU_SD:
            default:
                if (this->currentDirectory.generic_string()=="/" || !this->currentDirectory.has_parent_path())
                    return generateMainMenu(this);
                return generateMenu(this->currentDirectory.parent_path(),this);
        }
    }
    bool Menu::hasQueue() {
        return this->queue.size() > 0;
    }

    // shared install helper: template load + buildCIA + overwrite dialog
    static bool installForwarder(Builder* builder, C3D_RenderTarget* target, Config* config,
                                 const std::string& romPath, bool allowRandomTid) {
        if (!(builder->loadTemplate(config->templates.at(config->currentTemplate)))->isSuccess()) {
            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getString("menu_noTemplate")},{gLang.getString("menu_ok")}).handle();
            return false;
        }
        bool randomTID = allowRandomTid && config->randomTID;
        ReturnResult* buildResult = builder->buildCIA(romPath, randomTID, "", config->forceInstall);
        if (buildResult != nullptr && buildResult->code == ERROR_INSTALL_ALREADY_EXISTS) {
            if (Dialog(target,0,0,320,240,{gLang.getString("error_030102"),gLang.getString("menu_overwriteQ")},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                delete buildResult;
                buildResult = builder->buildCIA(romPath, randomTID, "", true);
            }
        }
        bool ok = buildResult->isSuccess();
        if (!ok) {
            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getErrorString(buildResult->code),gLang.parseString("format_hex",(u32)buildResult->code)},{gLang.getString("menu_ok")}).handle();
        }
        delete buildResult;
        return ok;
    }

    Menu* Menu::handleQueue(Builder* builder, C3D_RenderTarget* target, Config* config) {
        if (!this->hasQueue())
            return this;
        if (target==nullptr)
            target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
        while (queue.size() > 0) {
            MenuSelection entry = this->queue.front();
            switch (entry.action) {
                case Install:
                    if (config->dsiwareCount >= MAX_DSIWARE) {
                        Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                        break;
                    }
                    if (!(builder->loadTemplate(config->templates.at(config->currentTemplate)))->isSuccess()) {
                        Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getString("menu_noTemplate")},{gLang.getString("menu_ok")}).handle();
                        break;
                    }
                    if (Dialog(target,0,0,320,240,{gLang.getString("menu_installTitleQ"),entry.path.filename().generic_string()},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                        ReturnResult* buildResult=nullptr;
                        std::string customTitle="";
                        bool randomTID = false;
                        bool forceInstall = false;
                        if (config!=nullptr) {
                            randomTID = config->randomTID;
                            forceInstall = config->forceInstall;
                            if (config->customTitle) {
                                char customTitleBuffer[0x51] = {0};
                                SwkbdState kbstate;
                                swkbdInit(&kbstate,SWKBD_TYPE_NORMAL,2,0x50);
                                swkbdSetHintText(&kbstate,gLang.getString("menu_customTitleQ").c_str());
                                swkbdSetFeatures(&kbstate,SWKBD_MULTILINE | SWKBD_DEFAULT_QWERTY);
                                swkbdInputText(&kbstate,customTitleBuffer,0x51);
                                customTitle=std::string(customTitleBuffer);
                            }
                            buildResult = builder->loadTemplate(config->templates.at(config->currentTemplate));
                            if (buildResult->isSuccess()) {
                                delete buildResult;
                                buildResult = builder->buildCIA(entry.path.generic_string(), randomTID, customTitle, forceInstall);
                            }
                        } else {
                            buildResult = builder->buildCIA(entry.path.generic_string());
                        }

                        if (buildResult != nullptr && (buildResult->code == ERROR_INSTALL_ALREADY_EXISTS)) {
                            if (Dialog(target,0,0,320,240,{gLang.getString("error_030102"),gLang.getString("menu_overwriteQ")},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                                delete buildResult;
                                buildResult = builder->buildCIA(entry.path.generic_string(), randomTID, customTitle, true);
                            }
                        }

                        if (buildResult->isSuccess()) {
                            config->dsiwareCount++;
                            Dialog(target,0,0,320,240,gLang.getString("menu_installComplete"),{gLang.getString("menu_ok")}).handle();
                        }else{
                            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getErrorString(buildResult->code),gLang.parseString("format_hex",(u32)buildResult->code)},{gLang.getString("menu_ok")}).handle();
                        }
                        delete buildResult;
                    }
                    break;
                case Install_All:
                    if (Dialog(target,0,0,320,240,{gLang.getString("menu_installTitleQ"),gLang.getString("menu_allForwarders"),(!entry.path.filename().generic_string().empty())?entry.path.filename().generic_string():"/"},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                        if (!(builder->loadTemplate(config->templates.at(config->currentTemplate)))->isSuccess()) {
                            Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),gLang.getString("menu_noTemplate")},{gLang.getString("menu_ok")}).handle();
                            break;
                        }
                        for (const auto & dEntry : std::filesystem::directory_iterator(entry.path)) {
                            if (config->dsiwareCount >= MAX_DSIWARE) {
                                Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                                break;
                            }
                            std::string filename = dEntry.path().filename();
                            if (filename[0]=='.' || !validExtension(dEntry.path().extension().c_str()))
                                continue;
                            std::string shortname = shorten(dEntry.path().filename().generic_string(),25);
                            Dialog(target,0,0,320,240,{gLang.getString("menu_installing"),shortname},{},0).handle();
                            ReturnResult* buildResult=nullptr;
                            std::string customTitle="";
                            bool randomTID = false;
                            bool forceInstall = false;
                            if (config!=nullptr) {
                                randomTID = config->randomTID;
                                forceInstall = config->forceInstall;
                                if (config->customTitle) {
                                    char customTitleBuffer[0x51] = {0};
                                    SwkbdState kbstate;
                                    swkbdInit(&kbstate,SWKBD_TYPE_NORMAL,2,0x50);
                                    swkbdSetHintText(&kbstate,shortname.c_str());
                                    swkbdSetFeatures(&kbstate,SWKBD_MULTILINE | SWKBD_DEFAULT_QWERTY);
                                    swkbdInputText(&kbstate,customTitleBuffer,0x51);
                                    customTitle=std::string(customTitleBuffer);
                                }
                                buildResult = builder->buildCIA(dEntry.path().generic_string(), randomTID, customTitle, forceInstall);
                            } else {
                                buildResult = builder->buildCIA(dEntry.path().generic_string());
                            }

                            if (buildResult != nullptr && (buildResult->code == ERROR_INSTALL_ALREADY_EXISTS)) {
                                if (Dialog(target,0,0,320,240,{gLang.getString("error_030102"),shortname,gLang.getString("menu_overwriteQ")},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()==0) {
                                    delete buildResult;
                                    buildResult = builder->buildCIA(dEntry.path().generic_string(), randomTID, customTitle, true);
                                }
                            }

                            if (!buildResult->isSuccess()) {
                                Dialog(target,0,0,320,240,{gLang.getString("menu_installFailed"),shortname,gLang.getErrorString(buildResult->code),gLang.parseString("format_hex",(u32)buildResult->code)},{gLang.getString("menu_ok")}).handle();
                            }else{
                                config->dsiwareCount++;
                            }
                            delete buildResult;
                        }
                        Dialog(target,0,0,320,240,gLang.getString("menu_installComplete"),{gLang.getString("menu_ok")}).handle();
                    }
                    break;
                case OpenFolder:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateMenu(entry.path,this);
                case OpenSDBrowser:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateMenu(std::filesystem::path("/"),this);
                case OpenRommLibrary:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateRommMenu(this,target);
                case OpenManage:
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateManageMenu(this,config->dsiwareCount);
                case EditRommConfig:
                    gRomm.loadConfig();
                    if (gRomm.promptConfig())
                        Dialog(target,0,0,320,240,{"Saved.","Server: "+gRomm.host},{"OK"}).handle();
                    break;
                case RommInstall: {
                    std::string dest = ROMM_ROM_DIR + entry.fsName;   // as downloaded (may be .zip)
                    std::string romPath = rommLocalPath(entry.fsName); // playable .nds
                    bool onSD = fileExists(romPath);
                    bool needDownload = true;
                    if (onSD) {
                        int c = Dialog(target,0,0,320,240,{"Already on SD:",entry.fsName},{"Install fwd","Redownload","Cancel"}).handle();
                        if (c==2 || c==-1) break;
                        needDownload = (c==1);
                    } else {
                        if (Dialog(target,0,0,320,240,{"Download + install forwarder?",entry.fsName,humanSize(entry.sizeBytes)},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                            break;
                    }
                    if (config->dsiwareCount >= MAX_DSIWARE) {
                        Dialog(target,0,0,320,240,{gLang.getString("menu_tooManyDSiWare"),std::to_string(config->dsiwareCount)},{gLang.getString("menu_ok")}).handle();
                        break;
                    }
                    if (needDownload) {
                        std::error_code ec;
                        std::filesystem::create_directories(std::filesystem::path(ROMM_ROM_DIR), ec);
                        u64 lastDrawn = 0;
                        bool ok = gRomm.download(RommRom{entry.rommId, entry.display, entry.fsName, entry.sizeBytes, false}, dest,
                            [&](u64 done, u64 total) -> bool {
                                hidScanInput();
                                if (hidKeysDown() & KEY_B) return false; // cancel
                                if (done - lastDrawn < (1<<20) && done != total) return true; // redraw each ~1MB
                                lastDrawn = done;
                                int pct = (total>0)?(int)(done*100/total):0;
                                Dialog(target,0,0,320,240,{"Downloading... (B = cancel)",shorten(entry.fsName,28),humanSize(done)+" / "+humanSize(total)+" ("+std::to_string(pct)+"%)"},{},0).handle();
                                return true;
                            });
                        if (!ok) {
                            if (gRomm.lastError == "cancelled")
                                Dialog(target,0,0,320,240,{"Download cancelled"},{"OK"}).handle();
                            else
                                Dialog(target,0,0,320,240,{"Download failed",gRomm.lastError},{"OK"}).handle();
                            break;
                        }
                        if (isZipName(entry.fsName)) {
                            Dialog(target,0,0,320,240,{"Extracting...",shorten(entry.fsName,28)},{},0).handle();
                            std::string extracted, zerr;
                            if (!extractFirstNds(dest, ROMM_ROM_DIR, extracted, zerr)) {
                                remove(dest.c_str());
                                Dialog(target,0,0,320,240,{"Extract failed",zerr},{"OK"}).handle();
                                break;
                            }
                            remove(dest.c_str());
                            romPath = extracted;
                        }
                    }
                    Dialog(target,0,0,320,240,{gLang.getString("menu_installing"),shorten(entry.fsName,28)},{},0).handle();
                    if (installForwarder(builder,target,config,romPath,false)) {
                        config->dsiwareCount++;
                        Dialog(target,0,0,320,240,{"Installed!",shorten(entry.fsName,28)},{"OK"}).handle();
                        // refresh the on-SD marker
                        for (auto e : this->entries) {
                            if (e->action==RommInstall && e->fsName==entry.fsName && e->display.rfind("* ",0)!=0)
                                e->display="* "+e->display.substr(2);
                        }
                    }
                    break;
                }
                case ManageRom: {
                    std::string name = entry.path.filename().generic_string();
                    std::string fwdState = "fwd: none";
                    if (entry.installed && entry.ytid) fwdState="fwd: TWL + YANBF";
                    else if (entry.installed) fwdState="fwd: TWL installed";
                    else if (entry.ytid) fwdState="fwd: YANBF (CTR)";
                    int c = Dialog(target,0,0,320,240,{name,fwdState},{"Del all","Del fwd","Del ROM","Back"}).handle();
                    if (c==3 || c==-1) break;
                    bool delFwd = (c==0 || c==1);
                    bool delRom = (c==0 || c==2);
                    if (Dialog(target,0,0,320,240,{"Really delete?",name,(c==0?"forwarder + ROM file":(c==1?"forwarder only":"ROM file only"))},{gLang.getString("menu_yes"),gLang.getString("menu_no")}).handle()!=0)
                        break;
                    bool err=false;
                    if (delFwd && entry.installed && entry.tid!=0) {
                        if (R_FAILED(deleteForwarder(entry.tid))) {
                            Dialog(target,0,0,320,240,{"Failed to delete TWL forwarder"},{"OK"}).handle();
                            err=true;
                        } else if (config->dsiwareCount>0) {
                            config->dsiwareCount--;
                        }
                    }
                    if (delFwd && entry.ytid!=0) {
                        if (R_FAILED(deleteYanbfForwarder(entry.ytid))) {
                            Dialog(target,0,0,320,240,{"Failed to delete YANBF forwarder"},{"OK"}).handle();
                            err=true;
                        }
                    }
                    if (delRom && !err) {
                        std::error_code ec;
                        if (!std::filesystem::remove(entry.path, ec)) {
                            Dialog(target,0,0,320,240,{"Failed to delete ROM file"},{"OK"}).handle();
                            err=true;
                        }
                    }
                    if (!err)
                        Dialog(target,0,0,320,240,{"Deleted.",name},{"OK"}).handle();
                    while (this->queue.size() > 0) this->queue.pop();
                    return generateManageMenu(this,config->dsiwareCount);
                }
                default:
                    break;
            }
            this->queue.pop();
        }
        return this;
    }
    bool sortMenuSelections(MenuSelection* a, MenuSelection* b) {
        std::string aDisplay = toLowerCase(a->display);
        std::string bDisplay = toLowerCase(b->display);
        if ((a->action==OpenFolder && b->action==OpenFolder) || (!(a->action==OpenFolder) && !(b->action==OpenFolder)))
            return aDisplay<bDisplay;
        return a->action==OpenFolder;
    }
std::string shorten(std::string s, u16 len) {
    if (len > 8 && s.length() > len) {
        return s.substr(0,5)+"..."+s.substr(s.length()-(len-8));
    }
    return s;
}
