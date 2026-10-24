#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/Upstream.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"
#include "RSDK/Dev/CTR/PortText.hpp"

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

#endif
