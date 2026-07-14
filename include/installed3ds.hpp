#pragma once
#include <3ds.h>

// Tracks which RomM 3DS roms are installed on this console, by recording the
// title id of each installed .cia and checking it against the AM title list.
void installed3dsRecord(int rommId, unsigned long long titleId); // call after a successful install
void installed3dsRefresh();                                      // rebuild the AM installed-tid set
bool installed3dsIs(int rommId);                                 // is this rom currently installed? (by recorded tid)
bool installed3dsHasTitle(unsigned long long titleId);           // is this title id installed on the console?
// rommId recorded for a title id (0 = none). Lets the install flow tell a
// reinstall of the same game from a DIFFERENT rom (a rom hack) that shares
// the installed game's title id and would silently replace it.
int installed3dsRommIdForTitle(unsigned long long titleId);
