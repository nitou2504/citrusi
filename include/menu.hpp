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
    OpenSettings,
    SettingToggle,
    LocalInstall,          // "Install from SD": install one local file (.cia/.nds/.gba)
    LocalInstallSelected,  // batch-install the Y-marked rows
    LocalInstallAll,       // batch-install every not-yet-installed row
    CleanupCias,           // Manage->3DS: delete .cia installers already installed
    ManageZip              // Manage NDS/GBA: .zip whose ROM never got extracted
};

enum MenuType {
    MENU_MAIN,
    MENU_ROMM,
    MENU_SYSTEMS,
    MENU_MANAGE,
    MENU_SETTINGS,
    MENU_SERVER,
    MENU_LOCAL             // "Browse SD Card": navigable folder browser of local roms
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
        u32 region=0;                // 3DS: SMDH region lockout (0 = unknown)
        int gbaScreen=-1;            // GBA: preset baked into the install (-1 unknown)
        u64 ytid=0;
        u64 rtid=0;
        bool installed=false;
        std::string fwdCia;          // uninstalled forwarder .cia on SD for this rom
        bool selected=false;         // Y-marked for a batch (library/manage/Install-from-SD)
        bool protectedTitle=false;   // 3ds: system title / this app — never uninstall
    MenuSelection(std::string s="",std::filesystem::path p=std::filesystem::path("/"));
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
        MenuType type=MENU_MAIN;
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
        // Y: toggle the batch mark on the Install-from-SD row (else scrollDesc)
        void toggleMark();
        // SELECT: search prompt (RomM view); returns the (possibly new) menu
        Menu* searchPrompt();
        void init();
        void down();
        void pageDown();
        void up();
        void pageUp();
        void action();
        // batch multiselect: Install-from-SD rows via toggleMark (Y), RomM
        // library + Manage rows via toggleSelect (Y). START queues the batch.
        void toggleSelect();     // Y: mark/unmark the current row (skips rows that can't batch)
        void toggleSelectAll();  // R: mark every selectable row / clear all marks
        void selectIndex(size_t i);  // move the cursor to entry i (menu rebuilds keep the row)
        int  selectedCount();    // how many rows are [x]-selected
        void clearSelection();   // drop all marks (menu rebuild / after a batch)
        // START: queue a batch for the marked rows. Returns false when there
        // is nothing to batch here, so the caller quits the app instead.
        bool startBatch();
        // Install-from-SD: X shows/hides files that are already installed
        Menu* toggleShowInstalled();
        Menu* back();
        Menu* handleQueue(Builder* builder, C3D_RenderTarget* target=nullptr, Config* config =nullptr);
        bool hasQueue();

};
Menu* generateLocalMenu(Menu* prev, std::filesystem::path dir = std::filesystem::path());
Menu* generateMainMenu(Menu* prev);
Menu* generateSystemMenu(Menu* prev);
Menu* generateRommMenu(Menu* prev, C3D_RenderTarget* target, const std::string& slug);
Menu* generateSearchAllMenu(Menu* prev, C3D_RenderTarget* target);
Menu* generateManageSystemMenu(Menu* prev);
Menu* generateManageMenu(Menu* prev, unsigned long dsiwareCount, std::string slug,
                         C3D_RenderTarget* target=nullptr);
Menu* generateSettingsMenu(Menu* prev, Config* config);
bool sortMenuSelections(MenuSelection* a, MenuSelection* b);
std::string shorten(std::string s, u16 len);
extern RommClient gRomm;
