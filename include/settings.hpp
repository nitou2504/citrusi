#pragma once

#define VERSION "v1.1.1"
//#define DEBUG

#define FORWARDER_DIR std::string("sdmc:/3ds/forwarder")
#define SDCARD_TEMPLATE_DIR FORWARDER_DIR+std::string("/templates/")
#define ROMFS_TEMPLATE_DIR std::string("romfs:/templates/")
#define ROMFS_LANG_DIR std::string("romfs:/lang/")
#define SDCARD_BANNER_PATH FORWARDER_DIR+std::string("/banners/")
#define SDCARD_ICON_PATH FORWARDER_DIR+std::string("/icons/")
#define SDCARD_LANG_DIR FORWARDER_DIR+std::string("/lang/")
#define ENTRY_HEIGHT 32
#define FILELIST_HEIGHT (240-MENU_HEADING_HEIGHT-MENU_BORDER_HEIGHT)
#define MENU_BORDER_HEIGHT 4
#define MENU_HEADING_HEIGHT 28
#define MAX_ENTRY_COUNT ((FILELIST_HEIGHT-(FILELIST_HEIGHT%ENTRY_HEIGHT))/ENTRY_HEIGHT)

// flat dark design system
#define COL_BG        HexColor(0x10141C)
#define COL_SURFACE   HexColor(0x1A2130)
#define COL_ELEV      HexColor(0x232C40)
#define COL_ACCENT    HexColor(0x7C5CFF)
#define COL_TEXT      HexColor(0xF2F5FA)
#define COL_TEXT_DIM  HexColor(0x8E9AB3)

// legacy names (dialogs/config inherit the new palette)
#define BGColor COL_BG
#define BORDER_COLOR COL_ELEV
#define BORDER_FOREGROUND COL_TEXT_DIM
#define HIGHLIGHT_BGCOLOR COL_ACCENT
#define HIGHLIGHT_FOREGROUND COL_TEXT
#define FOREGROUND_COLOR COL_TEXT_DIM
