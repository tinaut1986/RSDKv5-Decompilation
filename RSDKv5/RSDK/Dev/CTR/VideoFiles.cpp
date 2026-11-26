#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/VideoFiles.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <sys/stat.h>
#include <stdio.h>

using namespace RSDK;

namespace
{

// Data/Video/ files the game plays: TitleSetup (the intro) and the Ending scenes' UIVideo objects
const char *const kVideos[] = { "Mania.ogv",     "BadSonic.ogv", "BadSonic2.ogv", "BadTails.ogv", "BadKnux.ogv",
                                "BadMighty.ogv", "BadRay.ogv",   "BadEnd.ogv",    "MREnd.ogv",    "GoodEnd.ogv" };
const int kVideoCount = (int)(sizeof(kVideos) / sizeof(kVideos[0]));

int sConverted = -1;
u64 sCountedAt;

} // namespace

int VideoFiles::Count() { return kVideoCount; }

int VideoFiles::Converted()
{
    const u64 now = svcGetSystemTick();
    if (sConverted < 0 || now - sCountedAt > 5ULL * SYSCLOCK_ARM11) { // a file may come from a PC or over FTP
        int n = 0;
        for (int i = 0; i < kVideoCount; ++i) {
            char path[0x100];
            snprintf(path, sizeof(path), "%sVideo/%s", SKU::userFileDir, kVideos[i]);
            struct stat st;
            n += stat(path, &st) == 0 && st.st_size > 0;
        }
        sConverted = n;
        sCountedAt = now;
    }
    return sConverted;
}

#endif // RETRO_PLATFORM == RETRO_3DS
