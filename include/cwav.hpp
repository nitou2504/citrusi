#pragma once
#include <string>
#include "romm.hpp"

// converts a RIFF WAV (PCM 8/16-bit) to CWAV. "" on failure.
std::string wavToCwav(const std::string& wav);

// builds a minimal, known-good silent CWAV (short PCM16 mono run of zeros)
// via the same writer bannertool uses. Used to force a jingle-free banner.
std::string silentCwav();

// reads the gamecode from the rom, fetches the matching YANBF banner sound
// (raw.githubusercontent.com/YANBForwarder/assets) and converts it.
// "" when unavailable — caller keeps the template sound.
std::string fetchGameSound(RommClient& client, const std::string& romPath);
