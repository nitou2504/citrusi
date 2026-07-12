#pragma once
#include <citro2d.h>
#include <string>
#include <vector>
#include "romm.hpp"

#define COVER_W 118
#define COVER_H 177

// background cover cache: a low-priority worker prefetches every library
// cover (network + decode) into raw RGBA files on SD; the main thread only
// does quick raw reads + GPU uploads.

// (re)start the worker for this library. copies client credentials + list.
void coverCacheStart(const RommClient& client, const std::vector<RommRom>& roms);
// hint: load this rom's cover next
void coverCacheWant(int rommId);
// main thread: load a cached cover into a texture. false = not ready yet.
bool coverCacheLoad(int rommId, C2D_Image* out);
// true when the cover is known to be unavailable (no art on server)
bool coverCacheUnavailable(int rommId);
// forget "no art" misses so covers added later re-fetch (call on library refresh)
void coverCacheClearMisses();
void coverCacheStop();

// pause the worker (blocks until its in-flight job finishes) so foreground
// network/SD work doesn't share httpc with it. Nestable (counted).
void coverCachePause();
void coverCacheResume();
struct CoverCachePause {
    CoverCachePause() { coverCachePause(); }
    ~CoverCachePause() { coverCacheResume(); }
};
