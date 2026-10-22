#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/DevTools.hpp"

#if RETRO_PLATFORM == RETRO_3DS && MANIA_DEV_TOOLS

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

// Own title ID (3ds/SonicMania.rsf: UniqueId 0x5A230). Installs of any other CIA are refused.
static const u64 TITLE_ID = 0x000400000005A230ULL;

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
    CIA_IO_SIZE   = 128 * 1024,
    RELAUNCH_MS   = 10000,
};

// ---------------------------------------------------------------- state

static Thread sThread;
static volatile bool sRun;
static int sListen = -1;
static uint32_t sIp;
static char *sIo;
static char sStatus[48] = "STOPPED";
static void *sSocBuffer;

// What the bottom screen says about an install in progress.
static const char *volatile sNotice;
static volatile int sPhase; // 1 receiving, 2 installing, 3 restart countdown
static volatile unsigned long long sAllo, sRecvDone, sRecvTotal, sCiaDone, sCiaTotal;
static volatile unsigned long long sRelaunchAt; // osGetTime() the countdown ends at

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
    unsigned char *data = (unsigned char *)malloc(CIA_IO_SIZE);
    snprintf(error, errorCap, "out of memory");
    if (!data)
        goto done;
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
    free(data);
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

static void CmdList(Session *s, const char *arg, int kind /* 0 LIST, 1 NLST, 2 MLSD */)
{
    if (arg && arg[0] == '-') {
        const char *space = strchr(arg, ' ');
        arg               = space ? space + 1 : "";
    }
    char vpath[FTP_PATH_MAX], rpath[FTP_PATH_MAX + 8];
    ResolvePath(s, arg, vpath, sizeof(vpath));
    RealPath(vpath, rpath, sizeof(rpath));
    DIR *dir = opendir(rpath);
    if (!dir) {
        ClosePasv(s);
        Reply(s, "550 No such directory.");
        return;
    }
    Reply(s, "150 Listing.");
    int data = AcceptData(s);
    if (data < 0) {
        closedir(dir);
        Reply(s, "425 No data connection.");
        return;
    }
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dir)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char line[FTP_PATH_MAX + 96];
        if (kind == 1) {
            snprintf(line, sizeof(line), "%s\r\n", e->d_name);
        }
        else {
            char full[FTP_PATH_MAX * 2];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", rpath, e->d_name);
            if (stat(full, &st) != 0)
                memset(&st, 0, sizeof(st));
            if (e->d_type == DT_DIR)
                st.st_mode = S_IFDIR;
            FormatListLine(line, sizeof(line), e->d_name, &st, kind == 2);
        }
        ok = SendAll(data, line, strlen(line));
    }
    closedir(dir);
    CloseSocket(&data);
    Reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void CmdRetr(Session *s, const char *arg)
{
    char vpath[FTP_PATH_MAX], rpath[FTP_PATH_MAX + 8];
    ResolvePath(s, arg, vpath, sizeof(vpath));
    RealPath(vpath, rpath, sizeof(rpath));
    FILE *f = fopen(rpath, "rb");
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
    FILE *f = fopen(rpath, append ? "ab" : "wb");
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
            if (stat(rpath, &st) != 0 || S_ISDIR(st.st_mode))
                Reply(s, "550 No such file.");
            else if (line[0] == 'S')
                Reply(s, "213 %llu", (unsigned long long)st.st_size);
            else
                Reply(s, "213 20260101000000");
        }
        else if (!strcmp(line, "DELE") || !strcmp(line, "RMD") || !strcmp(line, "XRMD") || !strcmp(line, "MKD") || !strcmp(line, "XMKD")) {
            ResolvePath(s, arg, vpath, sizeof(vpath));
            RealPath(vpath, rpath, sizeof(rpath));
            int r = line[0] == 'D' ? remove(rpath) : strstr(line, "RMD") ? rmdir(rpath) : mkdir(rpath, 0777);
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
            Reply(s, s->rnfr[0] && rename(from, rpath) == 0 ? "250 Renamed." : "550 Rename failed.");
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
            else
                Reply(s, "504 Unknown SITE command.");
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
    if (R_FAILED(acInit())) {
        snprintf(sStatus, sizeof(sStatus), "NO NETWORK SERVICE");
        return false;
    }
    u32 wifi = 0;
    ACU_GetWifiStatus(&wifi);
    sSocBuffer = memalign(0x1000, 1024 * 1024);
    if (!wifi) {
        snprintf(sStatus, sizeof(sStatus), "NO WI-FI");
        return false;
    }
    if (!sSocBuffer || R_FAILED(socInit((u32 *)sSocBuffer, 1024 * 1024))) {
        snprintf(sStatus, sizeof(sStatus), "NO NETWORK SERVICE");
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
        return false;
    }
    sIp  = (uint32_t)gethostid();
    sRun = true;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
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

// ---------------------------------------------------------------- perf timing

static const char *kStageName[DevTools::STAGE_COUNT] = { "update", "draw", "flip" };
static const char *kDir                              = "sdmc:/3ds/Sonic Mania 3DS/debug";

static uint64 sStageTicks[DevTools::STAGE_COUNT];
static uint64 sFrameTicksSum, sFrameTicksMax, sLastFrameTick, sSecondStart;
static unsigned sFrames;
static FILE *sCsv;
static float sLastFps, sLastFrameMs, sLastStageMs[DevTools::STAGE_COUNT];

DevTools::StageScope::StageScope(Stage s) : stage(s), start(svcGetSystemTick()) {}
DevTools::StageScope::~StageScope() { StageAdd(stage, svcGetSystemTick() - start); }

void DevTools::StageAdd(Stage stage, uint64 ticks) { sStageTicks[stage] += ticks; }

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
                fprintf(sCsv, "second,fps,frame_ms_avg,frame_ms_max,update_ms,draw_ms,flip_ms,scene\n");
        }
    }
}

static void DrawStatus()
{
    printf("\x1b[2J\x1b[1;1H");
    printf("SONIC MANIA 3DS  dev tools\n\n");
    printf("FTP  %s\n", sStatus);
    {
        extern u32 __ctru_heap_size;
        struct mallinfo mi = mallinfo();
        printf("heap %u/%u KB  linear free %u KB\n\n", (unsigned)(mi.uordblks >> 10), (unsigned)(__ctru_heap_size >> 10),
               (unsigned)(linearSpaceFree() >> 10));
    }
    printf("%.1f fps   frame %.1f ms\n", sLastFps, sLastFrameMs);
    for (int i = 0; i < DevTools::STAGE_COUNT; ++i)
        printf("  %-7s %5.1f ms\n", kStageName[i], sLastStageMs[i]);
    if (sNotice) {
        printf("\n%s", sNotice);
        if (sPhase == 1 && sRecvTotal)
            printf(" %u%%", (unsigned)(sRecvDone * 100 / sRecvTotal));
        else if (sPhase == 2 && sCiaTotal)
            printf(" %u%%", (unsigned)(sCiaDone * 100 / sCiaTotal));
        else if (sPhase == 3) {
            u64 now = osGetTime(), at = sRelaunchAt;
            printf(" %us", (unsigned)(now >= at ? 0 : (at - now + 999) / 1000));
        }
        printf("\n");
    }
    fflush(stdout);
}

void DevTools::Init()
{
    StartServer();
    OpenCsv();
    sSecondStart = sLastFrameTick = svcGetSystemTick();
    DrawStatus();
}

bool DevTools::FrameEnd()
{
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
            fprintf(sCsv, "%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d\n", ++second, sLastFps, sLastFrameMs, sFrameTicksMax * toMs, sLastStageMs[0],
                    sLastStageMs[1], sLastStageMs[2], (int)sceneInfo.listPos);
            fflush(sCsv);
        }
        DrawStatus();
        memset(sStageTicks, 0, sizeof(sStageTicks));
        sFrameTicksSum = sFrameTicksMax = 0;
        sFrames                         = 0;
        sSecondStart                    = now;
    }
    else if (sPhase == 3 && (sFrames & 15) == 0)
        DrawStatus();

    if (sPhase == 3 && osGetTime() >= sRelaunchAt) {
        // aptExit() starts this title again once the process has exited (as HOME -> close).
        aptSetChainloaderToSelf();
        return true;
    }
    return false;
}

void DevTools::Shutdown()
{
    sRun = false;
    if (sCsv)
        fclose(sCsv), sCsv = NULL;
}

#endif
