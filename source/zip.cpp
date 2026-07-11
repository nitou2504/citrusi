#include <string>
#include <cstring>
#include <cstdio>
#include "zip.hpp"
#include "miniz.h"
#include "helpers.hpp"

static bool hasExtension(const std::string& lower, const std::vector<std::string>& exts) {
    for (const std::string& e : exts)
        if (lower.size() > e.size() && lower.rfind(e) == lower.size() - e.size())
            return true;
    return false;
}

const std::vector<std::string>& zipRomExtsFor(const std::string& slug) {
    static const std::vector<std::string> nds = {".nds", ".srl", ".ids"};
    static const std::vector<std::string> gba = {".gba", ".agb"};
    return (slug == "gba") ? gba : nds;
}

struct ExtractCtx {
    FILE* f;
    unsigned long long written;
    unsigned long long total;
    bool cancelled;
    bool ioError;
    std::function<bool(unsigned long long, unsigned long long)>* progress;
};

static size_t extractCb(void* opaque, mz_uint64 ofs, const void* buf, size_t n) {
    (void)ofs;
    ExtractCtx* ctx = (ExtractCtx*)opaque;
    if (fwrite(buf, 1, n, ctx->f) != n) { ctx->ioError = true; return 0; }
    ctx->written += n;
    if (*ctx->progress && !(*ctx->progress)(ctx->written, ctx->total)) {
        ctx->cancelled = true;
        return 0; // abort decompression
    }
    return n;
}

bool extractFirstRom(const std::string& zipPath, const std::string& destDir,
                     const std::vector<std::string>& exts,
                     std::string& outPath, std::string& err,
                     std::function<bool(unsigned long long, unsigned long long)> progress) {
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
        if (hasExtension(toLowerCase(std::string(st.m_filename)), exts)) { found = (int)i; break; }
    }
    if (found < 0) {
        mz_zip_reader_end(&zip);
        err = "no " + (exts.empty() ? std::string("rom") : exts[0]) + " inside zip";
        return false;
    }
    // flatten: strip any directory components from the entry name
    std::string name(st.m_filename);
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    outPath = destDir + name;
    std::string tmp = outPath + ".part";

    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        mz_zip_reader_end(&zip);
        err = "cannot create file on SD";
        return false;
    }
    static char iobuf[0x20000];
    setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

    ExtractCtx ctx = { f, 0, st.m_uncomp_size, false, false, &progress };
    mz_bool ok = mz_zip_reader_extract_to_callback(&zip, found, extractCb, &ctx, 0);
    fclose(f);
    mz_zip_reader_end(&zip);

    if (!ok || ctx.cancelled || ctx.ioError) {
        remove(tmp.c_str());
        err = ctx.cancelled ? "cancelled" : (ctx.ioError ? "SD write failed (full?)" : "extract failed (corrupt zip?)");
        return false;
    }
    remove(outPath.c_str());
    if (rename(tmp.c_str(), outPath.c_str()) != 0) {
        err = "rename failed";
        return false;
    }
    return true;
}
