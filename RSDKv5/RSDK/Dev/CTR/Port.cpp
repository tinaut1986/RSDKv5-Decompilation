#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/Port.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"
#include "RSDK/Dev/CTR/Updater.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

using namespace RSDK;

bool Port::frameSkip  = true;
bool Port::lock30     = false;
bool Port::gpu        = true;
int Port::fps         = 0;
int Port::hud         = 1;
int Port::tab         = 0;
bool Port::autoUpdate = true;
bool Port::betas      = false;
int Port::theme       = 0;

// One file for the port, the dev tools' switches in it too (DevTools::SaveSwitches). The game's own Settings.ini
// (language, video, sound) is the engine's and stays apart; dev-settings.ini is where these lived before: read once.
static const char *kSettings    = "sdmc:/3ds/Sonic Mania 3DS/port-settings.ini";
static const char *kDevSettings = "sdmc:/3ds/Sonic Mania 3DS/dev-settings.ini";

static bool ReadSettings(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char key[24];
    int v;
    while (fscanf(f, " %23[^=]=%d", key, &v) == 2) {
        if (!strcmp(key, "frameskip"))
            Port::frameSkip = v != 0;
        else if (!strcmp(key, "lock30"))
            Port::lock30 = v != 0;
        else if (!strcmp(key, "gpumode"))
            Port::gpu = v != 0;
        else if (!strcmp(key, "fps"))
            Port::fps = v < 0 || v > 4 ? 0 : v;
        else if (!strcmp(key, "hud"))
            Port::hud = v < 0 || v > 2 ? 1 : v;
        else if (!strcmp(key, "tab"))
            Port::tab = v < 0 || v > 7 ? 0 : v;
        else if (!strcmp(key, "autoupdate"))
            Port::autoUpdate = v != 0;
        else if (!strcmp(key, "betas"))
            Port::betas = v != 0;
        else if (!strcmp(key, "theme"))
            Port::theme = v < 0 || v > 15 ? 0 : v;
    }
    fclose(f);
    return true;
}

void Port::Load()
{
    if (!ReadSettings(kSettings))
        ReadSettings(kDevSettings);
}

void Port::Save()
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/Sonic Mania 3DS", 0777);
    FILE *f = fopen(kSettings, "w");
    if (!f)
        return;
    fprintf(f, "frameskip=%d\nlock30=%d\ngpumode=%d\nfps=%d\nhud=%d\ntab=%d\nautoupdate=%d\nbetas=%d\ntheme=%d\n", (int)frameSkip,
            (int)lock30, (int)GpuMode(), fps, hud, tab, (int)autoUpdate, (int)betas, theme);
#if MANIA_DEV_TOOLS
    DevTools::SaveSwitches(f);
#endif
    fclose(f);
}

bool Port::GpuMode()
{
#if MANIA_DEV_TOOLS
    return DevTools::GpuMode();
#else
    return gpu;
#endif
}

void Port::SetGpuMode(bool on)
{
#if MANIA_DEV_TOOLS
    DevTools::SetGpuMode(on);
#endif
    gpu = on;
}

bool Port::HideHud(uint16 classID)
{
    if (hud == 0 || (hud == 1 && tab != 0)) // tab 0: GAME, which shows what the HUD shows
        return false;
    // The HUD's stage class ID, looked up again when the stage's class list changes.
    static uint32 hudHash[4];
    static bool hashed;
    static const void *seenList;
    static int32 seenCount = -1;
    static int hudID       = -1;
    if (!hashed) {
        GEN_HASH_MD5("HUD", hudHash);
        hashed = true;
    }
    if (seenList != (const void *)stageObjectIDs || seenCount != sceneInfo.classCount || sceneInfo.state == ENGINESTATE_LOAD) {
        seenList  = stageObjectIDs;
        seenCount = sceneInfo.classCount;
        hudID     = -1;
        for (int32 i = 0; i < sceneInfo.classCount; ++i)
            if (HASH_MATCH_MD5(objectClassList[stageObjectIDs[i]].hash, hudHash))
                hudID = i;
    }
    return hudID >= 0 && classID == hudID;
}

// ---------------------------------------------------------------- network (from ../zelda-tmc-3ds net_3ds.c)

// The sockets service's buffer, never given back: 1 MB on the New 3DS, 256 KB on the Old 3DS / 2DS, as in
// ../legend-of-doom-3ds (net_3ds.c), whose 2DS had no megabyte to spare either; here a megabyte failed on the 2DS after
// the engine's pools and the updater's libraries ("NO NETWORK SERVICE"). Transfers are slower with less.
static LightLock sNetLock = 1; // LightLock's unlocked value
static int sNetRefs;
static void *sSocBuffer;
static bool sAcReady;

bool Port::NetAcquire()
{
    LightLock_Lock(&sNetLock);
    bool ok = true;
    if (!sSocBuffer) {
        const Result ac = acInit();
        sAcReady        = R_SUCCEEDED(ac);
        bool isNew3DS = true;
        APT_CheckNew3DS(&isNew3DS);
        const u32 size = isNew3DS ? 1024 * 1024 : 256 * 1024;
        sSocBuffer     = memalign(0x1000, size);
        const Result soc = sAcReady && sSocBuffer ? socInit((u32 *)sSocBuffer, size) : -1;
        DEV_LOG("network: acInit %08lx, buffer %s (%lu KB), socInit %08lx", (unsigned long)ac, sSocBuffer ? "ok" : "NULL",
                (unsigned long)(size >> 10), (unsigned long)soc);
        if (!sAcReady || !sSocBuffer || R_FAILED(soc)) {
            free(sSocBuffer);
            sSocBuffer = NULL;
            if (sAcReady)
                acExit();
            sAcReady = false;
            ok       = false;
        }
    }
    if (ok)
        ++sNetRefs;
    LightLock_Unlock(&sNetLock);
    return ok;
}

void Port::NetRelease()
{
    // The service stays up once started (its buffer with it): in ../zelda-tmc-3ds socExit froze the game when the FTP
    // server stopped with a client still connected. The kernel closes the sessions when the process ends.
    LightLock_Lock(&sNetLock);
    if (sNetRefs > 0)
        --sNetRefs;
    LightLock_Unlock(&sNetLock);
}

bool Port::WifiConnected()
{
    u32 wifi = 0;
    if (sAcReady && R_SUCCEEDED(ACU_GetWifiStatus(&wifi)))
        return wifi != 0;
    return osGetWifiStrength() > 0; // without AC, as ../legend-of-doom-3ds
}

// ---------------------------------------------------------------- frame counter, restart

static u64 sSecondStart;
static unsigned sFrames;
static float sFps;

enum { RELAUNCH_MS = 10000 };
static u64 sRelaunchAt; // osGetTime() the countdown ends at (0: none)

float Port::Fps() { return sFps; }

void Port::RequestRelaunch() { sRelaunchAt = osGetTime() + RELAUNCH_MS; }

unsigned Port::RelaunchSecondsLeft(float *progress)
{
    const u64 at = sRelaunchAt;
    if (!at)
        return 0;
    const u64 now = osGetTime();
    if (progress)
        *progress = now >= at ? 1.0f : 1.0f - (float)(at - now) / (float)RELAUNCH_MS;
    return now >= at ? 1 : (unsigned)((at - now + 999) / 1000);
}

bool Port::FrameEnd()
{
    const u64 now = svcGetSystemTick();
    if (!sSecondStart)
        sSecondStart = now;
    ++sFrames;
    if (now - sSecondStart >= SYSCLOCK_ARM11) {
        sFps         = (float)(sFrames * (double)SYSCLOCK_ARM11 / (double)(now - sSecondStart));
        sFrames      = 0;
        sSecondStart = now;
    }
#if !MANIA_DEV_TOOLS
    BottomUi::Update(); // the dev tools call it from their own frame end
#endif
    if (Updater_ShouldClose()) // a 3DSX update: back to the Homebrew Launcher, which starts the new file
        return true;
    if (sRelaunchAt && osGetTime() >= sRelaunchAt) {
        // aptExit() starts this title again once the process has exited (as HOME -> close).
        aptSetChainloaderToSelf();
        return true;
    }
    return false;
}

#endif
