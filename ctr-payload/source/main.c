/*
romm3ds CTR forwarder payload
Based on YANBF forwarder (MIT) Copyright (C) 2022-present lifehackerhansol

Reads the rom path from sd:/3ds/forwarder/ctr/<TITLEID>.txt (written by the
romm3ds generator at install time), stages it for NTR_Forwarder, then chains
to the YANBF bootstrap TWL title (FWDR).
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <3ds.h>

static FILE* plog;
static void logline(const char* fmt, ...) {
    if (!plog) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(plog, fmt, args);
    va_end(args);
    fputc('\n', plog);
    fflush(plog);
}

static void fatal(const char* msg, Result code) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("%s\n\ncode: %08lX\n\nPress START to exit.", msg, (u32)code);
    while (aptMainLoop()) {
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
    }
    gfxExit();
}

int main() {
    Result amres = amInit();
    plog = fopen("sdmc:/3ds/forwarder/payload.log", "w");
    logline("payload start, amInit=%08lX", (u32)amres);

    u64 tid = 0;
    Result r = APT_GetProgramID(&tid);
    logline("APT_GetProgramID=%08lX tid=%016llX", (u32)r, tid);

    char cfgPath[64];
    snprintf(cfgPath, sizeof(cfgPath), "sdmc:/3ds/forwarder/ctr/%016llX.txt", tid);

    char line[512] = {0};
    FILE* f = fopen(cfgPath, "r");
    if (!f) {
        logline("missing %s", cfgPath);
        fatal("Missing forwarder path file.\nReinstall this forwarder with romm3ds.", 0);
        amExit();
        return 1;
    }
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    size_t len = strlen(line);
    while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
    logline("path=[%s]", line);
    if (!len) {
        fatal("Empty forwarder path file.\nReinstall this forwarder with romm3ds.", 0);
        amExit();
        return 1;
    }

    mkdir("sdmc:/_nds", 0777);
    mkdir("sdmc:/_nds/ntr-forwarder", 0777);
    FILE* path = fopen("sdmc:/_nds/ntr-forwarder/path.txt", "w");
    if (path) {
        fputs(line, path);
        fclose(path);
        logline("path.txt written");
    } else {
        logline("path.txt write FAILED");
    }

    Result ret = 0;
    AM_TitleEntry bootstrap;
    u64 fwdr = 0x0004800546574452ULL;
    ret = AM_GetTitleInfo(MEDIATYPE_NAND, 1, &fwdr, &bootstrap);
    logline("AM_GetTitleInfo=%08lX", (u32)ret);
    if (R_SUCCEEDED(ret)) {
        ret = APT_PrepareToDoApplicationJump(0, fwdr, 0);
        logline("APT_PrepareToDoApplicationJump=%08lX", (u32)ret);
        if (R_SUCCEEDED(ret)) {
            u8 param[0x300];
            u8 hmac[0x20];
            memset(param, 0, sizeof(param));
            memset(hmac, 0, sizeof(hmac));
            logline("jumping");
            ret = APT_DoApplicationJump(param, sizeof(param), hmac);
            logline("APT_DoApplicationJump=%08lX (returned!)", (u32)ret);
        }
    }
    if (R_FAILED(ret)) {
        fatal("Failed to launch bootstrap.\n\nInstall bootstrap.cia from the\nYANBF release with FBI.", ret);
    }

    if (plog) fclose(plog);
    amExit();
    return 0;
}
