#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/Upstream.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"
#include "RSDK/Dev/CTR/PortText.hpp"
#include "RSDK/Dev/CTR/DspFirm.h"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

using namespace RSDK;

static bool IsFile(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool Port::UpstreamFallback(const char *path, const char *name, char *out, unsigned cap)
{
    snprintf(out, cap, "%s", path);
    if (IsFile(path))
        return true;
    char alt[0x100];
    snprintf(alt, sizeof(alt), "%s%s", UPSTREAM_DIR, name);
    if (!IsFile(alt))
        return false;
    snprintf(out, cap, "%s", alt);
    return true;
}

// ---------------------------------------------------------------- missing game data (#14)

static bool HasPackExtension(const char *name)
{
    for (const char *p = name; *p; ++p)
        if (*p == '.' && !strncasecmp(p, ".rsdk", 5))
            return true;
    return false;
}

// A file with ".rsdk" in its name other than Data.rsdk (archive copies are named "Data Mania Plus.rsdk"; Windows hides
// the ".txt" of "Data.rsdk.txt"): its name in `out`, cut to fit the question box.
static bool FindOtherPack(const char *dir, char *out, unsigned cap)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !HasPackExtension(e->d_name) || !strcasecmp(e->d_name, "Data.rsdk"))
            continue;
        char path[0x200];
        snprintf(path, sizeof(path), "%s%s", dir, e->d_name);
        found = IsFile(path);
        if (found && strlen(e->d_name) > 28)
            snprintf(out, cap, "%.24s...", e->d_name);
        else if (found)
            snprintf(out, cap, "%s", e->d_name);
    }
    closedir(d);
    return found;
}

// A data pack that is whole: the signature, a file table that fits, and every file inside the file's size. A copy that
// was cut short (a full SD card, a pulled card) keeps its header and passed a check of the signature alone, then the
// game crashed reading files past its end (#14: a Data.rsdk shown as 0 KB).
static bool IsDataPack(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = false;
    uint8 head[8];
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > (long)sizeof(head) && fread(head, 1, sizeof(head), f) == sizeof(head) && !memcmp(head, "RSDK", 4)) {
        const unsigned count = head[6] | (head[7] << 8); // after the signature, 'v' and the version
        ok                   = count > 0 && count <= DATAFILE_COUNT && (long)(sizeof(head) + 24 * count) <= size;
        for (unsigned i = 0; ok && i < count; ++i) {
            uint8 e[24]; // 16 bytes of hash, the offset, the size (its top bit: encrypted)
            if (fread(e, 1, sizeof(e), f) != sizeof(e)) {
                ok = false;
                break;
            }
            const uint32 offset = e[16] | (e[17] << 8) | (e[18] << 16) | ((uint32)e[19] << 24);
            const uint32 length = (e[20] | (e[21] << 8) | (e[22] << 16) | ((uint32)e[23] << 24)) & 0x7FFFFFFF;
            ok                  = (uint64_t)offset + length <= (uint64_t)size;
        }
    }
    fclose(f);
    return ok;
}

bool Port::MissingDataStop()
{
    char ours[0x200], path[0x200];
    snprintf(ours, sizeof(ours), "%sData.romfs", SKU::userFileDir);
    if (IsFile(ours))
        return false; // MountRomFS takes it before any pack
    snprintf(ours, sizeof(ours), "%sData.rsdk", SKU::userFileDir);
    const bool found = UpstreamFallback(ours, "Data.rsdk", path, sizeof(path));
    if (found && IsDataPack(path))
        return false;

    const char *title = Tr("GAME DATA NOT FOUND");
    char first[0x280];
    snprintf(first, sizeof(first), "%s", Tr("COPY DATA.RSDK FROM YOUR SONIC MANIA (PC) TO THE SD CARD:"));
    char other[0x40];
    if (found) {
        title = Tr("GAME DATA NOT VALID");
        snprintf(first, sizeof(first), "%s", Tr("THE DATA.RSDK ON THE SD CARD IS NOT A SONIC MANIA DATA FILE. COPY IT AGAIN:"));
    }
    else if (FindOtherPack(SKU::userFileDir, other, sizeof(other)) || FindOtherPack(UPSTREAM_DIR, other, sizeof(other))) {
        snprintf(first, sizeof(first), Tr("THE SD CARD HAS %s. RENAME AND MOVE IT TO:"), other);
    }
    DEV_LOG("game data: %s (%s: %s)", found ? "not a data pack" : "missing", path, first);
    const char *where = "/3DS/SONIC MANIA 3DS/DATA.RSDK";
    const char *lines[3] = { first, where, Tr("THEN OPEN THE GAME AGAIN.") };
    BottomUi::Ask(title, lines, 3, Tr("EXIT"), NULL, where);
    return true;
}

// ---------------------------------------------------------------- save import (../zelda-tmc-3ds upstream_import_3ds.c)

static const char *kMarker = "save-import.txt";
enum { MAX_SAVES = 64, NAME_SIZE = 128 };
static char sNames[MAX_SAVES][NAME_SIZE];

// The game's user files in `dir`: SaveData.bin, Options.bin, TimeAttackDB.bin, ReplayDB.bin and the Replay_*.bin.
static int ListSaves(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < MAX_SAVES) {
        const size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || n >= NAME_SIZE || strcasecmp(e->d_name + n - 4, ".bin"))
            continue;
        char path[0x200];
        snprintf(path, sizeof(path), "%s%s", dir, e->d_name);
        if (IsFile(path))
            snprintf(sNames[count++], NAME_SIZE, "%s", e->d_name);
    }
    closedir(d);
    return count;
}

// Through a temporary name, so an interrupted copy never leaves a short file under the real one; ours is kept.
static bool CopySave(const char *name)
{
    char from[0x200], to[0x200], temp[0x210];
    snprintf(from, sizeof(from), "%s%s", UPSTREAM_DIR, name);
    snprintf(to, sizeof(to), "%s%s", SKU::userFileDir, name);
    if (IsFile(to))
        return true;
    snprintf(temp, sizeof(temp), "%s.part", to);
    FILE *in = fopen(from, "rb");
    if (!in)
        return false;
    FILE *out = fopen(temp, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    static char buffer[16384];
    bool ok = true;
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n) {
            ok = false;
            break;
        }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    if (ok && rename(temp, to) != 0)
        ok = false;
    if (!ok)
        remove(temp);
    return ok;
}

static void WriteMarker(const char *answer)
{
    char path[0x200];
    snprintf(path, sizeof(path), "%s%s", SKU::userFileDir, kMarker);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "%s\n", answer);
    fclose(f);
}

void Port::OfferSaveImport()
{
    char path[0x200];
    snprintf(path, sizeof(path), "%s%s", SKU::userFileDir, kMarker);
    if (IsFile(path) || !BottomUi::Ready())
        return;
    snprintf(path, sizeof(path), "%s%s", SKU::userFileDir, "SaveData.bin");
    if (IsFile(path)) // saves of our own already: nothing to offer, ever
        return;
    const int count = ListSaves(UPSTREAM_DIR);
    if (!count)
        return;

    const char *lines[] = { Tr("SAVES OF THE ORIGINAL 3DS PORT FOUND"), Tr("IN /3DS/SONICMANIA/. COPY THEM TO THIS APP?"),
                            Tr("THE ORIGINAL APP KEEPS ITS OWN.") };
    int answer = BottomUi::Ask(Tr("IMPORT SAVES"), lines, 3, Tr("COPY"), Tr("SKIP"));
    if (answer < 0)
        return; // closed from HOME: ask again next time
    if (!answer) {
        WriteMarker("skipped");
        return;
    }
    mkdir("/3ds", 0777);
    mkdir(SKU::userFileDir, 0777);
    bool ok = true;
    for (int i = 0; i < count; ++i)
        ok = CopySave(sNames[i]) && ok;
    WriteMarker(ok ? "copied" : "failed");
    DEV_LOG("save import: %d files, %s", count, ok ? "copied" : "FAILED");
    BottomUi::Ask(Tr(ok ? "SAVES COPIED" : "COPY FAILED"), NULL, 0, "OK", NULL);
}

// ---------------------------------------------------------------- DSP firmware (../sm-3ds dsp_firm_ui.c)

extern Result ctrNdspRc;

void Port::EnsureDspFirmware()
{
    if (R_SUCCEEDED(ctrNdspRc) || !BottomUi::Ready())
        return;
    const DspFirmStatus st = DspFirm_Check(DSPFIRM_PATH);
    DEV_LOG("dsp firmware: ndspInit %08lX, file %s", (unsigned long)ctrNdspRc,
            st == DSPFIRM_OK ? "valid" : st == DSPFIRM_MISSING ? "missing" : "damaged");
    if (st == DSPFIRM_OK) // something else failed: nothing to offer
        return;

    const char *lines[] = { Tr(st == DSPFIRM_MISSING ? "THE DSP FIRMWARE IS NOT ON THE SD CARD." : "THE DSP FIRMWARE ON THE SD CARD IS DAMAGED."),
                            Tr("WITHOUT IT THE GAME RUNS, BUT SILENT. TO PLAY WITH SOUND, EXTRACT IT NOW.") };
    if (BottomUi::Ask(Tr("SOUND FIRMWARE NEEDED"), lines, 2, Tr("EXTRACT"), Tr("NO SOUND")) != 1)
        return;

    int32_t detail = 0;
    const DspFirmExtractResult r = DspFirm_Extract(DSPFIRM_PATH, &detail);
    DEV_LOG("dsp firmware: extraction %d (%s), detail %08lX | %s", (int)r, DspFirm_ExtractText(r), (unsigned long)detail, g_dspfirm_trace);
    if (r == DSPFIRM_EX_OK && AudioDevice::RetryDsp()) {
        BottomUi::Ask(Tr("DONE. THE SOUND WILL WORK."), NULL, 0, "OK", NULL);
        return;
    }
    char code[48];
    snprintf(code, sizeof(code), "ERROR %d / %08lX", (int)r, (unsigned long)detail);
    const char *failed[] = { code };
    BottomUi::Ask(Tr("COULD NOT EXTRACT IT."), failed, 1, "OK", NULL);
}

#endif
