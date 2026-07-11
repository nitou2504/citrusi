#pragma once
#include <string>
#include <functional>

// Install a .cia from an SD file straight into the title database via am:net,
// streaming in chunks (works for multi-hundred-MB game CIAs). Games install to
// SD, system/TWL titles to NAND (chosen from the CIA's title id).
// progress(done,total) may return false to cancel. Returns true on success.
bool installCiaFromFile(const std::string& path, std::string& err, bool force,
                        std::function<bool(unsigned long long, unsigned long long)> progress);
