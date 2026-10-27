#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/DevTools.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"
#include "RSDK/Dev/CTR/GameInfo.h"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"
#include "RSDK/Graphics/CTR/GpuRender.hpp"
#include "RSDK/Graphics/CTR/C3DPresent.hpp"

// Production builds (MANIA_DEV_TOOLS=0) keep only the FTP server, in its install-only mode (DevFtp, DevTools.hpp).
#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace RSDK;

// The audio device's counters (CTRAudioDevice.cpp), for SITE C3D and the recorded runs.
extern volatile u64 ctrAudioMixTicks, ctrAudioDecodeTicks;
extern volatile u32 ctrAudioUnderruns, ctrMusicStarved;
extern int ctrAudioCore;
extern u32 ctrAudioCore1Limit;
extern Thread audioThreadHandle;

// Own title ID: 0004000000 + UniqueId 0x05A230 (3ds/SonicMania.rsf) + variation 00. Installs of any other CIA are refused.
static const u64 TITLE_ID = 0x0004000005A23000ULL;

enum {
    FTP_PORT      = 5000,
    FTP_PATH_MAX  = 512,
    FTP_LINE_MAX  = 600,
    FTP_IO_SIZE   = 64 * 1024,
    FTP_NAP_MS    = 10,
    FTP_CHUNK     = 8 * 1024,
    FTP_ACCEPT_MS = 15000,
    FTP_IDLE_MS   = 10 * 60 * 1000,
    FTP_PASV_FIRST = FTP_PORT + 1,
    FTP_PASV_COUNT = 64,
    CIA_IO_SIZE   = 32 * 1024,
    RELAUNCH_MS   = 10000,
};

// ---------------------------------------------------------------- state

static Thread sThread;
static volatile bool sRun;
static int sListen = -1;
static uint32_t sIp;
static char *sIo;
static char sStatus[48] = "STOPPED";

// What the bottom screen says about an install in progress.
static const char *volatile sNotice;
static volatile int sPhase; // 1 receiving, 2 installing, 3 restart countdown
static volatile unsigned long long sAllo, sRecvDone, sRecvTotal, sCiaDone, sCiaTotal;
static volatile unsigned long long sRelaunchAt; // osGetTime() the countdown ends at

// Install-only mode (production builds, started by dev-ftp.txt): only the update folder is reachable and the remote
// control is off but for SITE INSTALL. A CANCEL on the bottom screen stops it for the rest of the session.
static const bool sRestricted = !MANIA_DEV_TOOLS;
static bool sCancelled;
#define FTP_RESTRICTED_ROOT "/3ds/Sonic Mania 3DS/update"
static bool Allowed(const char *vpath)
{
    if (!sRestricted)
        return true;
    const size_t n = sizeof(FTP_RESTRICTED_ROOT) - 1;
    return !strncasecmp(vpath, FTP_RESTRICTED_ROOT, n) && (vpath[n] == 0 || vpath[n] == '/');
}

#if MANIA_DEV_TOOLS
static float sLastFps, sLastFrameMs, sLastStageMs[DevTools::STAGE_COUNT];
static unsigned sLastSkipped;

// Frames shown by renderer (NoteFrame): this second, the last full second, since the mode switch, and since the
// last log line; plus why the CPU drew them, counted per reason (the names are static strings: compared as pointers).
static unsigned sSecGpu, sSecCpu, sLastSecGpu, sLastSecCpu, sTotGpu, sTotCpu, sLogGpu, sLogCpu;
static bool sLastFrameGpu;
static const char *sLastWhy;
struct WhyCount {
    const char *why;
    unsigned count;
};
enum { WHY_SLOTS = 16 };
static WhyCount sWhy[WHY_SLOTS], sLogWhy[WHY_SLOTS];
static volatile int sPendingGpuMode = -1; // SITE GPU, applied by the main thread

// Requests from the FTP thread, applied by the main thread in FrameEnd.
static volatile int sPendingCategory = -1, sPendingScene = -1;
static char sOrderCur[400], sOrderLast[400];
static volatile bool sGoto; // SITE GOTO: move the player at the end of the frame
static volatile bool sUfoTime; // SITE SET ufotime 1: the Special Stage's countdown never runs out (not saved)
extern bool gifCheck; // Sprite.cpp: SITE SET gifcheck 1 (not saved)
static volatile bool sModelCheck; // SITE SET modelcheck 1: the 3D models' VRAM checked every 2 s (not saved)
static s32 sMainPrio; // the main thread's priority (SITE C3D)
static volatile bool sUfoNoFall; // SITE SET ufonofall 1: the Special Stage's holes are ground (not saved)

// GetTile asks: the Special Stage's player falls where the playfield has no tile (UFO_Player_HandleSpeedUp); with
// the switch on such a tile reads as tile 0, plain ground, in a Special Stage only. Keep it the same when recording a
// run and playing it.
bool RSDK::DevNoFall()
{
    return sUfoNoFall && sceneInfo.listData && !strncmp(sceneInfo.listData[sceneInfo.listPos].folder, "UFO", 3);
}
// The Special Stage's static variables as the game (Plus, GAME_VERSION 1.06) lays them out, up to what is used here.
struct UfoSetupStatics {
    uint16 classID;
    uint8 active;
    void (*deformCB)();
    int32 timedOut, encoreStage, machLevel, scoreBonus, machPoints, rings, timer, ringFrame, ringPan, spherePan;
};
static UfoSetupStatics *UfoSetup()
{
    static uint32 hash[4];
    static bool hashed;
    if (!hashed) {
        GEN_HASH_MD5("UFO_Setup", hash);
        hashed = true;
    }
    for (int32 i = 0; i < sceneInfo.classCount; ++i) {
        ObjectClass &oc = objectClassList[stageObjectIDs[i]];
        if (HASH_MATCH_MD5(oc.hash, hash) && oc.staticVars && *oc.staticVars)
            return (UfoSetupStatics *)*oc.staticVars;
    }
    return NULL;
}
static int32 sGotoX, sGotoY;
// SITE HIDE <ObjectName>: one class not drawn (to see what it covers); SITE HIDE alone shows it again. Not saved.
static uint32 sHideHash[4];
static volatile bool sHideSet;
// Recorded runs: keys per logic pass from the end of a scene load (12 button bits, the circle pad's 4 in 12-15).
enum { RUN_MAX = 60 * 60 * 5 }; // five minutes
enum RunMode { RUN_OFF, RUN_ARM_REC, RUN_REC, RUN_ARM_PLAY, RUN_PLAY };
static uint16 sRunKeys[RUN_MAX];
static unsigned sRunCount, sRunPos;
static volatile int sRunMode;
static uint32 sRunContent; // GpuFrame::contentVersion when armed: a new value means the scene has been loaded
static int sRunCategory = -1, sRunListPos = -1;
static unsigned sRunFrames0;
static unsigned sRunStarved0; // music buffers the decoder had not filled, at the run's start
static u64 sRunTick0;
static char sRunResult[96] = "-";
static bool sRunFreezeAtEnd; // RUNPLAY N F: freeze the objects on the last pass (to compare two playbacks' pictures)
static const char *kRunPath = "sdmc:/3ds/Sonic Mania 3DS/debug/run.bin";
static unsigned sRunRecLimit; // RUNREC N: stop by itself after N passes, frozen (to compare with RUNPLAY N F)
// A run starts from a full load of its scene (forceHardReset), in the recording and in the playback: a reload of the
// scene being played keeps the objects' static variables (the Special Stage's frame counter, the camera...), a load
// from another scene makes them anew, and the playback drifted off the route. (Copying the statics instead also
// copied what the load assigns, sprite sheet and sound IDs, storage pointers: garbage on the 2DS.)
static volatile bool sPendingHard;

static void RunSave()
{
    FILE *f = fopen(kRunPath, "wb");
    if (!f)
        return;
    const int32 hdr[3] = { sRunCategory, sRunListPos, (int32)sRunCount };
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(sRunKeys, 2, sRunCount, f);
    fclose(f);
}
static volatile int sShotRequest; // 1 top screen, 2 bottom screen, 3 top screen's right eye
#endif // MANIA_DEV_TOOLS

struct Session {
    int ctrl;
    int pasv;
    char cwd[FTP_PATH_MAX];
    char rnfr[FTP_PATH_MAX];
    char in[FTP_LINE_MAX * 2];
    unsigned inUsed;
};
static unsigned sNextPasv;

// ---------------------------------------------------------------- CIA install

static bool EnoughSpace(u64 required)
{
    FS_ArchiveResource r;
    return R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (u64)r.freeClusters * r.clusterSize >= required;
}

// `sdPath` is an SD path without "sdmc:". Same steps as the owner's other ports: AM overwrite
// install, falling back to removing the installed title when AM refuses a lower version.
static bool InstallCia(const char *sdPath, char *error, unsigned errorCap)
{
    Result rc = amInit();
    if (R_FAILED(rc)) {
        snprintf(error, errorCap, "AM unavailable (%08lx)", (unsigned long)rc);
        return false;
    }
    Handle input = 0, output = 0;
    bool started = false, ok = false, downgraded = false;
    u64 size = 0, required = 0;
    AM_TitleInfo info;
    // A static buffer: in a big stage the heap had no 128 KB left and the install failed ("out of memory").
    static unsigned char data[CIA_IO_SIZE];
    snprintf(error, errorCap, "cannot open the CIA");
    rc = FSUSER_OpenFileDirectly(&input, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, sdPath), FS_OPEN_READ, 0);
    snprintf(error, errorCap, "cannot open %s", sdPath);
    if (R_FAILED(rc) || R_FAILED(FSFILE_GetSize(input, &size)))
        goto done;
    rc = AM_GetCiaFileInfo(MEDIATYPE_SD, &info, input);
    snprintf(error, errorCap, "not a CIA of this game (title %016llx)", R_SUCCEEDED(rc) ? (unsigned long long)info.titleID : 0ull);
    if (R_FAILED(rc) || info.titleID != TITLE_ID)
        goto done;
    rc = AM_GetCiaRequiredSpace(&required, MEDIATYPE_SD, input);
    snprintf(error, errorCap, "not enough SD space");
    if (R_FAILED(rc) || !EnoughSpace(required + 4 * 1024 * 1024ull))
        goto done;
    rc = AM_StartCiaInstallOverwrite(&output, MEDIATYPE_SD);
    snprintf(error, errorCap, "install start failed (%08lx)", (unsigned long)rc);
    if (R_FAILED(rc))
        goto done;
    started   = true;
    sCiaDone  = 0;
    sCiaTotal = size;
    for (u64 offset = 0; offset < size;) {
        u32 read = 0, wrote = 0, want = size - offset > CIA_IO_SIZE ? CIA_IO_SIZE : (u32)(size - offset);
        rc = FSFILE_Read(input, &read, offset, data, want);
        snprintf(error, errorCap, "SD read failed (%08lx)", (unsigned long)rc);
        if (R_FAILED(rc) || read != want)
            goto done;
        rc = FSFILE_Write(output, &wrote, offset, data, read, 0);
        if (R_FAILED(rc) && offset == 0 && !downgraded) {
            // AM refuses a lower title version at the first write: remove the installed title and
            // install this one (saves and settings live under sdmc:/3ds/, not in the title).
            AM_CancelCIAInstall(output);
            started    = false;
            downgraded = true;
            rc         = AM_DeleteTitle(MEDIATYPE_SD, TITLE_ID);
            snprintf(error, errorCap, "removing the installed title failed (%08lx)", (unsigned long)rc);
            if (R_FAILED(rc))
                goto done;
            rc = AM_StartCiaInstall(MEDIATYPE_SD, &output);
            snprintf(error, errorCap, "install restart failed (%08lx)", (unsigned long)rc);
            if (R_FAILED(rc))
                goto done;
            started = true;
            continue;
        }
        snprintf(error, errorCap, "install write failed (%08lx)", (unsigned long)rc);
        if (R_FAILED(rc) || wrote != read)
            goto done;
        offset += read;
        sCiaDone = offset;
    }
    rc      = AM_FinishCiaInstall(output);
    started = false;
    ok      = R_SUCCEEDED(rc);
    snprintf(error, errorCap, ok ? "" : "install finish failed (%08lx)", (unsigned long)rc);
done:
    if (started)
        AM_CancelCIAInstall(output);
    if (input)
        FSFILE_Close(input);
    amExit();
    return ok;
}

// ---------------------------------------------------------------- sockets

static void CloseSocket(int *fd)
{
    if (*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

// libctru sends every socket call through one SOC session, so a blocking poll() would hold up
// every other thread's network call: ask without waiting and sleep between the asks.
static bool WaitFd(int fd, short events, int timeoutMs)
{
    int waited = 0;
    while (sRun) {
        struct pollfd p;
        p.fd      = fd;
        p.events  = events;
        p.revents = 0;
        int r     = poll(&p, 1, 0);
        if (r > 0)
            return (p.revents & (events | POLLHUP | POLLERR)) != 0;
        if (r < 0)
            return false;
        svcSleepThread((s64)FTP_NAP_MS * 1000000);
        waited += FTP_NAP_MS;
        if (timeoutMs > 0 && waited >= timeoutMs)
            return false;
    }
    return false;
}

static bool SendAll(int fd, const char *data, size_t n)
{
    while (n > 0) {
        if (!WaitFd(fd, POLLOUT, 30000))
            return false;
        ssize_t w = send(fd, data, n > FTP_CHUNK ? FTP_CHUNK : n, 0);
        if (w <= 0)
            return false;
        data += w;
        n -= (size_t)w;
    }
    return true;
}

static bool Reply(Session *s, const char *fmt, ...)
{
    char line[FTP_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (n < 0)
        return false;
    if (n > (int)sizeof(line) - 3)
        n = (int)sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    return SendAll(s->ctrl, line, (size_t)n);
}

static bool ReadLine(Session *s, char *out, unsigned cap)
{
    for (;;) {
        for (unsigned i = 0; i < s->inUsed; i++) {
            if (s->in[i] == '\n') {
                unsigned n = i;
                if (n > 0 && s->in[n - 1] == '\r')
                    n--;
                if (n >= cap)
                    n = cap - 1;
                memcpy(out, s->in, n);
                out[n] = 0;
                memmove(s->in, s->in + i + 1, s->inUsed - i - 1);
                s->inUsed -= i + 1;
                return true;
            }
        }
        if (s->inUsed >= sizeof(s->in) - 1)
            s->inUsed = 0;
        if (!WaitFd(s->ctrl, POLLIN, FTP_IDLE_MS))
            return false;
        ssize_t r = recv(s->ctrl, s->in + s->inUsed, sizeof(s->in) - 1 - s->inUsed, 0);
        if (r <= 0)
            return false;
        s->inUsed += (unsigned)r;
    }
}

// Next "/"-separated part of *cursor (strtok_r is not in newlib's strict mode).
static char *Token(char **cursor)
{
    char *p = *cursor;
    while (*p == '/')
        ++p;
    if (!*p)
        return NULL;
    char *start = p;
    while (*p && *p != '/')
        ++p;
    if (*p)
        *p++ = 0;
    *cursor = p;
    return start;
}

// "/" is the root of the SD card.
static void ResolvePath(const Session *s, const char *arg, char *out, unsigned cap)
{
    char joined[FTP_PATH_MAX * 2];
    if (!arg || !arg[0])
        snprintf(joined, sizeof(joined), "%s", s->cwd);
    else if (arg[0] == '/')
        snprintf(joined, sizeof(joined), "%s", arg);
    else
        snprintf(joined, sizeof(joined), "%s/%s", s->cwd, arg);
    char parts[64][128];
    int count  = 0;
    char *save = joined;
    for (char *p; (p = Token(&save)) != NULL;) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            if (count > 0)
                count--;
            continue;
        }
        if (count < 64)
            snprintf(parts[count++], sizeof(parts[0]), "%s", p);
    }
    unsigned n = 0;
    out[0]     = 0;
    for (int i = 0; i < count && n + 1 < cap; i++)
        n += (unsigned)snprintf(out + n, cap - n, "/%s", parts[i]);
    if (n == 0)
        snprintf(out, cap, "/");
}

static void RealPath(const char *virtualPath, char *out, unsigned cap) { snprintf(out, cap, "sdmc:%s", virtualPath); }

// ---------------------------------------------------------------- data connection

static void ClosePasv(Session *s) { CloseSocket(&s->pasv); }

static int OpenPasv(Session *s)
{
    ClosePasv(s);
    for (unsigned attempt = 0; attempt < FTP_PASV_COUNT; attempt++) {
        const unsigned port = FTP_PASV_FIRST + (sNextPasv++ % FTP_PASV_COUNT);
        int fd              = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (fd < 0)
            return -1;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(fd, 1) == 0) {
            s->pasv = fd;
            return (int)port;
        }
        close(fd);
    }
    return -1;
}

static int AcceptData(Session *s)
{
    if (s->pasv < 0)
        return -1;
    int fd = -1;
    if (WaitFd(s->pasv, POLLIN, FTP_ACCEPT_MS)) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        fd            = accept(s->pasv, (struct sockaddr *)&peer, &len);
    }
    ClosePasv(s);
    return fd;
}

// ---------------------------------------------------------------- commands

static void FormatListLine(char *out, unsigned cap, const char *name, const struct stat *st, bool mlsd)
{
    const bool dir = S_ISDIR(st->st_mode);
    if (mlsd) {
        snprintf(out, cap, "type=%s;size=%llu; %s\r\n", dir ? "dir" : "file", (unsigned long long)st->st_size, name);
        return;
    }
    snprintf(out, cap, "%s 1 3ds 3ds %12llu Jan  1 00:00 %s\r\n", dir ? "drwxrwxrwx" : "-rw-rw-rw-", (unsigned long long)st->st_size, name);
}

// The listing is read with FSDIR_Read, which gives each entry's size with its name: a stat() per entry opened the file
// and walked the directory again, and the debug folder's 400-odd captures and perf files took a minute to list (the
// server takes one session at a time: STATE, shots and tools/capture.sh all timed out behind it). The lines go out in
// FTP_IO_SIZE blocks, not one send a line.
static void CmdList(Session *s, const char *arg, int kind /* 0 LIST, 1 NLST, 2 MLSD */)
{
    if (arg && arg[0] == '-') {
        const char *space = strchr(arg, ' ');
        arg               = space ? space + 1 : "";
    }
    char vpath[FTP_PATH_MAX];
    ResolvePath(s, arg, vpath, sizeof(vpath));
    FS_Archive sdmc = 0;
    Handle dir      = 0;
    const bool open = Allowed(vpath) && R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
    if (!open || R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, vpath)))) {
        if (open)
            FSUSER_CloseArchive(sdmc);
        ClosePasv(s);
        Reply(s, "550 No such directory.");
        return;
    }
    Reply(s, "150 Listing.");
    int data = AcceptData(s);
    if (data < 0) {
        FSDIR_Close(dir);
        FSUSER_CloseArchive(sdmc);
        Reply(s, "425 No data connection.");
        return;
    }
    static FS_DirectoryEntry entries[32];
    bool ok     = true;
    size_t used = 0;
    u32 count   = 0;
    while (ok && R_SUCCEEDED(FSDIR_Read(dir, &count, 32, entries)) && count > 0) {
        for (u32 i = 0; i < count && ok; ++i) {
            char name[FTP_PATH_MAX];
            const ssize_t n = utf16_to_utf8((uint8_t *)name, entries[i].name, sizeof(name) - 1);
            name[n > 0 ? n : 0] = 0;
            char line[FTP_PATH_MAX + 96];
            if (kind == 1)
                snprintf(line, sizeof(line), "%s\r\n", name);
            else {
                struct stat st;
                memset(&st, 0, sizeof(st));
                st.st_mode = (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG;
                st.st_size = (off_t)entries[i].fileSize;
                FormatListLine(line, sizeof(line), name, &st, kind == 2);
            }
            const size_t len = strlen(line);
            if (used + len > FTP_IO_SIZE) {
                ok   = SendAll(data, sIo, used);
                used = 0;
            }
            memcpy(sIo + used, line, len);
            used += len;
        }
    }
    if (ok && used)
        ok = SendAll(data, sIo, used);
    FSDIR_Close(dir);
    FSUSER_CloseArchive(sdmc);
    CloseSocket(&data);
    Reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void CmdRetr(Session *s, const char *arg)
{
    char vpath[FTP_PATH_MAX], rpath[FTP_PATH_MAX + 8];
    ResolvePath(s, arg, vpath, sizeof(vpath));
    RealPath(vpath, rpath, sizeof(rpath));
    FILE *f = Allowed(vpath) ? fopen(rpath, "rb") : NULL;
    if (!f) {
        ClosePasv(s);
        Reply(s, "550 Cannot open the file.");
        return;
    }
    Reply(s, "150 Sending.");
    int data = AcceptData(s);
    if (data < 0) {
        fclose(f);
        Reply(s, "425 No data connection.");
        return;
    }
    bool ok = true;
    size_t n;
    while (ok && (n = fread(sIo, 1, FTP_IO_SIZE, f)) > 0)
        ok = SendAll(data, sIo, n);
    fclose(f);
    CloseSocket(&data);
    Reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void CmdStor(Session *s, const char *arg, bool append)
{
    char vpath[FTP_PATH_MAX], rpath[FTP_PATH_MAX + 8];
    ResolvePath(s, arg, vpath, sizeof(vpath));
    RealPath(vpath, rpath, sizeof(rpath));
    FILE *f = Allowed(vpath) ? fopen(rpath, append ? "ab" : "wb") : NULL;
    if (!f) {
        ClosePasv(s);
        Reply(s, "553 Cannot create the file.");
        return;
    }
    const size_t len = strlen(vpath);
    const bool cia   = len > 4 && strcasecmp(vpath + len - 4, ".cia") == 0;
    if (cia) {
        sNotice   = "RECEIVING NEW BUILD";
        sRecvDone = 0, sRecvTotal = sAllo;
        sPhase = 1;
    }
    sAllo = 0;
    Reply(s, "150 Receiving.");
    int data = AcceptData(s);
    if (data < 0) {
        fclose(f);
        if (cia)
            sNotice = NULL, sPhase = 0;
        Reply(s, "425 No data connection.");
        return;
    }
    bool ok = true;
    for (;;) {
        if (!WaitFd(data, POLLIN, 30000)) {
            ok = false;
            break;
        }
        ssize_t r = recv(data, sIo, FTP_IO_SIZE, 0);
        if (r == 0)
            break;
        if (r < 0 || fwrite(sIo, 1, (size_t)r, f) != (size_t)r) {
            ok = false;
            break;
        }
        sRecvDone += (unsigned long long)r;
    }
    if (fclose(f) != 0)
        ok = false;
    CloseSocket(&data);
    if (cia)
        sNotice = NULL, sPhase = 0;
    Reply(s, ok ? "226 Done." : "451 Transfer failed.");
}

static void HandleSession(int ctrl)
{
    Session *s = (Session *)calloc(1, sizeof(Session));
    if (!s)
        return;
    s->ctrl = ctrl;
    s->pasv = -1;
    snprintf(s->cwd, sizeof(s->cwd), "/");
    if (sRestricted)
        Reply(s, "220 Sonic Mania 3DS FTP (install only: " FTP_RESTRICTED_ROOT ").");
    else
        Reply(s, "220 Sonic Mania 3DS dev FTP. SD card root; the game's files are in /3ds/Sonic Mania 3DS/.");
    char line[FTP_LINE_MAX];
    for (;;) {
        if (!sRun || !ReadLine(s, line, sizeof(line)))
            break;
        char *arg = strchr(line, ' ');
        if (arg)
            *arg++ = 0;
        else
            arg = line + strlen(line);
        for (char *c = line; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c = (char)(*c - 32);
        char vpath[FTP_PATH_MAX], rpath[FTP_PATH_MAX + 8];
        struct stat st;
        if (!strcmp(line, "USER"))
            Reply(s, "331 Any password.");
        else if (!strcmp(line, "PASS"))
            Reply(s, "230 Logged in.");
        else if (!strcmp(line, "SYST"))
            Reply(s, "215 UNIX Type: L8");
        else if (!strcmp(line, "FEAT"))
            Reply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n MLSD\r\n EPSV\r\n PASV\r\n UTF8\r\n211 End");
        else if (!strcmp(line, "OPTS") || !strcmp(line, "TYPE") || !strcmp(line, "MODE") || !strcmp(line, "STRU") || !strcmp(line, "NOOP"))
            Reply(s, "200 OK.");
        else if (!strcmp(line, "ALLO")) {
            sAllo = strtoull(arg, NULL, 10); // the next STOR's size: the progress bar's total
            Reply(s, "200 OK.");
        }
        else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD"))
            Reply(s, "257 \"%s\"", s->cwd);
        else if (!strcmp(line, "CWD") || !strcmp(line, "CDUP")) {
            ResolvePath(s, !strcmp(line, "CDUP") ? ".." : arg, vpath, sizeof(vpath));
            RealPath(vpath, rpath, sizeof(rpath));
            DIR *d = opendir(rpath);
            if (d) {
                closedir(d);
                snprintf(s->cwd, sizeof(s->cwd), "%s", vpath);
                Reply(s, "250 OK.");
            }
            else
                Reply(s, "550 No such directory.");
        }
        else if (!strcmp(line, "PASV")) {
            int port = OpenPasv(s);
            if (port < 0)
                Reply(s, "425 Cannot open a data port.");
            else
                Reply(s, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%d,%d).", (unsigned long)(sIp & 0xFF), (unsigned long)((sIp >> 8) & 0xFF),
                      (unsigned long)((sIp >> 16) & 0xFF), (unsigned long)((sIp >> 24) & 0xFF), port >> 8, port & 0xFF);
        }
        else if (!strcmp(line, "EPSV")) {
            int port = OpenPasv(s);
            if (port < 0)
                Reply(s, "425 Cannot open a data port.");
            else
                Reply(s, "229 Entering Extended Passive Mode (|||%d|).", port);
        }
        else if (!strcmp(line, "LIST"))
            CmdList(s, arg, 0);
        else if (!strcmp(line, "NLST"))
            CmdList(s, arg, 1);
        else if (!strcmp(line, "MLSD"))
            CmdList(s, arg, 2);
        else if (!strcmp(line, "RETR"))
            CmdRetr(s, arg);
        else if (!strcmp(line, "STOR"))
            CmdStor(s, arg, false);
        else if (!strcmp(line, "APPE"))
            CmdStor(s, arg, true);
        else if (!strcmp(line, "SIZE") || !strcmp(line, "MDTM")) {
            ResolvePath(s, arg, vpath, sizeof(vpath));
            RealPath(vpath, rpath, sizeof(rpath));
            if (!Allowed(vpath) || stat(rpath, &st) != 0 || S_ISDIR(st.st_mode))
                Reply(s, "550 No such file.");
            else if (line[0] == 'S')
                Reply(s, "213 %llu", (unsigned long long)st.st_size);
            else
                Reply(s, "213 20260101000000");
        }
        else if (!strcmp(line, "DELE") || !strcmp(line, "RMD") || !strcmp(line, "XRMD") || !strcmp(line, "MKD") || !strcmp(line, "XMKD")) {
            ResolvePath(s, arg, vpath, sizeof(vpath));
            RealPath(vpath, rpath, sizeof(rpath));
            int r = !Allowed(vpath) ? -1 : line[0] == 'D' ? remove(rpath) : strstr(line, "RMD") ? rmdir(rpath) : mkdir(rpath, 0777);
            if (r != 0)
                Reply(s, "550 Failed.");
            else if (strstr(line, "MKD"))
                Reply(s, "257 \"%s\" created.", vpath);
            else
                Reply(s, "250 OK.");
        }
        else if (!strcmp(line, "RNFR")) {
            ResolvePath(s, arg, s->rnfr, sizeof(s->rnfr));
            Reply(s, "350 Ready for RNTO.");
        }
        else if (!strcmp(line, "RNTO")) {
            char from[FTP_PATH_MAX + 8];
            RealPath(s->rnfr, from, sizeof(from));
            ResolvePath(s, arg, vpath, sizeof(vpath));
            RealPath(vpath, rpath, sizeof(rpath));
            Reply(s, s->rnfr[0] && Allowed(s->rnfr) && Allowed(vpath) && rename(from, rpath) == 0 ? "250 Renamed." : "550 Rename failed.");
            s->rnfr[0] = 0;
        }
        else if (!strcmp(line, "SITE")) {
            // SITE INSTALL <path>: installs that CIA (this game's title ID only) over the running
            // game and relaunches into it, after a 10 s countdown shown on the bottom screen.
            char *sub  = arg;
            char *path = strchr(sub, ' ');
            if (path)
                *path++ = 0;
            for (char *c = sub; *c; c++)
                if (*c >= 'a' && *c <= 'z')
                    *c = (char)(*c - 32);
            if (!strcmp(sub, "INSTALL") && path && *path) {
                ResolvePath(s, path, vpath, sizeof(vpath));
                if (!Allowed(vpath)) {
                    Reply(s, "550 Not allowed.");
                    continue;
                }
                char error[96];
                sNotice = "INSTALLING NEW BUILD";
                sPhase  = 2;
                if (InstallCia(vpath, error, sizeof(error))) {
                    Reply(s, "200 Installed %s; relaunching the game.", vpath);
                    sNotice    = "RESTARTING IN";
                    sRelaunchAt = osGetTime() + RELAUNCH_MS;
                    sPhase     = 3;
                }
                else {
                    sPhase  = 0;
                    sNotice = NULL;
                    Reply(s, "550 Install failed: %s", error);
                }
            }
            else if (!strcmp(sub, "INSTALL"))
                Reply(s, "501 SITE INSTALL <path to a .cia of this game>");
#if MANIA_DEV_TOOLS
            else if (!strcmp(sub, "SCENES")) {
                // "0 Presentation(3) 1 Mania Mode(42) ...": category index, name and scene count.
                char list[FTP_LINE_MAX - 8];
                unsigned n = 0;
                for (int c = 0; c < sceneInfo.categoryCount && n + 40 < sizeof(list); ++c)
                    n += (unsigned)snprintf(list + n, sizeof(list) - n, "%d %s(%d) ", c, sceneInfo.listCategory[c].name,
                                            (int)sceneInfo.listCategory[c].sceneCount);
                list[n] = 0;
                Reply(s, "200 %s", list);
            }
            else if (!strcmp(sub, "SCENE") && path && *path) {
                // SITE SCENE <category> <index in category>; with no index, the category's scenes by name.
                int cat = atoi(path);
                char *idx = strchr(path, ' ');
                if (cat < 0 || cat >= sceneInfo.categoryCount)
                    Reply(s, "501 category 0..%d", (int)sceneInfo.categoryCount - 1);
                else if (!idx) {
                    const SceneListInfo *l = &sceneInfo.listCategory[cat];
                    char list[FTP_LINE_MAX - 8];
                    unsigned n = 0;
                    for (int i = 0; i < l->sceneCount && n + 40 < sizeof(list); ++i)
                        n += (unsigned)snprintf(list + n, sizeof(list) - n, "%d %s(%s) ", i, sceneInfo.listData[l->sceneOffsetStart + i].name,
                                                sceneInfo.listData[l->sceneOffsetStart + i].id);
                    list[n] = 0;
                    Reply(s, "200 %s", list);
                }
                else {
                    int scene = atoi(idx + 1);
                    if (scene < 0 || scene >= sceneInfo.listCategory[cat].sceneCount)
                        Reply(s, "501 scene 0..%d", (int)sceneInfo.listCategory[cat].sceneCount - 1);
                    else {
                        sPendingScene    = scene;
                        sPendingCategory = cat;
                        Reply(s, "200 loading category %d scene %d", cat, scene);
                    }
                }
            }
            else if (!strcmp(sub, "SHOT")) {
                // The top screen's framebuffer as debug/shot-NNNN.raw (tools/shot.sh converts it).
                sShotRequest = (path && !strncasecmp(path, "bottom", 6)) ? 2 : (path && !strncasecmp(path, "right", 5)) ? 3 : 1;
                Reply(s, "200 shot queued");
            }
            else if (!strcmp(sub, "GPU")) {
                // SITE GPU <0|1>: the renderer mode (the RENDERER button), switched at the end of the frame
                sPendingGpuMode = path && atoi(path) != 0;
                Reply(s, "200 renderer %s", sPendingGpuMode ? "gpu" : "cpu");
            }
            else if (!strcmp(sub, "SET") && path) {
                // SITE SET <frameskip|maxskip|draw|flipcopy|gpu|syncdraw|vblank> <value>
                char *val = strchr(path, ' ');
                const int v = val ? atoi(val + 1) : 0;
                bool ok     = true;
                if (!strncmp(path, "frameskip", 9))
                    Port::frameSkip = v != 0;
                else if (!strncmp(path, "maxskip", 7))
                    DevTools::swMaxSkip = v < 1 ? 1 : v > 4 ? 4 : v;
                else if (!strncmp(path, "draw", 4))
                    DevTools::swDraw = v != 0;
                else if (!strncmp(path, "flipcopy", 8))
                    DevTools::swFlipCopy = v != 0;
                else if (!strncmp(path, "gpu3d", 5))
                    DevTools::swGpu3D = v != 0;
                else if (!strncmp(path, "gpuskip", 7)) // before "gpu": it took gpuskip for gpu (0 turned the GPU renderer off)
                    GpuRender::gpuSkip = (unsigned)v;
                else if (!strncmp(path, "depthmode", 9))
                    GpuRender::depthMode = (unsigned)v & 2047;
                else if (!strncmp(path, "gpurender", 9))
                    DevTools::swGpuRender = v != 0;
                else if (!strncmp(path, "gpu", 3))
                    DevTools::swGpuPresent = v != 0;
                else if (!strncmp(path, "record", 6))
                    DevTools::swRecord = v != 0;
                else if (!strncmp(path, "bottomui", 8))
                    DevTools::swBottomUi = v != 0;
                else if (!strncmp(path, "syncdraw", 8))
                    DevTools::swSyncDraw = v != 0;
                else if (!strncmp(path, "vblank", 6))
                    DevTools::swVblankWait = v != 0;
                else if (!strncmp(path, "tab", 3))
                    Port::tab = v < 0 || v > 4 ? 0 : v;
                else if (!strncmp(path, "theme", 5)) // the bottom screen's colours (OPTIONS > THEME)
                    Port::theme = v < 0 || v > 15 ? 0 : v;
                else if (!strncmp(path, "fps", 3))
                    Port::fps = v < 0 || v > 4 ? 0 : v;
                else if (!strncmp(path, "hud", 3))
                    Port::hud = v < 0 || v > 2 ? 1 : v;
                else if (!strncmp(path, "lock30", 6))
                    Port::lock30 = v != 0;
                else if (!strncmp(path, "heapcheck", 9)) // debug: walk the heap's chunks every frame, log the first broken one
                    DevTools::heapCheck = v != 0;
                else if (!strncmp(path, "music", 5)) // debug: streams (music) off, to tell them from other faults; not saved
                    engine.streamsEnabled = v != 0;
                else if (!strncmp(path, "force3d", 7))
                    DevTools::swForce3D = v != 0;
                else if (!strncmp(path, "floorwin", 8))
                    GpuRender::floorWindows = v != 0;
                else if (!strncmp(path, "floorfogavg", 11)) {
                    GpuRender::floorFogAverage = v != 0;
                    GpuFrame::InvalidateContent(); // the windows' fog colour is set when they are built
                }
                else if (!strncmp(path, "floorfog", 8))
                    GpuRender::floorFogPercent = v < 0 ? 0 : v > 100 ? 100 : v;
                else if (!strncmp(path, "ufotime", 7))
                    sUfoTime = v != 0;
                else if (!strncmp(path, "modelcheck", 10))
                    sModelCheck = v != 0;
                else if (!strncmp(path, "gifcheck", 8))
                    gifCheck = v != 0;
                else if (!strncmp(path, "ufonofall", 9))
                    sUfoNoFall = v != 0;
                else if (!strncmp(path, "depthtint", 9))
                    GpuRender::depthTint = v != 0;
                else if (!strncmp(path, "stereofar", 9))
                    GpuRender::stereoFar = v < 0 ? 0 : v > 16 ? 16 : v;
                else if (!strncmp(path, "audioprio", 9)) {
                    // debug, not saved: the audio thread's priority (0x18-0x3F; lower runs first)
                    ok = v >= 0x18 && v <= 0x3F && R_SUCCEEDED(svcSetThreadPriority(threadGetHandle(audioThreadHandle), v));
                }
                else
                    ok = false;
                if (ok) {
                    DevTools::SaveSettings();
                    Reply(s, "200 %s", path);
                }
                else
                    Reply(s, "501 frameskip|maxskip|draw|flipcopy|gpu|record|gpurender|gpu3d|depthmode|bottomui|syncdraw|vblank|tab|fps|hud|lock30|force3d|stereofar|depthtint|ufotime|modelcheck|gifcheck|ufonofall|floorwin|floorfog|floorfogavg|gpuskip|audioprio <value>");
            }
            else if (!strcmp(sub, "MODELS")) {
                // Loaded 3D models: id, vertices per frame, frames, indices, face size, flags, coordinate range. "MODELS N"
                // starts at model N (the reply is cut at the FTP line limit).
                char list[FTP_LINE_MAX - 8];
                unsigned n = 0;
                const int from = path ? atoi(path) : 0;
                for (int m = from < 0 ? 0 : from; m < MODEL_COUNT && n + 64 < sizeof(list); ++m) {
                    const Model &md = modelList[m];
                    if (!md.vertices || !md.vertCount)
                        continue;
                    int32 lo = 0, hi = 0, nlo = 0, nhi = 0;
                    for (unsigned v = 0; v < (unsigned)md.vertCount * (md.frameCount ? md.frameCount : 1); ++v) {
                        const ModelVertex &mv = md.vertices[v];
                        lo = mv.x < lo ? mv.x : lo, lo = mv.y < lo ? mv.y : lo, lo = mv.z < lo ? mv.z : lo;
                        hi = mv.x > hi ? mv.x : hi, hi = mv.y > hi ? mv.y : hi, hi = mv.z > hi ? mv.z : hi;
                        nlo = mv.nx < nlo ? mv.nx : nlo, nlo = mv.ny < nlo ? mv.ny : nlo, nlo = mv.nz < nlo ? mv.nz : nlo;
                        nhi = mv.nx > nhi ? mv.nx : nhi, nhi = mv.ny > nhi ? mv.ny : nhi, nhi = mv.nz > nhi ? mv.nz : nhi;
                    }
                    n += (unsigned)snprintf(list + n, sizeof(list) - n, "%d:%ux%u fl%u p[%ld,%ld] n[%ld,%ld] ", m, md.vertCount, md.frameCount, md.flags, (long)lo,
                                            (long)hi, (long)nlo, (long)nhi);
                }
                list[n] = 0;
                Reply(s, "200 %s", list);
            }
            else if (!strcmp(sub, "CMDS")) {
                // The next recorded frame's command list, written to debug/cmds.txt (needs the GPU mode or SET record 1)
                GpuFrame::dumpPath    = "sdmc:/3ds/Sonic Mania 3DS/debug/cmds.txt";
                GpuFrame::dumpRequest = true;
                Reply(s, "200 next frame's commands go to debug/cmds.txt");
            }
            else if (!strcmp(sub, "LAYERS")) {
                // Scene layers, palettes, line banks and scanlines, for offline analysis: debug/layers.bin. Per layer
                // "LAYR", type, widthShift, heightShift, xsize, ysize, layout; then "PALS" fullPalette, "LINE"
                // gfxLineBuffer, "SCAN" scanlines (last drawn), "TILE" tilesetPixels.
                FILE *f = fopen("sdmc:/3ds/Sonic Mania 3DS/debug/layers.bin", "wb");
                if (f) {
                    for (int l = 0; l < LAYER_COUNT; ++l) {
                        const TileLayer &tl = tileLayers[l];
                        if (!tl.layout || !tl.xsize)
                            continue;
                        uint8 hdr[12] = { 'L', 'A', 'Y', 'R', (uint8)l, tl.type, tl.widthShift, tl.heightShift };
                        memcpy(hdr + 8, &tl.xsize, 2), memcpy(hdr + 10, &tl.ysize, 2);
                        fwrite(hdr, 1, 12, f);
                        fwrite(tl.layout, 2, (size_t)tl.xsize * tl.ysize, f);
                    }
                    // Scanlines: the shared buffer holds whatever layer ran last, so run the first rotozoom layer's
                    // callback again (debug only: it also sets the clip bounds and line banks, rewritten next frame).
                    static ScanlineInfo lines[SCREEN_YSIZE];
                    memcpy(lines, scanlines, sizeof(lines));
                    for (int l = 0; l < LAYER_COUNT; ++l) {
                        if (tileLayers[l].layout && tileLayers[l].type == LAYER_ROTOZOOM && tileLayers[l].scanlineCallback) {
                            tileLayers[l].scanlineCallback(lines);
                            break;
                        }
                    }
                    fwrite("PALS", 1, 4, f), fwrite(fullPalette, 1, sizeof(fullPalette), f);
                    fwrite("LINE", 1, 4, f), fwrite(gfxLineBuffer, 1, SCREEN_YSIZE, f);
                    fwrite("SCAN", 1, 4, f), fwrite(lines, sizeof(ScanlineInfo), SCREEN_YSIZE, f);
                    fwrite("TILE", 1, 4, f), fwrite(tilesetPixels, 1, TILESET_SIZE, f);
                    // Then every rotozoom layer's own lines: "SCNL", the layer, its scanlines
                    for (int l = 0; l < LAYER_COUNT; ++l) {
                        if (!tileLayers[l].layout || tileLayers[l].type != LAYER_ROTOZOOM || !tileLayers[l].scanlineCallback)
                            continue;
                        tileLayers[l].scanlineCallback(lines);
                        const uint8 id = (uint8)l;
                        fwrite("SCNL", 1, 4, f), fwrite(&id, 1, 1, f), fwrite(lines, sizeof(ScanlineInfo), SCREEN_YSIZE, f);
                    }
                    fclose(f);
                    Reply(s, "200 debug/layers.bin");
                }
                else
                    Reply(s, "550 cannot write debug/layers.bin");
            }
            else if (!strcmp(sub, "ACH") && path) {
                // SITE ACH <ACH_ID>: unlock one achievement, as the game does (test the notice, the sound, the tab)
                static char id[48];
                sscanf(path, "%47s", id);
                static SKU::AchievementID ach;
                ach.identifier = id;
                SKU::TryUnlockAchievement(&ach);
                Reply(s, "200 achievement %s", id);
            }
            else if (!strcmp(sub, "TOUCH") && path) {
                // SITE TOUCH <x> <y>: one tap on the bottom screen (320x240), to drive its tabs and options remotely.
                int x = -1, y = -1;
                if (sscanf(path, "%d %d", &x, &y) == 2 && x >= 0 && x < 320 && y >= 0 && y < 240) {
                    BottomUi::Touch(x, y);
                    Reply(s, "200 touch %d %d", x, y);
                }
                else
                    Reply(s, "501 TOUCH x y (0-319, 0-239)");
            }
            else if (!strcmp(sub, "HIDE")) {
                char name[48] = "";
                if (path)
                    sscanf(path, "%47s", name);
                sHideSet = false;
                if (name[0]) {
                    GEN_HASH_MD5(name, sHideHash);
                    sHideSet = true;
                }
                Reply(s, "200 hide %s", name[0] ? name : "-");
            }
            else if (!strcmp(sub, "DEPTHS")) {
                // The last 3D frame's depths; also debug/depths.txt: every layer's type, draw group and parallax
                // factors (vertical, then each scroll info's), to tune the stereo depths.
                FILE *f = fopen("sdmc:/3ds/Sonic Mania 3DS/debug/depths.txt", "w");
                if (f) {
                    fprintf(f, "%s\n", GpuRender::DepthSummary());
                    for (int l = 0; l < LAYER_COUNT; ++l) {
                        const TileLayer &tl = tileLayers[l];
                        if (!tl.layout || !tl.xsize)
                            continue;
                        fprintf(f, "L%d type %d group %d size %dx%d callback %d factor %d scroll", l, tl.type, tl.drawGroup[0], tl.xsize,
                                tl.ysize, tl.scanlineCallback != NULL, tl.parallaxFactor);
                        for (int i = 0; i < tl.scrollInfoCount; ++i)
                            fprintf(f, " %d", tl.scrollInfo[i].parallaxFactor);
                        fprintf(f, "\n");
                    }
                    fclose(f);
                }
                Reply(s, "200 %s", GpuRender::DepthSummary());
            }
            else if (!strcmp(sub, "MODELCHECK")) {
                // The 3D models' VRAM against what was uploaded, in debug/models.txt
                FILE *f = fopen("sdmc:/3ds/Sonic Mania 3DS/debug/models.txt", "w");
                if (f) {
                    GpuRender::CheckModels(f);
                    fclose(f);
                }
                Reply(s, f ? "200 debug/models.txt" : "550 cannot write debug/models.txt");
            }
            else if (!strcmp(sub, "FLOOR")) {
                // The Special Stage floor's last frame from its windows, a line each, in debug/floor.txt (GpuRender::DumpFloor)
                FILE *f = fopen("sdmc:/3ds/Sonic Mania 3DS/debug/floor.txt", "w");
                if (f) {
                    GpuRender::DumpFloor(f);
                    fclose(f);
                }
                Reply(s, f ? "200 debug/floor.txt" : "550 cannot write debug/floor.txt");
            }
            else if (!strcmp(sub, "READTEST")) {
                // The SD card's read speed on the data pack: 4 MB at 100 MB in one fread, in 64 KB freads, and with
                // FSFILE_Read (no stdio) into linear and into heap memory
                const char *pack = dataPackCount ? dataPacks[0].name : "";
                const u32 n = 4 << 20;
                u8 *heap = (u8 *)malloc(n), *lin = (u8 *)linearAlloc(64 * 1024);
                unsigned long ms[4] = {};
                auto now = []() { return svcGetSystemTick(); };
                const u64 k = SYSCLOCK_ARM11 / 1000;
                if (heap) {
                    if (FILE *f = fopen(pack, "rb")) {
                        fseek(f, 100 << 20, SEEK_SET);
                        u64 t = now();
                        fread(heap, 1, n, f);
                        ms[0] = (unsigned long)((now() - t) / k);
                        fseek(f, 108 << 20, SEEK_SET);
                        t = now();
                        for (u32 o = 0; o < n; o += 64 * 1024)
                            fread(heap + o, 1, 64 * 1024, f);
                        ms[1] = (unsigned long)((now() - t) / k);
                        fclose(f);
                    }
                    Handle h;
                    const char *rel = !strncmp(pack, "sdmc:", 5) ? pack + 5 : pack;
                    if (R_SUCCEEDED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, rel), FS_OPEN_READ, 0))) {
                        u32 got = 0;
                        u64 t = now();
                        FSFILE_Read(h, &got, 116ULL << 20, heap, n);
                        ms[2] = (unsigned long)((now() - t) / k);
                        t = now();
                        for (u32 o = 0; lin && o < n; o += 64 * 1024)
                            FSFILE_Read(h, &got, (124ULL << 20) + o, lin, 64 * 1024);
                        ms[3] = (unsigned long)((now() - t) / k);
                        FSFILE_Close(h);
                    }
                }
                free(heap);
                if (lin)
                    linearFree(lin);
                Reply(s, "200 %s: 4 MB fread %lu ms, 64 KB freads %lu ms, FSFILE_Read %lu ms, 64 KB FSFILE_Reads (linear) %lu ms", pack, ms[0], ms[1],
                      ms[2], ms[3]);
            }
            else if (!strcmp(sub, "ORDER")) {
                Reply(s, "200 %s", sOrderLast);
            }
            else if (!strcmp(sub, "SYNCDRAW")) {
                DevTools::swSyncDraw = path && atoi(path) != 0;
                Reply(s, "200 syncdraw %d", (int)DevTools::swSyncDraw);
            }
            else if (!strcmp(sub, "VBLANK")) {
                DevTools::swVblankWait = path && atoi(path) != 0;
                Reply(s, "200 vblank wait %d", (int)DevTools::swVblankWait);
            }
            else if (!strcmp(sub, "GOTO") && path) {
                // SITE GOTO <ObjectName> [dx]: moves the player (slot 0) next to the first entity of that class (dx
                // pixels to its left, 160 by default), e.g. GOTO DDWrecker for Green Hill 1's boss. Next frame.
                char name[48];
                int dx = 160;
                if (sscanf(path, "%47s %d", name, &dx) < 1)
                    name[0] = 0;
                uint32 hash[4];
                GEN_HASH_MD5(name, hash);
                int found = -1;
                for (int32 i = 0; i < sceneInfo.classCount && found < 0; ++i)
                    if (HASH_MATCH_MD5(objectClassList[stageObjectIDs[i]].hash, hash))
                        for (int32 e = 0; e < ENTITY_COUNT; ++e)
                            if (objectEntityList[e].classID == i) {
                                found = e;
                                break;
                            }
                if (found >= 0) {
                    sGotoX = objectEntityList[found].position.x - (dx << 16);
                    sGotoY = objectEntityList[found].position.y - (32 << 16);
                    sGoto  = true;
                    Reply(s, "200 entity %d at %ld %ld", found, (long)(objectEntityList[found].position.x >> 16), (long)(objectEntityList[found].position.y >> 16));
                }
                else
                    Reply(s, "550 no %s in this scene", name);
            }
            else if (!strcmp(sub, "SHADOW")) {
                entityShadowOff = path && atoi(path) == 0;
                Reply(s, "200 foreach shadow %s", entityShadowOff ? "off (searches the entities)" : "on");
            }
            else if (!strcmp(sub, "OBJTIME")) {
                // The eight object classes whose update, and whose draw, took the most since the last call (ms a second of
                // play), then reset
                static u64 last = 0;
                const u64 now   = svcGetSystemTick();
                const float sec = last ? (now - last) / (float)SYSCLOCK_ARM11 : 1.0f;
                last            = now;
                char buf[800] = "";
                int len       = 0;
                uint32 *const lists[2] = { DevTools::objTicks, DevTools::objDrawTicks };
                for (int l = 0; l < 2; ++l) {
                    uint32 *t = lists[l];
                    int top[8];
                    int n = 0;
                    for (int c = 0; c < OBJECT_COUNT; ++c) {
                        if (!t[c] || (n == 8 && t[top[7]] >= t[c]))
                            continue;
                        int k  = n < 8 ? n++ : 7;
                        top[k] = c;
                        for (; k > 0 && t[top[k - 1]] < t[top[k]]; --k)
                            std::swap(top[k], top[k - 1]);
                    }
                    len += snprintf(buf + len, sizeof(buf) - len, l ? " | draw:" : " update:");
                    for (int i = 0; i < n; ++i)
                        len += snprintf(buf + len, sizeof(buf) - len, " %s %.2f", objectClassList[top[i]].name, t[top[i]] * 1000.0f / SYSCLOCK_ARM11 / sec);
                    memset(t, 0, sizeof(DevTools::objTicks));
                }
                Reply(s, "200 ms/second:%s | shadow misses %u", buf, (unsigned)entityShadowMiss);
            }
            else if (!strcmp(sub, "CONV")) {
                // The last GPU frame's atlas conversions: which ones really changed colour
                Reply(s, "200 %s", GpuRender::ConvReport());
            }
            else if (!strcmp(sub, "SVAR") && path) {
                // SITE SVAR <Class> <byte offset> [n]: n int32 of a stage object's static variables (debug)
                char name[48];
                int off = 0, n = 8;
                name[0] = 0;
                sscanf(path, "%47s %d %d", name, &off, &n);
                n = n < 1 ? 1 : n > 32 ? 32 : n;
                uint32 hash[4];
                GEN_HASH_MD5(name, hash);
                const uint8 *vars = NULL;
                for (int i = 0; i < sceneInfo.classCount && !vars; ++i)
                    if (HASH_MATCH_MD5(objectClassList[stageObjectIDs[i]].hash, hash) && objectClassList[stageObjectIDs[i]].staticVars)
                        vars = (const uint8 *)*objectClassList[stageObjectIDs[i]].staticVars;
                if (!vars)
                    Reply(s, "550 no %s in this scene", name);
                else {
                    char line[400];
                    int len = 0;
                    for (int k = 0; k < n && len < (int)sizeof(line) - 16; ++k)
                        len += snprintf(line + len, sizeof(line) - len, " %ld", (long)*(const int32 *)(vars + off + k * 4));
                    Reply(s, "200 %s +%d:%s", name, off, line);
                }
            }
            else if (!strcmp(sub, "REFUSED")) {
                // The last command the GPU executor refused (SITE STATE's line is cut by the FTP line limit)
                Reply(s, "200 %s | why %s | roto to software: runs %u quads %u, soft layers %u, roto texture %s, vram free %u KB", GpuRender::LastRefusal(), sLastWhy ? sLastWhy : "-", GpuRender::GetStats().rotoSoftRuns, GpuRender::GetStats().rotoSoftQuads, GpuRender::GetStats().softLayers, GpuRender::RotoTexWhy(), (unsigned)(vramSpaceFree() >> 10));
            }
            else if (!strcmp(sub, "FREEZE")) {
                // SITE FREEZE 1: the scene stops (no object updates) but every frame is still drawn, so the GPU and
                // the CPU renderers can be compared on the same picture. FREEZE 0 resumes.
                DevTools::freeze = path && atoi(path) != 0;
                Reply(s, "200 freeze %d", (int)DevTools::freeze);
            }
            else if (!strcmp(sub, "RUNREC")) {
                // The current scene again from its load, recording the keys until RUNSTOP (or five minutes).
                sRunCategory = sceneInfo.activeCategory, sRunListPos = sceneInfo.listPos;
                sRunCount = 0, sRunContent = GpuFrame::contentVersion;
                sRunRecLimit = path ? (unsigned)atoi(path) : 0;
                sRunMode = RUN_ARM_REC;
                // Loaded anew by the main thread (FrameEnd), as SITE SCENE does.
                sPendingHard     = true;
                sPendingCategory = sRunCategory;
                sPendingScene    = sRunListPos - sceneInfo.listCategory[sRunCategory].sceneOffsetStart;
                Reply(s, "200 recording from the reload of category %d list %d", sRunCategory, sRunListPos);
            }
            else if (!strcmp(sub, "RUNSTOP")) {
                if (sRunMode == RUN_REC || sRunMode == RUN_ARM_REC) {
                    sRunMode = RUN_OFF;
                    RunSave();
                    snprintf(sRunResult, sizeof(sRunResult), "recorded %u passes (%.1f s) of category %d list %d", sRunCount, sRunCount / 60.0f,
                             sRunCategory, sRunListPos);
                }
                else
                    sRunMode = RUN_OFF;
                Reply(s, "200 %s", sRunResult);
            }
            else if (!strcmp(sub, "RUNPLAY")) {
                // The recorded run (debug/run.bin) from its scene's load; RUN shows the result (drawn fps). RUNPLAY N: only
                // its first N passes (3600: a minute of play).
                FILE *f = fopen(kRunPath, "rb");
                int32 hdr[3] = { -1, -1, 0 };
                if (f) {
                    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[2] > 0 && hdr[2] <= RUN_MAX)
                        sRunCount = (unsigned)fread(sRunKeys, 2, (size_t)hdr[2], f);
                    fclose(f);
                }
                unsigned limit = 0;
                char freezeArg[4] = "";
                if (path)
                    sscanf(path, "%u %3s", &limit, freezeArg);
                sRunFreezeAtEnd = freezeArg[0] == 'F' || freezeArg[0] == 'f';
                if (limit && limit < sRunCount)
                    sRunCount = limit;
                if (hdr[0] < 0 || !sRunCount)
                    Reply(s, "550 no recorded run (RUNREC, then RUNSTOP)");
                else {
                    sRunCategory = hdr[0], sRunListPos = hdr[1];
                    sRunPos = 0, sRunContent = GpuFrame::contentVersion;
                    sRunMode = RUN_ARM_PLAY;
                    sPendingHard     = true;
                    sPendingCategory = sRunCategory;
                    sPendingScene    = sRunListPos - sceneInfo.listCategory[sRunCategory].sceneOffsetStart;
                    snprintf(sRunResult, sizeof(sRunResult), "playing %u passes", sRunCount);
                    Reply(s, "200 playing %u passes of category %d list %d", sRunCount, sRunCategory, sRunListPos);
                }
            }
            else if (!strcmp(sub, "TIMES")) {
                // The frame's costs in one short line (ms; STATE's is cut by the FTP limit): logic, recording, flip
                // (pcopy geometry, pframe citro3d frame), wait, and the GPU's own drawing time.
                const GpuRender::Stats &g = GpuRender::GetStats();
                unsigned bottomDraws;
                float bottomMs;
                BottomUi::DrawStats(&bottomDraws, &bottomMs);
                Reply(s, "200 frame %.1f upd %.1f draw %.1f flip %.1f pcopy %.1f pframe %.1f wait %.1f bottom %.1f (redraws %u, %.0f ms) | gpu draw %.1f submit %.1f | quads %u batches %u cmdbuf %u%% eye %u KB 3d %u KB (%u models in %u scenes) dropped %u",
                      sLastFrameMs, sLastStageMs[DevTools::STAGE_UPDATE], sLastStageMs[DevTools::STAGE_DRAW], sLastStageMs[DevTools::STAGE_FLIP], sLastStageMs[DevTools::STAGE_PCOPY],
                      sLastStageMs[DevTools::STAGE_PFRAME], sLastStageMs[DevTools::STAGE_WAIT], sLastStageMs[DevTools::STAGE_BOTTOM], bottomDraws, bottomMs, g.gpuDrawMs, g.gpuProcMs, g.quads, g.batches, g.cmdBufPct, g.cmdEyeKB, g.cmd3dKB, g.instances3d, g.scenes3d, g.eyesDropped);
            }
            else if (!strcmp(sub, "C3D")) {
                // pframe split, averages since the last C3D (ms a frame): FrameBegin's wait, each eye's commands, the
                // FPS overlay, FrameEnd; and the audio thread's mixing as a share of the time since the last C3D.
                static u64 lastAt, lastMix, lastDecode;
                const u64 now = svcGetSystemTick(), mix = ctrAudioMixTicks, decode = ctrAudioDecodeTicks;
                const float audioPct  = lastAt ? (float)(mix - lastMix) * 100.0f / (float)(now - lastAt) : 0.0f;
                const float decodePct = lastAt ? (float)(decode - lastDecode) * 100.0f / (float)(now - lastAt) : 0.0f;
                lastAt = now, lastMix = mix, lastDecode = decode;
                float c[5];
                const unsigned n = GpuRender::TakeC3DTimes(c);
                s32 audioPrio = 0, mainPrio = 0;
                svcGetThreadPriority(&audioPrio, threadGetHandle(audioThreadHandle));
                mainPrio = sMainPrio;
                Reply(s, "200 begin %.2f eye %.2f %.2f overlay %.2f end %.2f (%u frames) | pxfer %.2f pframe %.2f | mix %.1f%% ogg %.1f%% (core %d) underruns %lu starved %lu core 1 %lu%% prio 0x%lX main 0x%lX", c[0], c[1], c[2], c[3], c[4], n,
                      sLastStageMs[DevTools::STAGE_PXFER], sLastStageMs[DevTools::STAGE_PFRAME], audioPct, decodePct, ctrAudioCore, (unsigned long)ctrAudioUnderruns, (unsigned long)ctrMusicStarved,
                      (unsigned long)ctrAudioCore1Limit, (unsigned long)audioPrio, (unsigned long)mainPrio);
            }
            else if (!strcmp(sub, "EMIT")) {
                // pcopy split: before the commands, then the geometry building per command type (ms, last frame).
                const GpuRender::Stats &g = GpuRender::GetStats();
                using namespace GpuFrame;
                Reply(s, "200 pre %.2f | layer %.2f (roto: texture way %.2f, strips %.2f, floor %.2f; roto textures %.2f, floor moves %.2f in %u writes) roto-spr %.2f sprite %.2f 3d %.2f faces %.2f face %.2f line %.2f rect %.2f fill %.2f circle %.2f other %.2f",
                      g.preMs, g.emitMs[CMD_LAYER], g.rotoTexUs / 1000.0f, g.rotoStripUs / 1000.0f, g.rotoFloorUs / 1000.0f, g.rotoBuildUs / 1000.0f, g.floorMoveUs / 1000.0f, g.floorWrites, g.emitMs[CMD_ROTO], g.emitMs[CMD_SPRITE], g.emitMs[CMD_SCENE3D],
                      g.emitMs[CMD_FACES] + g.emitMs[CMD_BLENDED_FACES], g.emitMs[CMD_FACE] + g.emitMs[CMD_BLENDED_FACE], g.emitMs[CMD_LINE],
                      g.emitMs[CMD_RECT], g.emitMs[CMD_FILL], g.emitMs[CMD_CIRCLE] + g.emitMs[CMD_CIRCLE_OUTLINE],
                      g.emitMs[CMD_LINEBUF] + g.emitMs[CMD_PALETTE]);
            }
            else if (!strcmp(sub, "RUN")) {
                static const char *kMode[] = { "off", "armed to record", "recording", "armed to play", "playing" };
                Reply(s, "200 %s %u/%u | %s", kMode[sRunMode], sRunMode == RUN_PLAY ? sRunPos : sRunCount, sRunCount, sRunResult);
            }
            else if (!strcmp(sub, "INPUT")) {
                // SITE INPUT <hex mask of 3DS keys, e.g. 10 = A, 10 = KEY_A bit>: held until set to 0.
                DevTools::injectKeys = path ? (uint32)strtoul(path, NULL, 16) : 0;
                Reply(s, "200 keys %08lx", (unsigned long)DevTools::injectKeys);
            }
            else if (!strcmp(sub, "SURFACES")) {
                // Sprite sheets and tile sheet in memory: how much GPU texture memory a scene would need.
                char list[FTP_LINE_MAX - 8];
                unsigned n = 0, total = 0, count = 0;
                for (int i = 0; i < SURFACE_COUNT; ++i) {
                    const GFXSurface &g = gfxSurface[i];
                    if (g.scope == SCOPE_NONE || !g.pixels)
                        continue;
                    ++count;
                    total += (unsigned)g.width * g.height;
                    if (n + 24 < sizeof(list))
                        n += (unsigned)snprintf(list + n, sizeof(list) - n, "%dx%d ", (int)g.width, (int)g.height);
                }
                list[n] = 0;
                Reply(s, "200 %u sheets, %u KB at 8 bit (%u KB at 16 bit): %s", count, total >> 10, (total * 2) >> 10, list);
            }
            else if (!strcmp(sub, "STATE")) {
                Reply(s, "200 category %d scene %d state %d skipped %u fps %.1f frame %.1f upd %.1f draw %.1f flip %.1f | layers %.1f sprites %.1f roto %.1f faces %.1f 3d %.1f shapes %.1f | pcopy %.1f pxfer %.1f pframe %.1f wait %.1f | cmds %u replayed %u gpu %u quads %u reason %u cmd %u tileconv %u sprconv %u layers %u tileq %u empty %u rowsout %u | renderer %s frames gpu %u cpu %u (last second %u/%u) why %s | atlas retries %u quad retries %u cmdbuf %u%% eyes dropped %u tile evictions %u roto %u us %u quads tex %u soft layers %u faces %u | heap used %u KB linear free %u KB | refused %s",
                      (int)sceneInfo.activeCategory, (int)sceneInfo.listPos, (int)sceneInfo.state, sLastSkipped, sLastFps, sLastFrameMs, sLastStageMs[0],
                      sLastStageMs[1], sLastStageMs[2], sLastStageMs[3], sLastStageMs[4], sLastStageMs[5], sLastStageMs[6], sLastStageMs[7],
                      sLastStageMs[8], sLastStageMs[9], sLastStageMs[10], sLastStageMs[11], sLastStageMs[12], GpuFrame::GetStats().commands, GpuFrame::GetStats().replayed, GpuFrame::GetStats().gpuFrames, GpuRender::GetStats().quads,
                      GpuRender::GetStats().reason, GpuRender::GetStats().unsupportedCmd, GpuRender::GetStats().tileConversions, GpuRender::GetStats().spriteConversions, GpuRender::GetStats().layerCmds,
                      GpuRender::GetStats().tileQuads, GpuRender::GetStats().emptyTiles, GpuRender::GetStats().rowsOut, DevTools::GpuMode() ? "gpu" : "cpu",
                      sTotGpu, sTotCpu, sLastSecGpu, sLastSecCpu, sLastWhy ? sLastWhy : "-", GpuRender::GetStats().atlasRetries, GpuRender::GetStats().quadRetries, GpuRender::GetStats().cmdBufPct, GpuRender::GetStats().eyesDropped, GpuRender::GetStats().tileEvictions, GpuRender::GetStats().rotoUs, GpuRender::GetStats().rotoQuads, GpuRender::GetStats().rotoTexBuilds, GpuRender::GetStats().softLayers, GpuRender::GetStats().faces, (unsigned)(mallinfo().uordblks >> 10), (unsigned)(linearSpaceFree() >> 10), GpuRender::LastRefusal());
            }
            else
                Reply(s, "504 Unknown SITE command.");
#else
            else
                Reply(s, "504 Unknown SITE command.");
#endif
        }
        else if (!strcmp(line, "QUIT")) {
            Reply(s, "221 Bye.");
            break;
        }
        else
            Reply(s, "502 Not implemented.");
    }
    ClosePasv(s);
    free(s);
}

static void FtpMain(void *)
{
    while (sRun) {
        if (!WaitFd(sListen, POLLIN, 0))
            continue;
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd        = accept(sListen, (struct sockaddr *)&peer, &len);
        if (fd < 0)
            continue;
        HandleSession(fd);
        CloseSocket(&fd);
    }
}

static bool StartServer()
{
    // The network is shared with the updater (Port::NetAcquire); the server keeps its share until the process ends.
    if (!Port::NetAcquire()) {
        snprintf(sStatus, sizeof(sStatus), "NO NETWORK SERVICE");
        return false;
    }
    if (!Port::WifiConnected()) {
        snprintf(sStatus, sizeof(sStatus), "NO WI-FI");
        Port::NetRelease();
        return false;
    }
    sIo     = (char *)malloc(FTP_IO_SIZE);
    sListen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sListen >= 0) {
        int yes = 1;
        setsockopt(sListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(FTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (!sIo || sListen < 0 || bind(sListen, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sListen, 1) != 0) {
        snprintf(sStatus, sizeof(sStatus), "PORT %d BUSY", FTP_PORT);
        CloseSocket(&sListen);
        free(sIo);
        sIo = NULL;
        Port::NetRelease();
        return false;
    }
    sIp  = (uint32_t)gethostid();
    sRun = true;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
#if MANIA_DEV_TOOLS
    sMainPrio = priority;
#endif
    priority = priority + 4 <= 0x3F ? priority + 4 : 0x3F;
    sThread  = threadCreate(FtpMain, NULL, 64 * 1024, priority, 0, false);
    if (!sThread) {
        sRun = false;
        snprintf(sStatus, sizeof(sStatus), "NO THREAD");
        return false;
    }
    struct in_addr a;
    a.s_addr = sIp;
    snprintf(sStatus, sizeof(sStatus), "%s:%d", inet_ntoa(a), FTP_PORT);
    return true;
}

// FTP OFF (DEBUG tab): the thread ends within a nap (it checks sRun between polls); the network stays up.
static void StopServer()
{
    sRun = false;
    if (sThread) {
        if (threadJoin(sThread, 3000000000ULL) == 0)
            threadFree(sThread);
        sThread = NULL;
    }
    CloseSocket(&sListen);
    free(sIo);
    sIo = NULL;
    Port::NetRelease();
    snprintf(sStatus, sizeof(sStatus), "OFF");
}

// ---------------------------------------------------------------- DevFtp (every build)

int DevFtp::InstallNotice(char *title, unsigned cap, float *progress, unsigned *secondsLeft)
{
    const int phase = sPhase;
    if (!sNotice || !phase)
        return 0;
    snprintf(title, cap, "%s", sNotice);
    *progress    = -1;
    *secondsLeft = 0;
    if (phase == 1 && sRecvTotal)
        *progress = (float)sRecvDone / (float)sRecvTotal;
    else if (phase == 2 && sCiaTotal)
        *progress = (float)sCiaDone / (float)sCiaTotal;
    else if (phase == 3) {
        u64 now = osGetTime(), at = sRelaunchAt;
        *progress    = now >= at ? 1.0f : 1.0f - (float)(at - now) / (float)RELAUNCH_MS;
        *secondsLeft = now >= at ? 0 : (unsigned)((at - now + 999) / 1000);
    }
    return phase;
}

bool DevFtp::InstallOnly() { return sRestricted && sRun; }

const char *DevFtp::Address() { return sStatus; }

#if !MANIA_DEV_TOOLS
// dev-ftp.txt in the game's folder: the install-only server, so a new build can be sent over FTP to a production
// install (build_3ds.sh --install). Tried every ~2 s until Wi-Fi is up; never again after a CANCEL.
bool DevFtp::Tick()
{
    static int marker = -1;
    static unsigned wait;
    if (marker < 0)
        marker = access("sdmc:/3ds/Sonic Mania 3DS/dev-ftp.txt", F_OK) == 0;
    if (marker && !sRun && !sCancelled && ++wait >= 120) {
        wait = 0;
        if (osGetWifiStrength() > 0)
            StartServer();
    }
    // SITE INSTALL put a new build in place: the countdown has run out, start it (as HOME -> close)
    if (sPhase == 3 && osGetTime() >= sRelaunchAt) {
        aptSetChainloaderToSelf();
        return true;
    }
    return false;
}

void DevFtp::Cancel()
{
    sCancelled = true;
    if (sRun)
        StopServer();
}

void DevFtp::Shutdown()
{
    // As DevTools::Shutdown: the thread ends within a nap; one still alive is left alone
    sRun = false;
    if (sThread) {
        if (threadJoin(sThread, 3000000000ULL) == 0)
            threadFree(sThread);
        sThread = NULL;
    }
    CloseSocket(&sListen);
}
#endif

#if MANIA_DEV_TOOLS
bool DevTools::FtpRunning() { return sRun; }

void DevTools::SetFtp(bool on)
{
    swFtp = on;
    if (on && !sRun) {
        if (!StartServer())
            Log("ftp: not started: %s", sStatus);
    }
    else if (!on && sRun)
        StopServer();
}

// ---------------------------------------------------------------- perf timing

static const char *kStageName[DevTools::STAGE_COUNT] = { "update", "draw", "flip", "layers", "sprites", "roto", "faces", "3d", "shapes", "pcopy", "pxfer", "pframe", "wait", "bottom" };
static const char *kDir                              = "sdmc:/3ds/Sonic Mania 3DS/debug";

static uint64 sStageTicks[DevTools::STAGE_COUNT];
static uint64 sFrameTicksSum, sFrameTicksMax, sLastFrameTick, sSecondStart;
static unsigned sFrames;
static FILE *sCsv;

static unsigned sOrderLen;
static char sOrderKind;
static unsigned sOrderRun;

static void OrderFlush()
{
    if (sOrderRun && sOrderLen + 8 < sizeof(sOrderCur))
        sOrderLen += (unsigned)snprintf(sOrderCur + sOrderLen, sizeof(sOrderCur) - sOrderLen, "%c%u ", sOrderKind, sOrderRun);
    sOrderRun = 0;
}

void DevTools::OrderBegin()
{
    OrderFlush();
    sOrderCur[sOrderLen] = 0;
    memcpy(sOrderLast, sOrderCur, sizeof(sOrderLast));
    sOrderLen = 0;
    sOrderCur[0] = 0;
    sOrderKind   = 0;
}

void DevTools::OrderNote(char kind)
{
    if (kind != sOrderKind) {
        OrderFlush();
        sOrderKind = kind;
    }
    ++sOrderRun;
}

DevTools::StageScope::StageScope(Stage s) : stage(s), start(svcGetSystemTick())
{
    static const char kinds[STAGE_COUNT] = { 0, 0, 0, 'L', 'S', 'R', 'F', '3', 'P', 0, 0, 0, 0, 0 };
    if (kinds[s])
        OrderNote(kinds[s]);
}
DevTools::StageScope::~StageScope() { StageAdd(stage, svcGetSystemTick() - start); }

void DevTools::StageAdd(Stage stage, uint64 ticks) { sStageTicks[stage] += ticks; }

void DevTools::Log(const char *fmt, ...)
{
    static FILE *log;
    if (!log) {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/Sonic Mania 3DS", 0777);
        mkdir(kDir, 0777);
        char path[160], prev[160];
        snprintf(path, sizeof(path), "%s/log.txt", kDir);
        // The previous run's log (with its exit steps) is kept as log-prev.txt.
        snprintf(prev, sizeof(prev), "%s/log-prev.txt", kDir);
        remove(prev);
        rename(path, prev);
        log = fopen(path, "w");
        if (!log)
            return;
    }
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    struct mallinfo mi = mallinfo();
    fprintf(log, "[%6llu ms] %s  (heap used %u KB, linear free %u KB)\n", (unsigned long long)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000)), line,
            (unsigned)(mi.uordblks >> 10), (unsigned)(linearSpaceFree() >> 10));
    fflush(log);
}

static void OpenCsv()
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/Sonic Mania 3DS", 0777);
    mkdir(kDir, 0777);
    for (int i = 1; i < 10000 && !sCsv; ++i) {
        char path[160];
        snprintf(path, sizeof(path), "%s/perf-%04d.csv", kDir, i);
        struct stat st;
        if (stat(path, &st) != 0) {
            sCsv = fopen(path, "w");
            if (sCsv)
                fprintf(sCsv, "second,fps,frame_ms_avg,frame_ms_max,update_ms,draw_ms,flip_ms,layers_ms,sprites_ms,roto_ms,faces_ms,d3_ms,shapes_ms,scene,gpu_frames,cpu_frames\n");
        }
    }
}

// ---------------------------------------------------------------- bottom-screen menu
//
// The bottom screen is the libctru text console (40x30). Each row below the header is a "cell":
// tap it to flip or cycle its value, as in the DEBUG tab of the owner's other ports.

int DevTools::swMaxSkip    = 2;
bool DevTools::swDraw      = true;
bool DevTools::swFlipCopy  = true;
bool DevTools::swGpuRender = true;
bool DevTools::swGpu3D  = true; // on since the 2DS ran the Special Stage with it (off, the faces overflow the quads)
bool DevTools::swRecord = true;
bool DevTools::swBottomUi = true;
bool DevTools::swSyncDraw = false;   // waiting for the previous GPU frame in FrameBegin cost 10 ms (46 fps instead of 60)
bool DevTools::swVblankWait = false; // the clock paces the loop now (RenderDevice::UpdateFPSCap)
bool DevTools::swGpuPresent = true; // the renderer starts in GPU mode (SetGpuMode, the RENDERER button)
unsigned DevTools::skipCount = 0;
uint32 DevTools::injectKeys  = 0;
uint32 DevTools::objTicks[0x400];
uint32 DevTools::objDrawTicks[0x400];
bool DevTools::freeze         = false;
bool DevTools::immortal       = false;
bool DevTools::swForce3D      = false;
bool DevTools::swFtp          = true;

void DevTools::RunKeys(uint32 *held, uint32 *down)
{
    const int mode = sRunMode;
    if (mode == RUN_OFF)
        return;
    // Armed: wait for the reload to end (the first regular pass with a new content version).
    if (mode == RUN_ARM_REC || mode == RUN_ARM_PLAY) {
        if (GpuFrame::contentVersion == sRunContent || sceneInfo.state == ENGINESTATE_LOAD) {
            if (mode == RUN_ARM_PLAY)
                *held = *down = 0;
            return;
        }
        sRunMode    = mode == RUN_ARM_REC ? RUN_REC : RUN_PLAY;
        // The same random numbers in the recording and the playback (the game's ReplayRecorder seeds it the same
        // way); without it a replay drifted off the route.
        SetRandSeed(1624633040);
        // Statics the Special Stage does not reset on a reload (its frame counter: every other frame does things):
        // from the same values in the recording and the playback.
        sRunFrames0 = sTotGpu + sTotCpu;
        sRunStarved0 = ctrMusicStarved;
        sRunTick0   = svcGetSystemTick();
        Log("run: %s from category %d list %d", mode == RUN_ARM_REC ? "recording" : "playing", sRunCategory, sRunListPos);
    }
    if (sRunMode == RUN_REC) {
        if (sRunRecLimit && sRunCount >= sRunRecLimit) {
            // Done: saved as RUNSTOP does, and frozen on this pass.
            sRunMode = RUN_OFF;
            freeze   = true;
            RunSave();
            snprintf(sRunResult, sizeof(sRunResult), "recorded %u passes, frozen", sRunCount);
            return;
        }
        if (sRunCount < RUN_MAX)
            sRunKeys[sRunCount++] = (uint16)((*held & 0xFFF) | ((*held >> 16) & 0xF000)); // circle pad bits 28-31 -> 12-15
        return;
    }
    // Playing: the recorded keys instead of the real ones; at the end, the drawn frames a second over the run.
    if (sRunPos < sRunCount) {
        // The buttons, and the circle pad's directions (KEY_LEFT etc. are the d-pad's or the circle pad's).
        auto unpack       = [](uint16 k) { return (uint32)(k & 0xFFF) | ((uint32)(k & 0xF000) << 16); };
        const uint32 prev = sRunPos ? unpack(sRunKeys[sRunPos - 1]) : 0;
        *held             = unpack(sRunKeys[sRunPos]);
        *down             = *held & ~prev;
        ++sRunPos;
        return;
    }
    const float secs = (float)(svcGetSystemTick() - sRunTick0) / SYSCLOCK_ARM11;
    const unsigned frames = sTotGpu + sTotCpu - sRunFrames0;
    snprintf(sRunResult, sizeof(sRunResult), "played %u passes in %.1f s: %u frames drawn, %.1f fps, music gaps %u", sRunCount, secs, frames,
             secs > 0 ? frames / secs : 0.0f, (unsigned)(ctrMusicStarved - sRunStarved0));
    Log("run: %s", sRunResult);
    sRunMode = RUN_OFF;
    if (sRunFreezeAtEnd)
        freeze = true;
    *held = *down = 0;
}

bool DevTools::HideEntityClass(uint16 classID)
{
    if (sHideSet && HASH_MATCH_MD5(objectClassList[stageObjectIDs[classID]].hash, sHideHash))
        return true;
    return Port::HideHud(classID);
}

static unsigned sMarks;
static bool sMenuDirty = true;

struct Cell {
    const char *label;
    void (*tap)();
    void (*value)(char *out, unsigned cap);
};

static void ValFrameSkip(char *o, unsigned c) { snprintf(o, c, "%s", Port::frameSkip ? "ON" : "OFF"); }
static void TapFrameSkip() { Port::frameSkip = !Port::frameSkip; }
static void ValMaxSkip(char *o, unsigned c) { snprintf(o, c, "%d", DevTools::swMaxSkip); }
static void TapMaxSkip() { DevTools::swMaxSkip = DevTools::swMaxSkip % 4 + 1; }
static void ValDraw(char *o, unsigned c) { snprintf(o, c, "%s", DevTools::swDraw ? "ON" : "OFF (logic only)"); }
static void TapDraw() { DevTools::swDraw = !DevTools::swDraw; }
static void ValFlip(char *o, unsigned c) { snprintf(o, c, "%s", DevTools::swFlipCopy ? "ON" : "OFF (no copy)"); }
static void TapFlip() { DevTools::swFlipCopy = !DevTools::swFlipCopy; }
static void ValGpu(char *o, unsigned c) { snprintf(o, c, "%s", DevTools::GpuMode() ? "GPU" : "CPU"); }
static void TapGpu() { DevTools::SetGpuMode(!DevTools::GpuMode()); }
static void ValMark(char *o, unsigned c) { snprintf(o, c, "%u", sMarks); }
static void TapMark()
{
    ++sMarks;
    DevTools::Log("--- MARK %u --- scene %d, %.1f fps", sMarks, (int)sceneInfo.listPos, sLastFps);
    if (sCsv) {
        fprintf(sCsv, "# MARK %u\n", sMarks);
        fflush(sCsv);
    }
}
static void ValFtp(char *o, unsigned c) { snprintf(o, c, "%s", sStatus); }
static void ValNone(char *, unsigned) {}

static const Cell kCells[] = {
    { "FRAME SKIP", TapFrameSkip, ValFrameSkip },
    { "MAX SKIPPED IN A ROW", TapMaxSkip, ValMaxSkip },
    { "DRAW LISTS", TapDraw, ValDraw },
    { "FLIP COPY", TapFlip, ValFlip },
    { "RENDERER", TapGpu, ValGpu },
    { "FTP SERVER", NULL, ValFtp },
    { "LOG MARK", TapMark, ValMark },
};
enum { CELL_COUNT = sizeof(kCells) / sizeof(kCells[0]), CELL_FIRST_ROW = 8, CELL_ROWS = 3 };
// The console has 30 rows: printing below the last one scrolls everything (that made the whole menu vanish
// once the cells outgrew the screen), so the layout must fit: cells, then the notice row.
static_assert(CELL_FIRST_ROW + CELL_COUNT * CELL_ROWS <= 29, "the menu does not fit the 30-row console");

static void PrintRow(int row, const char *text)
{
    // The console is slow to paint: only rows whose text changed are written.
    static char shown[30][41];
    static u64 lastFullRepaint;
    if (svcGetSystemTick() - lastFullRepaint > 3ULL * SYSCLOCK_ARM11) {
        memset(shown, 0, sizeof(shown)); // repaint everything now and then, whatever the screen kept
        lastFullRepaint = svcGetSystemTick();
    }
    if (row < 0 || row >= 30)
        return; // never print on or below the console's last row: it scrolls
    char padded[41];
    snprintf(padded, sizeof(padded), "%-39.39s", text); // 39: the 40th column would wrap, and wrapping on the last row scrolls
    if (!strcmp(shown[row], padded))
        return;
    memcpy(shown[row], padded, sizeof(padded));
    printf("\x1b[%d;1H%s", row + 1, padded);
}

static void DrawStatus()
{
    char line[96], val[48];
    PrintRow(0, "SONIC MANIA 3DS  dev tools");
    snprintf(line, sizeof(line), "%.1f fps  frame %.1f ms", sLastFps, sLastFrameMs);
    PrintRow(2, line);
    snprintf(line, sizeof(line), "upd %.1f  draw %.1f  flip %.1f", sLastStageMs[0], sLastStageMs[1], sLastStageMs[2]);
    PrintRow(3, line);
    snprintf(line, sizeof(line), "lay %.1f spr %.1f rot %.1f", sLastStageMs[3], sLastStageMs[4], sLastStageMs[5]);
    PrintRow(4, line);
    snprintf(line, sizeof(line), "face %.1f 3d %.1f shp %.1f", sLastStageMs[6], sLastStageMs[7], sLastStageMs[8]);
    PrintRow(5, line);
    extern u32 __ctru_heap_size;
    struct mallinfo mi = mallinfo();
    snprintf(line, sizeof(line), "heap %u/%u KB  lin free %u KB", (unsigned)(mi.uordblks >> 10), (unsigned)(__ctru_heap_size >> 10),
             (unsigned)(linearSpaceFree() >> 10));
    PrintRow(6, line);
    snprintf(line, sizeof(line), "scene %d  skipped passes/s %u", (int)sceneInfo.listPos, sLastSkipped);
    PrintRow(7, line);
    for (int i = 0; i < CELL_COUNT; ++i) {
        val[0] = 0;
        kCells[i].value(val, sizeof(val));
        snprintf(line, sizeof(line), "[%s%s] %-21.21s %s", kCells[i].tap ? "*" : " ", "", kCells[i].label, val);
        PrintRow(CELL_FIRST_ROW + i * CELL_ROWS, line);
    }

    if (sNotice) {
        snprintf(line, sizeof(line), "%s", sNotice);
        if (sPhase == 1 && sRecvTotal)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " %u%%", (unsigned)(sRecvDone * 100 / sRecvTotal));
        else if (sPhase == 2 && sCiaTotal)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " %u%%", (unsigned)(sCiaDone * 100 / sCiaTotal));
        else if (sPhase == 3) {
            u64 now = osGetTime(), at = sRelaunchAt;
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " %us", (unsigned)(now >= at ? 0 : (at - now + 999) / 1000));
        }
        PrintRow(29, line);
    }
    else
        PrintRow(29, "");
    fflush(stdout);
    // The console writes through the CPU cache; in GPU presentation mode nothing else flushes the screens
    // every frame (the CPU flip does), so the menu flushes what it painted.
    gfxFlushBuffers();
    {
        // And the bottom framebuffer by hand, whole: only the first rows of the menu reached the screen
        // with gfxFlushBuffers alone (the owner saw the menu cut after row 7).
        u16 w = 0, h = 0;
        u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
        if (fb)
            GSPGPU_FlushDataCache(fb, (u32)w * h * (gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGB565_OES ? 2 : 3));
    }
    sMenuDirty = false;
}

// Taps on the console rows (8 px each) act on the cell under the finger.
static void PollTouch()
{
    if (!(hidKeysDown() & KEY_TOUCH))
        return;
    touchPosition t;
    hidTouchRead(&t);
    // Each cell is a block of three text rows; only its first two react, so a stray touch between two
    // cells does nothing.
    const int row = t.py / 8 - CELL_FIRST_ROW;
    if (row >= 0 && row / CELL_ROWS < CELL_COUNT && row % CELL_ROWS < 2 && kCells[row / CELL_ROWS].tap) {
        kCells[row / CELL_ROWS].tap();
        sMenuDirty = true;
    }
}

void DevTools::GetStats(Stats *out)
{
    out->fps     = sLastFps;
    out->frameMs = sLastFrameMs;
    memcpy(out->stageMs, sLastStageMs, sizeof(out->stageMs));
    out->skipped = sLastSkipped;
    out->scene   = (int)sceneInfo.listPos;
}

const char *DevTools::FtpStatus() { return sStatus; }

void DevTools::SetGpuMode(bool on)
{
    swGpuPresent = swRecord = swGpuRender = on;
    sSecGpu = sSecCpu = sLastSecGpu = sLastSecCpu = sTotGpu = sTotCpu = sLogGpu = sLogCpu = 0;
    sLastWhy = NULL;
    memset(sWhy, 0, sizeof(sWhy));
    memset(sLogWhy, 0, sizeof(sLogWhy));
    Log("renderer: %s", on ? "GPU" : "CPU");
}

bool DevTools::GpuMode() { return swGpuPresent && swRecord && swGpuRender; }

static void CountWhy(WhyCount *table, const char *why)
{
    for (int i = 0; i < WHY_SLOTS; ++i) {
        if (table[i].why == why || !table[i].why) {
            table[i].why = why;
            ++table[i].count;
            return;
        }
    }
}

void DevTools::NoteFrame(const char *why)
{
    // Passes that drew nothing (loads, transitions: the last picture is shown again) are neither GPU nor CPU work.
    if (why && (!strcmp(why, "NOT RECORDED") || !strcmp(why, "EMPTY FRAME")))
        return;
    sLastFrameGpu = !why;
    if (!why) {
        ++sSecGpu, ++sTotGpu, ++sLogGpu;
        return;
    }
    ++sSecCpu, ++sTotCpu, ++sLogCpu;
    if (GpuMode()) {
        sLastWhy = why;
        CountWhy(sWhy, why);
        CountWhy(sLogWhy, why);
    }
}

// The `n` most frequent entries of a reason table, most frequent first.
static int TopWhy(const WhyCount *table, WhyCount *out, int n)
{
    int found = 0;
    bool used[WHY_SLOTS] = {};
    for (; found < n; ++found) {
        int best = -1;
        for (int i = 0; i < WHY_SLOTS && table[i].why; ++i)
            if (!used[i] && (best < 0 || table[i].count > table[best].count))
                best = i;
        if (best < 0)
            break;
        used[best] = true;
        out[found] = table[best];
    }
    return found;
}

void DevTools::GetRenderStats(RenderStats *out)
{
    out->gpuMode      = GpuMode();
    out->lastFrameGpu = sLastFrameGpu;
    out->secGpu       = sLastSecGpu;
    out->secCpu       = sLastSecCpu;
    out->totalGpu     = sTotGpu;
    out->totalCpu     = sTotCpu;
    out->lastWhy      = sLastWhy;
    WhyCount top[3] = {};
    TopWhy(sWhy, top, 3);
    for (int i = 0; i < 3; ++i)
        out->top[i].why = top[i].why, out->top[i].count = top[i].count;
}
void DevTools::Mark() { TapMark(); }

int DevTools::InstallNotice(char *title, unsigned cap, float *progress, unsigned *secondsLeft)
{
    return DevFtp::InstallNotice(title, cap, progress, secondsLeft);
}

// The switches share the port's file (port-settings.ini, Port::Save) with the player's options; dev-settings.ini is
// where they lived before (read when the port's file is not there yet).
static const char *kSettingsPath = "sdmc:/3ds/Sonic Mania 3DS/port-settings.ini";
static const char *kOldSettingsPath = "sdmc:/3ds/Sonic Mania 3DS/dev-settings.ini";

void DevTools::SaveSettings() { Port::Save(); }

void DevTools::SaveSwitches(void *file)
{
    fprintf((FILE *)file, "maxskip=%d\nflipcopy=%d\nsyncdraw=%d\nvblank=%d\ngpu3d=%d\nstereofar=%d\nforce3d=%d\nftp=%d\n", swMaxSkip, (int)swFlipCopy,
            (int)swSyncDraw, (int)swVblankWait, (int)swGpu3D, GpuRender::stereoFar, (int)swForce3D, (int)swFtp);
}

static void LoadSettings()
{
    FILE *f = fopen(kSettingsPath, "r");
    if (!f)
        f = fopen(kOldSettingsPath, "r");
    if (!f)
        return;
    char key[24];
    int v;
    while (fscanf(f, " %23[^=]=%d", key, &v) == 2) {
        if (!strcmp(key, "maxskip"))
            DevTools::swMaxSkip = v < 1 ? 1 : v > 4 ? 4 : v;
        else if (!strcmp(key, "flipcopy"))
            DevTools::swFlipCopy = v != 0;
        else if (!strcmp(key, "gpumode")) // the old "gpu" key (presentation alone) is ignored
            DevTools::SetGpuMode(v != 0);
        else if (!strcmp(key, "syncdraw"))
            DevTools::swSyncDraw = v != 0;
        else if (!strcmp(key, "vblank"))
            DevTools::swVblankWait = v != 0;
        else if (!strcmp(key, "gpu3d"))
            DevTools::swGpu3D = v != 0;
        else if (!strcmp(key, "force3d"))
            DevTools::swForce3D = v != 0;
        else if (!strcmp(key, "stereofar"))
            GpuRender::stereoFar = v < 0 ? 0 : v > 16 ? 16 : v;
        else if (!strcmp(key, "ftp"))
            DevTools::swFtp = v != 0;
    }
    fclose(f);
}

void DevTools::Init()
{
    LoadSettings();
    if (swFtp)
        StartServer();
    else
        snprintf(sStatus, sizeof(sStatus), "OFF");
    OpenCsv();
    BottomUi::Init(); // the graphical bottom screen; the text console menu stays as the fallback
    sSecondStart = sLastFrameTick = svcGetSystemTick();
    DrawStatus();
}

// A screen's back buffer as a raw file (tools/shot.sh, tools/capture.sh convert it): request 1 top screen, 2 bottom
// screen, 3 the top screen's right eye. The back buffer holds the frame before the one just shown.
static void WriteShot(int request, const char *path)
{
    const gfxScreen_t screen = request == 2 ? GFX_BOTTOM : GFX_TOP;
    u16 w = 0, h = 0;
    const u8 *fb = gfxGetFramebuffer(screen, request == 3 ? GFX_RIGHT : GFX_LEFT, &w, &h);
    FILE *f      = fopen(path, "wb");
    if (f && fb) {
        // Header: width, height (the framebuffer is column-major), bytes per pixel.
        const u32 head[3] = { w, h, (u32)(gfxGetScreenFormat(screen) == GSP_RGB565_OES ? 2 : 3) };
        fwrite(head, sizeof(head), 1, f);
        fwrite(fb, head[2], (size_t)w * h, f);
    }
    if (f)
        fclose(f);
}

// The CAPTURE button (DEBUG tab, dev builds): the owner's own capture of what is on screen now. The game freezes
// (FREEZE: the objects stop, the frame keeps being drawn) and debug/capture-NNN-* get the picture shown when it was
// tapped (both eyes: the back buffers one frame later), the one before it (-prev), the next frame's command list
// (-cmds.txt) and the renderer's state (-info.txt). Tapped again, the game carries on. tools/capture.sh [N] fetches
// and converts them.
static int sCaptureNumber = -1;
static volatile bool sCaptureTap;

void DevTools::TapCapture() { sCaptureTap = true; }

void DevTools::LoadScene(int category, int scene)
{
    if (category < 0 || category >= sceneInfo.categoryCount || scene < 0 || scene >= sceneInfo.listCategory[category].sceneCount)
        return;
    sPendingScene    = scene;
    sPendingCategory = category;
}
int DevTools::CaptureNumber() { return sCaptureNumber; }

static void Capture()
{
    static int sStep;
    int &sNumber = sCaptureNumber;
    char path[160];
    auto name = [&](const char *what) {
        snprintf(path, sizeof(path), "%s/capture-%03d-%s", kDir, sNumber, what);
        return path;
    };
    if (sStep == 1) {
        // One frame on: the back buffers hold the frame that was on screen when it was tapped.
        sStep = 0;
        WriteShot(1, name("top.raw"));
        WriteShot(3, name("right.raw"));
        static char cmdsPath[160];
        snprintf(cmdsPath, sizeof(cmdsPath), "%s/capture-%03d-cmds.txt", kDir, sNumber);
        GpuFrame::dumpPath    = cmdsPath;
        GpuFrame::dumpRequest = true;
        DevTools::Log("capture %03d saved", sNumber);
        return;
    }
    if (!sCaptureTap)
        return;
    sCaptureTap = false;
    if (DevTools::freeze) {
        DevTools::freeze = false;
        return;
    }
    DevTools::freeze = true;
    if (sNumber < 0) { // the first free number on the card
        mkdir(kDir, 0777);
        for (sNumber = 0; sNumber < 999; ++sNumber) {
            struct stat st;
            if (stat(name("info.txt"), &st) != 0)
                break;
        }
    }
    else
        ++sNumber;
    WriteShot(1, name("prev-top.raw"));
    WriteShot(3, name("prev-right.raw"));
    FILE *f = fopen(name("info.txt"), "w");
    if (f) {
        const GpuRender::Stats &g = GpuRender::GetStats();
        fprintf(f, "capture %03d: category %d scene %d (list %d), frame %u\n", sNumber, (int)sceneInfo.activeCategory,
                (int)(sceneInfo.listPos - sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart), (int)sceneInfo.listPos, sTotGpu + sTotCpu);
        fprintf(f, "renderer %s, last frame %s (why %s), stereo %d force3d %d slider %.2f\n", DevTools::GpuMode() ? "GPU" : "CPU",
                sLastWhy ? "CPU" : "GPU", sLastWhy ? sLastWhy : "-", (int)C3DPresent::Stereo(), (int)DevTools::swForce3D, osGet3DSliderState());
        fprintf(f, "refused: %s\nroto texture: %s\n", GpuRender::LastRefusal(), GpuRender::RotoTexWhy());
        fprintf(f, "quads %u batches %u faces %u soft layers %u roto quads %u, cmdbuf %u%% eyes dropped %u, atlas retries %u quad retries %u\n",
                g.quads, g.batches, g.faces, g.softLayers, g.rotoQuads, g.cmdBufPct, g.eyesDropped, g.atlasRetries, g.quadRetries);
        fprintf(f, "fps %.1f frame %.1f ms, heap used %u KB, linear free %u KB, vram free %u KB\n", sLastFps, sLastFrameMs,
                (unsigned)(mallinfo().uordblks >> 10), (unsigned)(linearSpaceFree() >> 10), (unsigned)(vramSpaceFree() >> 10));
        fprintf(f, "depths: %s\n", GpuRender::DepthSummary());
        fprintf(f, "times: frame %.1f update %.1f draw %.1f flip %.1f (pcopy %.1f pframe %.1f), gpu draw %.1f\n", sLastFrameMs,
                sLastStageMs[DevTools::STAGE_UPDATE], sLastStageMs[DevTools::STAGE_DRAW], sLastStageMs[DevTools::STAGE_FLIP],
                sLastStageMs[DevTools::STAGE_PCOPY], sLastStageMs[DevTools::STAGE_PFRAME], g.gpuDrawMs);
        {
            using namespace GpuFrame;
            fprintf(f, "emit: pre %.2f layer %.2f roto-spr %.2f sprite %.2f 3d %.2f faces %.2f rect %.2f other %.2f\n", g.preMs,
                    g.emitMs[CMD_LAYER], g.emitMs[CMD_ROTO], g.emitMs[CMD_SPRITE], g.emitMs[CMD_SCENE3D],
                    g.emitMs[CMD_FACES] + g.emitMs[CMD_BLENDED_FACES] + g.emitMs[CMD_FACE] + g.emitMs[CMD_BLENDED_FACE],
                    g.emitMs[CMD_RECT] + g.emitMs[CMD_FILL], g.emitMs[CMD_LINEBUF] + g.emitMs[CMD_PALETTE]);
        }
        GpuRender::DumpBatches(f);
        fclose(f);
    }
    sStep = 1;
}

// SET heapcheck 1 (debug): newlib's heap is one run of chunks from the start of the heap to the break, each with its
// size (and two flag bits) in the word before its data. Walked every frame: the first chunk whose size does not fit is
// logged with the chunk before it, which is likely the one written past its end (its size and first bytes say what it
// holds), and the words around the bad header (the data that overwrote it).
bool DevTools::heapCheck;
extern "C" {
extern char *fake_heap_start;
void *_sbrk_r(struct _reent *, ptrdiff_t);
}
static bool HeapWalk(const char *where)
{
    static bool reported;
    if (reported)
        return false;
    char *p   = (char *)(((uintptr_t)fake_heap_start + 7) & ~(uintptr_t)7);
    char *top = (char *)_sbrk_r(_REENT, 0);
    char *prev = NULL;
    unsigned prevSize = 0, n = 0;
    while (p + 8 <= top) {
        const unsigned size = *(unsigned *)(p + 4) & ~7u;
        if (size < 16 || p + size > top) {
            reported = true;
            const unsigned *w = (const unsigned *)p;
            DevTools::Log("heapcheck (%s): chunk %u at %p has size %08x (top %p); the one before: %p, %u bytes", where, n, p, *(unsigned *)(p + 4), top, prev,
                prevSize);
            DevTools::Log("heapcheck: header words %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            if (prev) {
                const unsigned *q = (const unsigned *)(prev + 8);
                DevTools::Log("heapcheck: before's data %08x %08x %08x %08x %08x %08x %08x %08x", q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
                const unsigned *e = (const unsigned *)(p - 32);
                DevTools::Log("heapcheck: before's last words %08x %08x %08x %08x %08x %08x %08x %08x", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
            }
            return false;
        }
        prev = p, prevSize = size;
        p += size;
        ++n;
    }
    return true;
}
void DevTools::ShotFromFile()
{
    static u64 last;
    const u64 now = svcGetSystemTick();
    if (now - last < SYSCLOCK_ARM11 / 2)
        return;
    last = now;
    char path[160];
    snprintf(path, sizeof(path), "%s/shot.txt", kDir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    const int c = fgetc(f);
    fclose(f);
    remove(path);
    snprintf(path, sizeof(path), "%s/shot-%s.raw", kDir, c == 'b' ? "bottom" : "top");
    WriteShot(c == 'b' ? 2 : 1, path);
}

int DevTools::AskFromFile()
{
    char path[160];
    snprintf(path, sizeof(path), "%s/ask.txt", kDir);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    const int c = fgetc(f);
    fclose(f);
    remove(path);
    if (c == 's' || c == 't') {
        snprintf(path, sizeof(path), "%s/shot-%s.raw", kDir, c == 's' ? "bottom" : "top");
        WriteShot(c == 's' ? 2 : 1, path);
        return -1;
    }
    return c == '1' ? 1 : c == '0' ? 0 : -1;
}

void DevTools::HeapCheck(const char *where)
{
    if (heapCheck)
        HeapWalk(where);
}

bool DevTools::FrameEnd()
{
    // Whether this frame ran frozen, before CAPTURE (below) can change it: the input is read for the frame that was
    const bool frozenFrame = freeze;
    HeapCheck("frame end");
    if (sShotRequest) {
        const int request = sShotRequest;
        sShotRequest      = 0;
        char path[160];
        snprintf(path, sizeof(path), "%s/shot-%s.raw", kDir, request == 2 ? "bottom" : request == 3 ? "right" : "top");
        WriteShot(request, path);
    }
    Capture();
    if (sModelCheck) {
        // SITE SET modelcheck 1: the models' VRAM against what was uploaded every 120 frames; the first damage goes to
        // the log with the scene and frame, and the VRAM map to debug/models-damage.txt (once per boot).
        static unsigned frames;
        static bool reported;
        if (!reported && ++frames % 120 == 0) {
            const unsigned bad = GpuRender::ModelDamage();
            if (bad) {
                reported = true;
                DevTools::Log("modelcheck: %u bytes of the 3D models' VRAM differ (scene %d/%d, frame %u): debug/models-damage.txt", bad,
                              (int)sceneInfo.activeCategory, (int)sceneInfo.listPos, sTotGpu + sTotCpu);
                char path[160];
                snprintf(path, sizeof(path), "%s/models-damage.txt", kDir);
                if (FILE *f = fopen(path, "w")) {
                    GpuRender::CheckModels(f);
                    fclose(f);
                }
            }
        }
    }
    if (sUfoTime) {
        // SITE SET ufotime 1: the Special Stage's countdown (UFO_Setup->rings, one a second, time over at 0) kept at
        // 50 or more, to test the floor at leisure.
        UfoSetupStatics *u = UfoSetup();
        if (u && u->rings < 50)
            u->rings = 50;
    }
    if (sGoto) {
        sGoto = false;
        objectEntityList[0].position.x = sGotoX;
        objectEntityList[0].position.y = sGotoY;
        objectEntityList[0].velocity.x = objectEntityList[0].velocity.y = 0;
    }
    if (sPendingScene >= 0) {
        const SceneListInfo *l   = &sceneInfo.listCategory[sPendingCategory];
        sceneInfo.activeCategory = (uint8)sPendingCategory;
        sceneInfo.listPos        = (uint16)(l->sceneOffsetStart + sPendingScene);
        sceneInfo.state          = ENGINESTATE_LOAD;
        if (sPendingHard) { // a recorded run: a full load, not a reload of the same scene
            ForceHardReset(true);
            sPendingHard = false;
        }
        DevTools::Log("scene request: category %d scene %d", sPendingCategory, sPendingScene);
        sPendingScene = sPendingCategory = -1;
    }
    if (sPendingGpuMode >= 0) {
        DevTools::SetGpuMode(sPendingGpuMode != 0);
        DevTools::SaveSettings();
        sPendingGpuMode = -1;
    }
    const uint64 now = svcGetSystemTick();
    const uint64 dt  = now - sLastFrameTick;
    sLastFrameTick   = now;
    sFrameTicksSum += dt;
    if (dt > sFrameTicksMax)
        sFrameTicksMax = dt;
    ++sFrames;

    const double toMs = 1000.0 / SYSCLOCK_ARM11;
    if (now - sSecondStart >= SYSCLOCK_ARM11) {
        const double secs = (double)(now - sSecondStart) / SYSCLOCK_ARM11;
        sLastFps          = (float)(sFrames / secs);
        sLastFrameMs      = (float)(sFrameTicksSum * toMs / sFrames);
        for (int i = 0; i < STAGE_COUNT; ++i)
            sLastStageMs[i] = (float)(sStageTicks[i] * toMs / sFrames);
        if (sCsv) {
            static unsigned second;
            fprintf(sCsv, "%u,%.2f,%.2f,%.2f", ++second, sLastFps, sLastFrameMs, sFrameTicksMax * toMs);
            for (int i = 0; i < STAGE_COUNT; ++i)
                fprintf(sCsv, ",%.2f", sLastStageMs[i]);
            fprintf(sCsv, ",%d,%u,%u\n", (int)sceneInfo.listPos, sSecGpu, sSecCpu);
            if (second % 5 == 0)
                fflush(sCsv);
        }
        sLastSecGpu = sSecGpu, sLastSecCpu = sSecCpu;
        sSecGpu = sSecCpu = 0;
        // In GPU mode, every 5 s with a CPU frame in it: how many and why, in log.txt.
        static unsigned logSeconds;
        if (++logSeconds >= 5) {
            logSeconds = 0;
            if (GpuMode() && sLogCpu) {
                WhyCount top[4] = {};
                const int n = TopWhy(sLogWhy, top, 4);
                char why[160];
                unsigned w = 0;
                for (int i = 0; i < n && w < sizeof(why); ++i)
                    w += (unsigned)snprintf(why + w, sizeof(why) - w, "%s%s %u", i ? ", " : "", top[i].why, top[i].count);
                Log("render (5 s, scene %d): gpu %u cpu %u | why: %s", (int)sceneInfo.listPos, sLogGpu, sLogCpu, n ? why : "-");
            }
            sLogGpu = sLogCpu = 0;
            memset(sLogWhy, 0, sizeof(sLogWhy));
        }
        sLastSkipped = skipCount;
        if (!BottomUi::Ready())
            DrawStatus();
        skipCount    = 0;
        memset(sStageTicks, 0, sizeof(sStageTicks));
        sFrameTicksSum = sFrameTicksMax = 0;
        sFrames                         = 0;
        sSecondStart                    = now;
    }
    else if (!BottomUi::Ready() && ((sPhase == 3 && (sFrames & 15) == 0) || sMenuDirty))
        DrawStatus();
    // Frozen, the game's ProcessInput (the only hidScanInput) does not run: read the touch screen here, or the
    // bottom screen (CAPTURE again) never sees a tap. By whether this frame was frozen: on the frame the second tap
    // unfreezes, a test of `freeze` skipped the read and the bottom screen saw the same tap as new (CAPTURE again,
    // frozen again, every tap).
    if (frozenFrame)
        hidScanInput();
    PortGame_Immortal(DevTools::immortal);
    if (BottomUi::Ready()) {
        DEV_STAGE(STAGE_BOTTOM);
        BottomUi::Update();
    }
    else
        PollTouch();

    if (sPhase == 3 && osGetTime() >= sRelaunchAt) {
        // aptExit() starts this title again once the process has exited (as HOME -> close).
        aptSetChainloaderToSelf();
        return true;
    }
    return false;
}

void DevTools::Shutdown()
{
    // The server thread polls without blocking and checks sRun between polls and transfer chunks, so it ends
    // within a nap. A thread still alive is left alone (freeing it under a socket call froze other ports).
    Log("exit: stopping the FTP server");
    sRun = false;
    if (sThread) {
        if (threadJoin(sThread, 3000000000ULL) == 0)
            threadFree(sThread);
        else
            Log("exit: the FTP thread did not end in 3 s, left running");
        sThread = NULL;
    }
    CloseSocket(&sListen);
    if (sCsv)
        fclose(sCsv), sCsv = NULL;
    Log("exit: dev tools stopped");
}

#endif // MANIA_DEV_TOOLS

#endif
