#pragma once
#include <citro3d.h>
#include <string>
#include "artfetch.hpp"

// S4 art picker (docs/ART-UX-SPEC.md §4): bottom-screen thumb grid with an
// icon page (5x3, 48px cells) and a banner page (2x2, 128x64 thumbs).
// Controls: D-pad move, L/R page, A use, B skip page, X refine search
// (swkbd), Y icon<->banner page.
//
// Fills entry (sources/ids/query/sgdbGameId) and pieces for the pages the
// user picked on; skipped pages are left untouched. Returns true if at
// least one piece was chosen. iconChosen/bannerChosen (optional) report
// which.
bool artPickerRun(C3D_RenderTarget* target, const std::string& fsName,
                  const std::string& title, const std::string& coverPath,
                  const std::string& slug, ArtEntry& entry, ArtPieces& pieces,
                  bool wantIcon, bool wantBanner,
                  bool* iconChosen = nullptr, bool* bannerChosen = nullptr);
