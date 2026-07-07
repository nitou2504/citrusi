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
    RommInstall,
    OpenManage,
    ManageRom,
    OpenSDBrowser,
    EditRommConfig
};

enum MenuType {
    MENU_MAIN,
    MENU_SD,
    MENU_ROMM,
    MENU_MANAGE
};

class MenuSelection {
    public:
        std::filesystem::path path;
        std::string display;
        MenuAction action;
        // RomM entries
        int rommId=0;
        std::string fsName;
        u64 sizeBytes=0;
        // Manage entries
        u64 tid=0;
        bool installed=false;
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
        Menu(std::vector<MenuSelection*> entries);
        Menu();
        ~Menu();
        Menu* addEntry(MenuSelection* s);

//        Menu* setSelections(std::vector<MenuSelection> s);
//        std::vector<MenuSelection> getSelections();
        void drawMenu();
        void refreshStrings();
        void init();
        void down();
        void pageDown();
        void up();
        void pageUp();
        void action();
        Menu* back();
        Menu* handleQueue(Builder* builder, C3D_RenderTarget* target=nullptr, Config* config =nullptr);
        bool hasQueue();

};
Menu* generateMenu(std::filesystem::path path, Menu* prev);
Menu* generateMainMenu(Menu* prev);
Menu* generateRommMenu(Menu* prev, C3D_RenderTarget* target);
Menu* generateManageMenu(Menu* prev, unsigned long dsiwareCount);
bool sortMenuSelections(MenuSelection* a, MenuSelection* b);
std::string shorten(std::string s, u16 len);
extern RommClient gRomm;
