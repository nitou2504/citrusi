#include <filesystem>
#include <vector>
#include <queue>
#include "builder.hpp"
#include "config.hpp"
#include "romm.hpp"
//#include "menu.h"
#define ATTRIB_DIR 1
#define ATTRIB_HIDDEN 1<<1

enum MenuAction {
    OpenFolder,
    ReturnToMenu,
    Install,
    Install_All,
    OpenRommLibrary,
    OpenPlatform,
    OpenSearchAll,
    RefreshLibraries,
    RommInstall,
    BatchRommInstall,   // START in the library: install every [x]-selected game
    BatchManage,         // START in Manage: uninstall / change art for selected
    OpenManage,
    ManageRom,
    OpenSDBrowser,
    EditRommConfig,
    OpenSettings,
    SettingToggle
};

enum MenuType {
    MENU_MAIN,
    MENU_SD,
    MENU_ROMM,
    MENU_SYSTEMS,
    MENU_MANAGE,
    MENU_SETTINGS,
    MENU_SERVER
};

class MenuSelection {
    public:
        std::filesystem::path path;
        std::string display;
        MenuAction action;
        // RomM entries
        int rommId=0;
        std::string fsName;
        int fileId=0;                // multi-file: specific file id to download
        u64 titleId=0;               // 3ds: resolved cia title id (for install detection)
        std::string platformSlug;    // "nds" / "3ds" for RomM entries
        bool installable=true;
        std::string title;
        std::string coverPath;
        std::string coverSmallPath;
        std::string summary;
        std::string genres;
        int year=0;
        float rating=0;
        u64 sizeBytes=0;
        // Manage entries
        u64 tid=0;
        u64 ytid=0;
        u64 rtid=0;
        bool installed=false;
        std::string fwdCia;          // uninstalled forwarder .cia on SD for this rom
        bool selected=false;         // batch multiselect mark (Y); reset on menu rebuild
    MenuSelection(std::string s="",std::filesystem::path p=std::filesystem::path("/"));
    MenuSelection(MenuSelection* old);
    MenuSelection* setPath(std::filesystem::path p);
    MenuSelection* setDisplay(std::string s);

};
class Menu {
    private:
        std::vector<MenuSelection*>::iterator selection;
        std::vector<MenuSelection*>::iterator top;
        std::vector<MenuSelection*> entries;
        std::queue<MenuSelection> queue;
    public:
        std::filesystem::path currentDirectory;
        MenuType type=MENU_SD;
        std::string heading;
        std::string filter; // active search query (RomM view)
        std::string platformSlug; // active platform (RomM view); "" = cross-system search
        bool crossSystem=false;   // RomM view spanning all systems
        Menu(std::vector<MenuSelection*> entries);
        Menu();
        ~Menu();
        Menu* addEntry(MenuSelection* s);

//        Menu* setSelections(std::vector<MenuSelection> s);
//        std::vector<MenuSelection> getSelections();
        void drawMenu();
        // bottom-screen: details panel for RomM/Manage menus, config otherwise
        void drawBottom(Config* config);
        // pre-frame work for the bottom panel (cover fetch/decode) — call
        // OUTSIDE the C3D frame
        void tickBottom();
        // scroll the description panel (X/Y)
        void scrollDesc(int dir);
        // SELECT: search prompt (RomM view); returns the (possibly new) menu
        Menu* searchPrompt();
        void refreshStrings();
        void init();
        void down();
        void pageDown();
        void up();
        void pageUp();
        void action();
        // batch multiselect (RomM library + Manage lists)
        void toggleSelect();     // Y: mark/unmark the current row (skips rows that can't batch)
        int  selectedCount();    // how many rows are [x]-selected
        void clearSelection();   // drop all marks (menu rebuild / after a batch)
        // START: queue a batch for the selected rows. false = nothing selected
        // (caller falls back to quitting the app).
        bool startBatch();
        Menu* back();
        Menu* handleQueue(Builder* builder, C3D_RenderTarget* target=nullptr, Config* config =nullptr);
        bool hasQueue();

};
Menu* generateMenu(std::filesystem::path path, Menu* prev);
Menu* generateMainMenu(Menu* prev);
Menu* generateSystemMenu(Menu* prev);
Menu* generateRommMenu(Menu* prev, C3D_RenderTarget* target, const std::string& slug);
Menu* generateSearchAllMenu(Menu* prev, C3D_RenderTarget* target);
Menu* generateManageSystemMenu(Menu* prev);
Menu* generateManageMenu(Menu* prev, unsigned long dsiwareCount, std::string slug);
Menu* generateSettingsMenu(Menu* prev, Config* config);
bool sortMenuSelections(MenuSelection* a, MenuSelection* b);
std::string shorten(std::string s, u16 len);
extern RommClient gRomm;
