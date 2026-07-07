#include <string>
#include <cstring>
#include "zip.hpp"
#include "miniz.h"
#include "helpers.hpp"

static bool ndsExtension(const std::string& lower) {
    return lower.size() > 4 &&
           (lower.rfind(".nds") == lower.size()-4 ||
            lower.rfind(".srl") == lower.size()-4 ||
            lower.rfind(".ids") == lower.size()-4);
}

bool extractFirstNds(const std::string& zipPath, const std::string& destDir,
                     std::string& outPath, std::string& err) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
        err = "not a valid zip";
        return false;
    }
    int found = -1;
    mz_zip_archive_file_stat st;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (st.m_is_directory) continue;
        if (ndsExtension(toLowerCase(std::string(st.m_filename)))) { found = (int)i; break; }
    }
    if (found < 0) {
        mz_zip_reader_end(&zip);
        err = "no .nds inside zip";
        return false;
    }
    // flatten: strip any directory components from the entry name
    std::string name(st.m_filename);
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    outPath = destDir + name;
    std::string tmp = outPath + ".part";
    if (!mz_zip_reader_extract_to_file(&zip, found, tmp.c_str(), 0)) {
        mz_zip_reader_end(&zip);
        remove(tmp.c_str());
        err = "extract failed (SD full?)";
        return false;
    }
    mz_zip_reader_end(&zip);
    remove(outPath.c_str());
    if (rename(tmp.c_str(), outPath.c_str()) != 0) {
        err = "rename failed";
        return false;
    }
    return true;
}
