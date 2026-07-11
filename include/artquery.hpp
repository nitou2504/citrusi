#pragma once
#include <string>
#include <vector>

// Name handling for the art layer (docs/ART-UX-SPEC.md §3): SGDB search
// queries, libretro thumbnail file names, and match-confidence scoring.

enum ArtConfidence {
    ART_MATCH_NONE = 0,   // zero results
    ART_MATCH_WEAK,       // results, none convincing
    ART_MATCH_MEDIUM,     // top result prefix/substring either direction
    ART_MATCH_STRONG      // some result norm-equal to the query
};

// fs_name stem -> SGDB search query: strip extension(s), erase (...)/[...]
// blocks, _ -> space, flip the No-Intro article ("Zelda, The - X" ->
// "The Zelda - X"), collapse whitespace (ES-DE removeParenthesis + extras)
std::string artSanitizeQuery(const std::string& fsName);

// strip known rom/archive extensions (.zip/.gba/.nds/...) — the shared stem
// keys art.json so "game.zip" (library) and "game.gba" (SD) agree
std::string artStripExts(const std::string& name);

// normalize for comparison: lowercase, fold common accents/macrons, & -> and,
// drop trailing platform tokens ("gba"/"ds"/"nds"), keep alnum only
std::string artNorm(const std::string& name);

// scores autocomplete results against the sanitized query.
// *bestIdx = index of the winning result (valid for STRONG/MEDIUM)
ArtConfidence artConfidence(const std::string& query,
                            const std::vector<std::string>& resultNames,
                            int* bestIdx);

// libretro thumbnail file names to try, in order: the exact No-Intro stem,
// then (on 404) the stem with (Translated)/[...] tags stripped. RetroArch
// filename rule applied (&*/:`<>?\| -> _). NOT url-encoded.
std::vector<std::string> libretroNameVariants(const std::string& fsName);
