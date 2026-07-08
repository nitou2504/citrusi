/*
romm3ds CTR forwarder payload
Based on YANBF forwarder (MIT) Copyright (C) 2022-present lifehackerhansol

Reads the rom path from sd:/3ds/forwarder/ctr/<TITLEID>.txt (written by the
romm3ds generator at install time), stages it for NTR_Forwarder, then chains
to the YANBF bootstrap TWL title (FWDR).
*/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <3ds.h>

static void fatal(const char* msg) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("%s\n\nPress START to exit.", msg);
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
    amInit();

    u64 tid = 0;
    APT_GetProgramID(&tid);

    char cfgPath[64];
    snprintf(cfgPath, sizeof(cfgPath), "sdmc:/3ds/forwarder/ctr/%016llX.txt", tid);

    char line[512] = {0};
    FILE* f = fopen(cfgPath, "r");
    if (!f) {
        fatal("Missing forwarder path file.\nReinstall this forwarder with romm3ds.");
        amExit();
        return 1;
    }
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    size_t len = strlen(line);
    while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
    if (!len) {
        fatal("Empty forwarder path file.\nReinstall this forwarder with romm3ds.");
        amExit();
        return 1;
    }

    mkdir("sdmc:/_nds", 0777);
    mkdir("sdmc:/_nds/ntr-forwarder", 0777);
    FILE* path = fopen("sdmc:/_nds/ntr-forwarder/path.txt", "w");
    if (path) {
        fputs(line, path);
        fclose(path);
    }

    Result ret = 0;
    AM_TitleEntry bootstrap;
    u64 fwdr = 0x0004800546574452ULL;
    if (ret = AM_GetTitleInfo(MEDIATYPE_NAND, 1, &fwdr, &bootstrap), R_SUCCEEDED(ret)) {
        if (ret = APT_PrepareToDoApplicationJump(0, fwdr, 0), R_SUCCEEDED(ret)) {
            u8 param[0x300];
            u8 hmac[0x20];
            ret = APT_DoApplicationJump(param, sizeof(param), hmac);
        }
    }
    if (R_FAILED(ret)) {
        fatal("Failed to launch bootstrap.\n\nInstall bootstrap.cia from the\nYANBF release with FBI.");
    }

    amExit();
    return 0;
}
