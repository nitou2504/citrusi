#include <3ds.h>
#include <cstring>
#include <cstdio>
#include "cwav.hpp"
#include "logger.hpp"
#include "helpers.hpp"
#include "settings.hpp"

static Logger cwavLogger("CWAV");

// ---- CWAV structures (from bannertool, MIT) ----------------------------

#define CWAV_VERSION 0x02010000
enum {
    CWAV_REF_SAMPLE_DATA = 0x1F00,
    CWAV_REF_INFO_BLOCK = 0x7000,
    CWAV_REF_DATA_BLOCK = 0x7001,
    CWAV_REF_CHANNEL_INFO = 0x7100
};
enum { CWAV_ENCODING_PCM8 = 0, CWAV_ENCODING_PCM16 };

typedef struct { u16 typeId; u16 padding; u32 offset; } CWAVReference;
typedef struct { CWAVReference ref; u32 size; } CWAVSizedReference;
typedef struct { u32 count; } CWAVReferenceTable;
typedef struct {
    char magic[4]; u16 endianness; u16 headerSize; u32 version; u32 fileSize;
    u16 numBlocks; u16 reserved; CWAVSizedReference infoBlock; CWAVSizedReference dataBlock;
} CWAVHeader;
typedef struct { char magic[4]; u32 size; } CWAVBlockHeader;
typedef struct {
    CWAVBlockHeader header; u8 encoding; u8 loop; u16 padding;
    u32 sampleRate; u32 loopStartFrame; u32 loopEndFrame; u32 reserved;
    CWAVReferenceTable channelInfos;
} CWAVInfoBlockHeader;
typedef struct { CWAVReference samples; CWAVReference adpcmInfo; u32 reserved; } CWAVChannelInfo;
typedef struct { CWAVBlockHeader header; } CWAVDataBlock;

// ---- WAV parsing --------------------------------------------------------

std::string wavToCwav(const std::string& wav) {
    if (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) || memcmp(wav.data()+8, "WAVE", 4)) {
        cwavLogger.info("not a RIFF WAV");
        return "";
    }
    u16 fmt = 0, channels = 0, bits = 0;
    u32 rate = 0;
    const char* pcm = nullptr;
    u32 pcmSize = 0;
    size_t pos = 12;
    while (pos + 8 <= wav.size()) {
        u32 sz; memcpy(&sz, wav.data()+pos+4, 4);
        if (!memcmp(wav.data()+pos, "fmt ", 4) && sz >= 16) {
            memcpy(&fmt, wav.data()+pos+8, 2);
            memcpy(&channels, wav.data()+pos+10, 2);
            memcpy(&rate, wav.data()+pos+12, 4);
            memcpy(&bits, wav.data()+pos+22, 2);
        } else if (!memcmp(wav.data()+pos, "data", 4)) {
            pcm = wav.data()+pos+8;
            pcmSize = sz;
            if (pos + 8 + pcmSize > wav.size()) pcmSize = wav.size() - pos - 8;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || !pcm || !pcmSize || !channels || (bits != 8 && bits != 16)) {
        cwavLogger.info("unsupported WAV (need PCM 8/16)");
        return "";
    }

    u32 bytesPerSample = bits / 8;
    u32 headerSize = (sizeof(CWAVHeader) + 0x1F) & ~0x1F;
    u32 infoSize = ((sizeof(CWAVInfoBlockHeader) +
                    (channels * (sizeof(CWAVReference) + sizeof(CWAVChannelInfo)))) + 0x1F) & ~0x1F;
    u32 dataPad = ((sizeof(CWAVDataBlock) + 0x1F) & ~0x1F) - sizeof(CWAVDataBlock);
    u32 dataSize = ((sizeof(CWAVDataBlock) + 0x1F) & ~0x1F) + pcmSize;
    std::string out(headerSize + infoSize + dataSize, '\0');

    CWAVHeader* header = (CWAVHeader*)&out[0];
    memcpy(header->magic, "CWAV", 4);
    header->endianness = 0xFEFF;
    header->headerSize = (u16)headerSize;
    header->version = CWAV_VERSION;
    header->fileSize = out.size();
    header->numBlocks = 2;
    header->infoBlock.ref.typeId = CWAV_REF_INFO_BLOCK;
    header->infoBlock.ref.offset = headerSize;
    header->infoBlock.size = infoSize;
    header->dataBlock.ref.typeId = CWAV_REF_DATA_BLOCK;
    header->dataBlock.ref.offset = headerSize + infoSize;
    header->dataBlock.size = dataSize;

    CWAVInfoBlockHeader* info = (CWAVInfoBlockHeader*)&out[headerSize];
    memcpy(info->header.magic, "INFO", 4);
    info->header.size = infoSize;
    info->encoding = (bits == 16) ? CWAV_ENCODING_PCM16 : CWAV_ENCODING_PCM8;
    info->loop = 0;
    info->sampleRate = rate;
    info->loopStartFrame = 0;
    info->loopEndFrame = pcmSize / channels / bytesPerSample;
    info->channelInfos.count = channels;

    u32 samplesPerChan = pcmSize / channels;
    for (u32 c = 0; c < channels; c++) {
        CWAVReference* ref = (CWAVReference*)&out[headerSize + sizeof(CWAVInfoBlockHeader) + c*sizeof(CWAVReference)];
        ref->typeId = CWAV_REF_CHANNEL_INFO;
        ref->offset = sizeof(CWAVReferenceTable) + (channels * sizeof(CWAVReference)) + (c * sizeof(CWAVChannelInfo));
        CWAVChannelInfo* ci = (CWAVChannelInfo*)&out[headerSize + sizeof(CWAVInfoBlockHeader) +
                              (channels * sizeof(CWAVReference)) + (c * sizeof(CWAVChannelInfo))];
        ci->samples.typeId = CWAV_REF_SAMPLE_DATA;
        ci->samples.offset = dataPad + (c * samplesPerChan);
        ci->adpcmInfo.typeId = 0;
        ci->adpcmInfo.offset = 0xFFFFFFFF;
    }

    CWAVDataBlock* data = (CWAVDataBlock*)&out[headerSize + infoSize];
    memcpy(data->header.magic, "DATA", 4);
    data->header.size = dataSize;
    // deinterleave into per-channel contiguous blocks
    char* dataStart = &out[headerSize + infoSize + sizeof(CWAVDataBlock)];
    for (u32 i = 0; i < pcmSize; i += channels * bytesPerSample)
        for (u32 c = 0; c < channels; c++)
            memcpy(dataStart + dataPad + c*samplesPerChan + (i / channels),
                   pcm + i + c*bytesPerSample, bytesPerSample);
    return out;
}

std::string fetchGameSound(RommClient& client, const std::string& romPath) {
    FILE* f = fopen(romPath.c_str(), "rb");
    if (!f) return "";
    char gc[5] = {0};
    fseek(f, 0x0C, SEEK_SET);
    fread(gc, 1, 4, f);
    fclose(f);
    for (int i = 0; i < 4; i++)
        if (gc[i] < 0x20 || gc[i] > 0x7E) return "";

    std::string wav;
    std::string gc4(gc, 4), gc3(gc, 3);
    // local mirror on SD first
    std::string assetsDir = FORWARDER_DIR + std::string("/assets/");
    for (const std::string& c : {gc4, gc3}) {
        std::string p = assetsDir + c + "/" + c + ".wav";
        if (fileExists(p)) {
            wav = readEntireFile(p);
            if (wav.size() >= 44) {
                cwavLogger.info("sound: SD assets " + c);
                return wavToCwav(wav);
            }
        }
    }
    std::string base = "https://raw.githubusercontent.com/YANBForwarder/assets/main/assets/";
    if (!client.fetchUrl(base + gc4 + "/" + gc4 + ".wav", wav) || wav.size() < 44) {
        if (!client.fetchUrl(base + gc3 + "/" + gc3 + ".wav", wav) || wav.size() < 44) {
            cwavLogger.info("no YANBF sound for " + gc4 + " (" + client.lastError + ")");
            return "";
        }
    }
    cwavLogger.info("sound: YANBF assets github, " + std::to_string(wav.size()) + " bytes");
    return wavToCwav(wav);
}
