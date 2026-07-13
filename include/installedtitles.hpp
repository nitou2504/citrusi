#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include <functional>

// Every title installed on the console, with its size and its SMDH name —
// the source for "Manage -> 3DS" (which lists ALL installed apps, not just
// the ones that match the RomM library) and for the storage breakdown.
//
// Names come from each title's ExeFS 'icon' (SMDH), readable from userland
// via FSUSER_OpenFileDirectly on ARCHIVE_SAVEDATA_AND_CONTENT. That's one FS
// open per title, so the result is cached on SD (smdhnames.json, keyed by
// tid|version) — the first pass costs a second, later ones are free.

enum TitleKind {
    TK_APP,          // regular 3DS application
    TK_DEMO,         // 0002: demo
    TK_NDS_FWD,      // ours: romm3ds CTR forwarder for a .nds rom
    TK_GBA_INJECT,   // ours: GBA VC inject
    TK_YANBF,        // YANBF forwarder (made by the PC tool)
    TK_UPDATE,       // 000E: update data of some app
    TK_DLC,          // 008C: add-on content
    TK_DSIWARE,      // 8004/8005: TWL title on NAND (banner, not SMDH)
    TK_SELF,         // this app
    TK_SYSTEM        // system title / anything we must not touch
};

struct InstalledTitle {
    u64 tid = 0;
    FS_MediaType media = MEDIATYPE_SD;
    u64 sizeBytes = 0;
    u16 version = 0;
    std::string name;              // SMDH English short title (product code / hex tid as fallback)
    TitleKind kind = TK_APP;
    bool protectedTitle = false;   // never list, never uninstall (this app, system titles)
};

// installed applications on SD (cat 0000, plus 0002 demos when asked), with
// names resolved. progress(done, total) fires only for titles whose name is
// not cached yet, so the caller can put a loading screen behind the first pass.
std::vector<InstalledTitle> listInstalledApps(bool includeDemos = true,
                                              bool sortBySize = true,
                                              const std::function<void(int,int)>& progress = nullptr);

// update (0004000E) / DLC (0004008C) titles that belong to an app
struct TitleExtras {
    std::vector<u64> tids;
    u64 bytes = 0;
    u32 updates = 0;
    u32 dlc = 0;
    bool empty() const { return tids.empty(); }
};
TitleExtras findTitleExtras(u64 appTid);

// bytes + counts per class, and SD free space. Cached until invalidated.
struct StorageTally {
    u64 ndsFwdBytes = 0;   u32 ndsFwdCount = 0;    // our CTR forwarders (SD)
    u64 yanbfBytes = 0;    u32 yanbfCount = 0;     // YANBF forwarders (SD)
    u64 dsiwareBytes = 0;  u32 dsiwareCount = 0;   // TWL forwarders / DSiWare (NAND)
    u64 gbaBytes = 0;      u32 gbaCount = 0;       // our GBA injects (SD)
    u64 appBytes = 0;      u32 appCount = 0;       // other 3DS apps + demos (SD)
    u64 extraBytes = 0;    u32 extraCount = 0;     // updates + DLC (SD)
    // rom files on the card (sd:/roms/...): the forwarders are tiny, the roms
    // they point at are not — so a DS row that omits them is meaningless
    u64 ndsRomBytes = 0;   u32 ndsRomCount = 0;
    u64 gbaRomBytes = 0;   u32 gbaRomCount = 0;
    // .cia installer files left on the card (sd:/cias, sd:/cia): once the
    // title is installed the file is a duplicate of it
    u64 ciaBytes = 0;      u32 ciaCount = 0;
    u64 ciaDoneBytes = 0;  u32 ciaDoneCount = 0;   // ...whose title IS installed
    u64 sdFreeBytes = 0;
    // "Nintendo DS" as the Manage tab sees it: every kind of DS forwarder
    u64 dsBytes() const { return ndsFwdBytes + yanbfBytes + dsiwareBytes + ndsRomBytes; }
    u32 dsCount() const { return ndsFwdCount + yanbfCount + dsiwareCount; }
    u64 gbaTotalBytes() const { return gbaBytes + gbaRomBytes; }
};

// a .cia sitting on the card; installed=true means its title is on the console
// already, so the file is a pure duplicate and safe to delete.
struct CiaFile {
    std::string path;
    std::string name;
    u64 sizeBytes = 0;
    u64 tid = 0;
    bool installed = false;

};
StorageTally computeStorageTally();
// true while computeStorageTally() would return the cached value (callers can
// skip a CoverCachePause for the cache-hit path)
bool storageTallyCached();

// drop the cached tally (and reload the name cache) after an install/uninstall
void installedTitlesInvalidate();

// every .cia still sitting in sd:/cias or sd:/cia, with its install state
std::vector<CiaFile> listCiaFiles();

// the title's own 48x48 HOME icon, saved from its SMDH: raw RGBA8 (48*48*4).
// "" when we've never read that title's SMDH. Free art for every 3DS game.
std::string titleIconRGBA(u64 tid);
