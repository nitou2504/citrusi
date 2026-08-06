#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <filesystem>
extern "C" {
#include "nds.h"
#include "graphics.h"
}
#include "lang.hpp"
#include "image.hpp"
#include "menu.hpp"
#include "builder.hpp"
#include "settings.hpp"
#include "config.hpp"
#include "helpers.hpp"
#include "covercache.hpp"
#include "librefresh.hpp"
#include "dialog.hpp"
#include "sgdb.hpp"
#include "logger.hpp"
namespace fs = std::filesystem;

// default 3dsx main-thread stack is 32KB; http/json/dialog paths need headroom
extern "C" { unsigned int __stacksize__ = 0x40000; }

#define SYSTEM_APP_COUNT 6
static u64 systemApps[SYSTEM_APP_COUNT] = { 
					 0x00048005484E4441,
					 0x0004800542383841,
					 0x0004800F484E4841,
					 0x0004800F484E4C41,
					 0x00048005484E4443,
					 0x00048005484E444B
};
inline bool isSystemApp(u64 tid) {
	for (int i =0;i<SYSTEM_APP_COUNT;i++) {
		if (tid==systemApps[i]) {
			return true;
		}
	}
	return false;
}
void denit() {
	sgdbNetExit();
	ptmuExit();
	httpcExit();
	amExit();
	cfguExit();
	romfsExit();
	fsExit();
	psExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	
}
int failWait(std::string message) {
	consoleInit(GFX_BOTTOM, NULL);
	std::cout << message << '\n';
	std::cout << "\x1b[21;16H" << ((gLang.isReady())?gLang.getString("main_exit"):"Press Start to exit.") << "\n";
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break; 
	}
	denit();
	return -1;
}
u32 getDsiWareCount() {
	u32 title_count=0;
	Result res = AM_GetTitleCount(MEDIATYPE_NAND, &title_count);
	if (R_SUCCEEDED(res)) {
		u64 titleID[title_count]={0};
		u32 titles_read=0;
		res = AM_GetTitleList(&titles_read,MEDIATYPE_NAND,title_count,titleID);
		if (R_FAILED(res)) {
			title_count=1000;
		}else{
			title_count=0;
			for (u32 i=0;i<titles_read;i++) {
				u16 uCategory = (u16)((titleID[i] >> 32) & 0xFFFF);
				if (uCategory==0x8004 || uCategory==0x8005 || uCategory==0x800F || uCategory==0x8015 ) {
						if (!isSystemApp(titleID[i]))
							title_count+=1;
				}
			}
		}
		return title_count;
	}
	return 1000;
}
Result init() {
	osSetSpeedupEnable(true); // New3DS 804MHz for download/extract/hashing
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

	if (R_FAILED(fsInit())) {
		return failWait("Failed to init fs\n");
	}
	if (R_FAILED(cfguInit())) {
		return failWait("Failed to init cfgu\n");
	}
	if (R_FAILED(psInit())) {
		return failWait("Failed to init ps\n");
	}
	if (R_FAILED(romfsInit())) {
		return failWait("Failed to init romfs\n");
	}
	if (R_FAILED(amInit())) {
		return failWait("Failed to init am\n");
	}
	if (R_FAILED(httpcInit(0))) {
		return failWait("Failed to init httpc\n");
	}
	// battery level + charge state for the header cluster (hbmenu sprites).
	// optional: on failure the header just shows the clock.
	ptmuInit();
	// soc:u + curl for the SGDB art client — at boot, never lazily mid-UI
	// (a first-use socInit alongside live httpc/gsp hard-froze the console)
	sgdbNetBoot();
	if (!fileExists(FORWARDER_DIR)) {
		std::filesystem::create_directories(std::filesystem::path(FORWARDER_DIR));
	}

	return 0;
}

int main()
{
	if (R_FAILED(init()))
		return -1;
	// build stamp: tells the log (and us) exactly which binary is running
	Logger("boot").info(std::string(VERSION) + " built " + __DATE__ + " " + __TIME__);
	
	Config* config = new Config();
	config->dsiwareCount=getDsiWareCount();

	// default to English regardless of console language (the language picker
	// was removed from settings; mixed EN/system-language UI read badly)
	u8 language = 1;   // "en" in the lang table
	if (config->selectedLanguage != 12)
		language = config->selectedLanguage;
	gLang.loadStrings(language);

	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	gLoadingTargets(top, bottom);

	Menu* menu = generateMainMenu(nullptr);
	Builder b;
	b.initialize();

	while (aptMainLoop())
	{

		//Scan all the inputs. This should be done once for each frame
		hidScanInput();
		u32 kDown = hidKeysDown();

		// Touch Handling
		touchPosition touch;
		hidTouchRead(&touch);
		
			// Button Handling

		// START quits, except when a screen has marked rows: begin the batch
		if (kDown & KEY_START) { if (!menu->startBatch()) break; }

		{

			if (kDown & KEY_DOWN) menu->down();

			else if (kDown & KEY_UP) menu->up();
			
			else if (kDown & KEY_RIGHT) menu->pageDown();

			else if (kDown & KEY_LEFT) menu->pageUp();

			else if(kDown & KEY_A) menu->action();

			else if(kDown & KEY_X) {
				if (menu->type == MENU_LOCAL) menu = menu->toggleShowInstalled();
				else menu->scrollDesc(1);
			}

			else if(kDown & KEY_L) menu->scrollDesc(-1); // desc scroll up (Y toggles selection)

			else if(kDown & KEY_R) {
				if (menu->type == MENU_ROMM || menu->type == MENU_MANAGE || menu->type == MENU_LOCAL)
					menu->toggleSelectAll();   // R: select all / none
			}

			else if(kDown & KEY_Y) {
				// Browse/RomM/Manage all use Y to mark a row for a batch
				if (menu->type == MENU_ROMM || menu->type == MENU_MANAGE || menu->type == MENU_LOCAL)
					menu->toggleSelect();
				else menu->toggleMark();   // other screens: Y scrolls the description
			}

			else if(kDown & KEY_SELECT) menu = menu->searchPrompt();

			else if(kDown & KEY_B) menu = menu->back();
		}
		// Draw Screens

		menu->tickBottom(); // cover fetch/decode, outside the frame
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, BGColor);
		C2D_TargetClear(bottom, C2D_Color32f(0,0,0,1));
		C2D_SceneBegin(top);
		menu->drawMenu();
		C2D_SceneBegin(bottom);
		menu->drawBottom(config);
		C3D_FrameEnd(0);

		// Process queue
		menu = menu->handleQueue(&b,bottom,config);

	}
	config->save();
	libRefreshStop();
	coverCacheStop();
	denit();
	return 0;


}
