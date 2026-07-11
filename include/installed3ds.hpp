#pragma once
#include <3ds.h>

// Tracks which RomM 3DS roms are installed on this console, by recording the
// title id of each installed .cia and checking it against the AM title list.
void installed3dsRecord(int rommId, unsigned long long titleId); // call after a successful install
void installed3dsRefresh();                                      // rebuild the AM installed-tid set
bool installed3dsIs(int rommId);                                 // is this rom currently installed? (by recorded tid)
bool installed3dsHasTitle(unsigned long long titleId);           // is this title id installed on the console?
