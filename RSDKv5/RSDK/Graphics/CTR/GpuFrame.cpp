#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"
#include "RSDK/Graphics/CTR/GpuFrameCmd.hpp"
#include "RSDK/Graphics/CTR/GpuRender.hpp"
#include "RSDK/Graphics/CTR/GpuScene3D.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <stdio.h>
#include <string.h>

#include "RSDK/Dev/CTR/DevTools.hpp"

using namespace RSDK;

volatile bool GpuFrame::dumpRequest;
const char *GpuFrame::dumpPath;
bool GpuFrame::lineBufDirty = true;
bool GpuFrame::active;
bool GpuFrame::pending;
unsigned GpuFrame::paletteVersion;
unsigned GpuFrame::contentVersion;

using namespace RSDK::GpuFrame;

namespace
{

// The Special Stage records thousands of faces a frame; the heap is tight (~51 MB used), so the buffers are as big
// as malloc allows, from the first size down (Alloc logs which one it got).
unsigned MAX_CMDS, ARENA_SIZE;

Cmd *sCmds;
uint8 *sArena;
unsigned sCount, sArenaUsed;
uint8 sLineBuf[SCREEN_YSIZE];
GpuFrame::Stats sStats;
int16 sEndClip[4];
bool sRecorded;         // this frame's draw lists were recorded (Begin got going)
const char *sHazardWhy; // the first hazard of this frame
const char *sLastWhy;   // why the last presented frame was not drawn by the GPU

bool Alloc()
{
    static bool failed;
    if (sCmds)
        return true;
    if (failed)
        return false;
    static const unsigned kSizes[][2] = { { 8192, 384 * 1024 }, { 4096, 256 * 1024 }, { 2048, 96 * 1024 } };
    for (const auto &sz : kSizes) {
        sCmds  = (Cmd *)malloc(sz[0] * sizeof(Cmd));
        sArena = (uint8 *)malloc(sz[1]);
        if (sCmds && sArena) {
            MAX_CMDS = sz[0], ARENA_SIZE = sz[1];
            DEV_LOG("gpuframe: %u commands, %u KB arena (%u KB)", MAX_CMDS, ARENA_SIZE >> 10, (unsigned)((MAX_CMDS * sizeof(Cmd) + ARENA_SIZE) >> 10));
            return true;
        }
        free(sCmds);
        free(sArena);
        sCmds  = NULL;
        sArena = NULL;
    }
    failed = true;
    DEV_LOG("gpuframe: no memory for the command buffers");
    return false;
}

// A new command with the screen state it needs; NULL when the buffers are full (the caller then falls back
// to drawing immediately for the rest of the frame).
Cmd *NewCmd(CmdType type, uint32 arenaBytes = 0)
{
    if (sCount >= MAX_CMDS || sArenaUsed + arenaBytes > ARENA_SIZE) {
        GpuFrame::Hazard("COMMAND LIMIT");
        return NULL;
    }
    Cmd *c = &sCmds[sCount++];
    memset(c, 0, sizeof(*c));
    c->type    = type;
    c->clip[0] = (int16)currentScreen->clipBound_X1;
    c->clip[1] = (int16)currentScreen->clipBound_Y1;
    c->clip[2] = (int16)currentScreen->clipBound_X2;
    c->clip[3] = (int16)currentScreen->clipBound_Y2;
    c->pos[0]    = currentScreen->position.x;
    c->pos[1]    = currentScreen->position.y;
    c->arena     = sArenaUsed;
    c->arenaSize = arenaBytes;
    sArenaUsed += (arenaBytes + 3) & ~3u;
    return c;
}

// The line buffer picks each screen line's palette bank (the water line); the game rewrites it while it
// draws, so a snapshot goes into the stream whenever it differs from the one the stream last set.
bool SyncLineBuffer()
{
    // Compared only after a write to it: a frame records thousands of commands (the Special Stage's faces).
    if (!lineBufDirty)
        return true;
    lineBufDirty = false;
    if (!memcmp(sLineBuf, gfxLineBuffer, sizeof(sLineBuf)))
        return true;
    Cmd *c = NewCmd(CMD_LINEBUF, sizeof(sLineBuf));
    if (!c)
        return false;
    memcpy(sArena + c->arena, gfxLineBuffer, sizeof(sLineBuf));
    memcpy(sLineBuf, gfxLineBuffer, sizeof(sLineBuf));
    return true;
}

Cmd *BeginCmd(CmdType type, uint32 arenaBytes = 0)
{
    if (!SyncLineBuffer())
        return NULL;
    return NewCmd(type, arenaBytes);
}

void SetClip(const int16 *clip)
{
    currentScreen->clipBound_X1 = clip[0];
    currentScreen->clipBound_Y1 = clip[1];
    currentScreen->clipBound_X2 = clip[2];
    currentScreen->clipBound_Y2 = clip[3];
}

} // namespace

const GpuFrame::Stats &GpuFrame::GetStats() { return sStats; }

void GpuFrame::Begin()
{
    sRecorded  = false;
    sHazardWhy = NULL;
    GpuScene3D::BeginFrame();
    if (!DEV_SW_RECORD || videoSettings.screenCount != 1 || !Alloc())
        return;
    sCount     = 0;
    sArenaUsed = 0;
    memcpy(sLineBuf, gfxLineBuffer, sizeof(sLineBuf));
    lineBufDirty = false;
    // The stream starts by setting the line buffer the frame starts with.
    Cmd *c = NewCmd(CMD_LINEBUF, sizeof(sLineBuf));
    if (!c)
        return;
    memcpy(sArena + c->arena, sLineBuf, sizeof(sLineBuf));
    active    = true;
    pending   = false;
    sRecorded = true;
}

void GpuFrame::End()
{
    if (!active)
        return;
    active = false;
    // The draw loop has already moved currentScreen on to the next screen: the 3DS has only screen 0.
    sEndClip[0] = (int16)screens[0].clipBound_X1;
    sEndClip[1] = (int16)screens[0].clipBound_Y1;
    sEndClip[2] = (int16)screens[0].clipBound_X2;
    sEndClip[3] = (int16)screens[0].clipBound_Y2;
    pending = sCount > 1;
}

void GpuFrame::Hazard(const char *why)
{
    if (!sHazardWhy)
        sHazardWhy = why;
    if (active) {
        sEndClip[0] = (int16)screens[0].clipBound_X1;
        sEndClip[1] = (int16)screens[0].clipBound_Y1;
        sEndClip[2] = (int16)screens[0].clipBound_X2;
        sEndClip[3] = (int16)screens[0].clipBound_Y2;
    }
    active  = false;
    pending = sCount > 1;
    Flush();
}

void GpuFrame::Flush()
{
    if (!pending)
        return;
    pending = false;
    active  = false;
    sStats.commands = sCount;
    ++sStats.replayed;

    // Replays only call the public draw functions, with `active` and `pending` off: they draw. They draw on
    // screen 0 whatever currentScreen says now (the draw loop moved it on; Present runs later).
    ScreenInfo *const keepScreen = currentScreen;
    currentScreen                = &screens[0];
    for (unsigned i = 0; i < sCount; ++i) {
        const Cmd &c = sCmds[i];
        const int32 *p = c.p;
        if (c.type == CMD_LINEBUF) {
            memcpy(gfxLineBuffer, sArena + c.arena, sizeof(sLineBuf));
            continue;
        }
        SetClip(c.clip);
        currentScreen->position.x = c.pos[0];
        currentScreen->position.y = c.pos[1];
        switch (c.type) {
            case CMD_FILL: FillScreen((uint32)p[0], p[1], p[2], p[3]); break;
            case CMD_LINE: DrawLine(p[0], p[1], p[2], p[3], (uint32)p[4], p[5], p[6], p[7]); break;
            case CMD_RECT: DrawRectangle(p[0], p[1], p[2], p[3], (uint32)p[4], p[5], p[6], p[7]); break;
            case CMD_CIRCLE: DrawCircle(p[0], p[1], p[2], (uint32)p[3], p[4], p[5], p[6]); break;
            case CMD_CIRCLE_OUTLINE: DrawCircleOutline(p[0], p[1], p[2], p[3], (uint32)p[4], p[5], p[6], p[7]); break;
            case CMD_FACE: DrawFace((Vector2 *)(sArena + c.arena), p[0], p[1], p[2], p[3], p[4], p[5]); break;
            case CMD_BLENDED_FACE: {
                Vector2 *v      = (Vector2 *)(sArena + c.arena);
                uint32 *colors = (uint32 *)(sArena + c.arena + p[0] * sizeof(Vector2));
                DrawBlendedFace(v, colors, p[0], p[1], p[2]);
                break;
            }
            case CMD_SCENE3D: GpuScene3D::Replay(sArena + c.arena); break;
            case CMD_FACES: {
                const uint8 *at = sArena + c.arena;
                for (int32 f = 0; f < p[0]; ++f) {
                    const int32 n = ((const int32 *)at)[0], rgb = ((const int32 *)at)[1];
                    DrawFace((Vector2 *)(at + 8), n, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, p[1], p[2]);
                    at += 8 + n * sizeof(Vector2);
                }
                break;
            }
            case CMD_BLENDED_FACES: {
                const uint8 *at = sArena + c.arena;
                for (int32 f = 0; f < p[0]; ++f) {
                    const int32 n = *(const int32 *)at;
                    DrawBlendedFace((Vector2 *)(at + 4), (uint32 *)(at + 4 + n * sizeof(Vector2)), n, p[1], p[2]);
                    at += 4 + n * (sizeof(Vector2) + sizeof(uint32));
                }
                break;
            }
            case CMD_SPRITE: DrawSpriteFlipped(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break;
            case CMD_ROTO: DrawSpriteRotozoom(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], (int16)p[11], p[12], p[13], p[14]); break;
            case CMD_LAYER: {
                TileLayer *layer = (TileLayer *)(uintptr_t)p[0];
                // The scanline table the layer's draw reads was filled by ProcessParallax just before the call;
                // later layers overwrote it, so the copy taken then goes back.
                const int y1 = c.clip[1], y2 = c.clip[3];
                if (y2 > y1)
                    memcpy(&scanlines[y1], sArena + c.arena, (size_t)(y2 - y1) * sizeof(ScanlineInfo));
                switch (c.kind) {
                    case LAYER_HSCROLL: DrawLayerHScroll(layer); break;
                    case LAYER_VSCROLL: DrawLayerVScroll(layer); break;
                    case LAYER_ROTOZOOM: DrawLayerRotozoom(layer); break;
                    case LAYER_BASIC: DrawLayerBasic(layer); break;
                    default: break;
                }
                break;
            }
            default: break;
        }
    }
    SetClip(sEndClip);
    currentScreen = keepScreen;
    sCount = sArenaUsed = 0;
}

static void DumpCommands()
{
    static const char *const kName[] = { "FILL", "LINE", "RECT", "CIRCLE", "CIRCLE_OUTLINE", "FACE", "BLENDED_FACE", "SPRITE", "ROTO", "LAYER", "LINEBUF", "FACES", "BLENDED_FACES", "SCENE3D" };
    FILE *f = fopen(dumpPath, "w");
    if (!f)
        return;
    fprintf(f, "# %u commands; clip x1 y1 x2 y2; screen pos; params\n", sCount);
    for (unsigned i = 0; i < sCount; ++i) {
        const Cmd &c = sCmds[i];
        fprintf(f, "%3u %-14s k%u clip %d %d %d %d pos %ld %ld |", i, c.type < sizeof(kName) / sizeof(kName[0]) ? kName[c.type] : "?", c.kind, c.clip[0], c.clip[1],
                c.clip[2], c.clip[3], (long)c.pos[0], (long)c.pos[1]);
        for (int k = 0; k < 15; ++k)
            fprintf(f, " %ld", (long)c.p[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}

bool GpuFrame::Present()
{
    if (dumpRequest && pending) {
        dumpRequest = false;
        DumpCommands();
    }
    const bool gpuMode = DEV_SW_GPUPRESENT && DEV_SW_GPURENDER;
    if (!gpuMode)
        sLastWhy = "CPU MODE";
    else if (sHazardWhy)
        sLastWhy = sHazardWhy; // part of the frame was drawn by the CPU already
    else if (!sRecorded)
        sLastWhy = "NOT RECORDED";
    else if (!pending)
        sLastWhy = "EMPTY FRAME";
    else {
        const Stream stream = { sCmds, sCount, sArena };
        if (GpuRender::Render(stream)) {
            pending = false;
            sCount = sArenaUsed = 0;
            ++sStats.gpuFrames;
            sLastWhy = NULL;
            return true; // the frame is on screen already
        }
        sLastWhy = GpuRender::ReasonName();
        if (!sLastWhy)
            sLastWhy = "REFUSED";
    }
    Flush();
    return false;
}

const char *GpuFrame::LastWhy() { return sLastWhy; }

void GpuFrame::InvalidateContent() { ++contentVersion; }

uint32 GpuFrame::tileDirty[TILE_COUNT / 32];
bool GpuFrame::tileDirtyAny;

void GpuFrame::TilesChanged(unsigned first, unsigned count)
{
    for (unsigned t = first; t < first + count && t < TILE_COUNT; ++t)
        tileDirty[t >> 5] |= 1u << (t & 31);
    tileDirtyAny = true;
}

// ---------------------------------------------------------------- recording

bool GpuFrame::RecFill(uint32 color, int32 aR, int32 aG, int32 aB)
{
    Cmd *c = BeginCmd(CMD_FILL);
    if (!c)
        return false;
    c->p[0] = (int32)color, c->p[1] = aR, c->p[2] = aG, c->p[3] = aB;
    return true;
}

bool GpuFrame::RecLine(int32 x1, int32 y1, int32 x2, int32 y2, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = BeginCmd(CMD_LINE);
    if (!c)
        return false;
    c->p[0] = x1, c->p[1] = y1, c->p[2] = x2, c->p[3] = y2, c->p[4] = (int32)color, c->p[5] = alpha, c->p[6] = ink, c->p[7] = sr;
    return true;
}

bool GpuFrame::RecRect(int32 x, int32 y, int32 w, int32 h, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = BeginCmd(CMD_RECT);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = w, c->p[3] = h, c->p[4] = (int32)color, c->p[5] = alpha, c->p[6] = ink, c->p[7] = sr;
    return true;
}

bool GpuFrame::RecCircle(int32 x, int32 y, int32 r, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = BeginCmd(CMD_CIRCLE);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = r, c->p[3] = (int32)color, c->p[4] = alpha, c->p[5] = ink, c->p[6] = sr;
    return true;
}

bool GpuFrame::RecCircleOutline(int32 x, int32 y, int32 ri, int32 ro, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = BeginCmd(CMD_CIRCLE_OUTLINE);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = ri, c->p[3] = ro, c->p[4] = (int32)color, c->p[5] = alpha, c->p[6] = ink, c->p[7] = sr;
    return true;
}

// Arena room for one more face in a run of `type`: appended to the last command when it is a run of the same kind
// with the same state and its data ends the arena, else a new run. NULL when the buffers are full.
static uint8 *FaceRecord(CmdType type, uint32 bytes, int32 alpha, int32 ink)
{
    if (!SyncLineBuffer())
        return NULL;
    Cmd *last = sCount ? &sCmds[sCount - 1] : NULL;
    if (last && last->type == type && last->p[1] == alpha && last->p[2] == ink && last->arena + last->arenaSize == sArenaUsed
        && last->clip[0] == currentScreen->clipBound_X1 && last->clip[1] == currentScreen->clipBound_Y1 && last->clip[2] == currentScreen->clipBound_X2
        && last->clip[3] == currentScreen->clipBound_Y2 && last->pos[0] == currentScreen->position.x && last->pos[1] == currentScreen->position.y) {
        if (sArenaUsed + bytes > ARENA_SIZE) {
            GpuFrame::Hazard("ARENA FULL");
            return NULL;
        }
        uint8 *at = sArena + sArenaUsed;
        sArenaUsed += bytes;
        last->arenaSize += bytes;
        ++last->p[0];
        return at;
    }
    Cmd *c = NewCmd(type, bytes);
    if (!c)
        return NULL;
    c->p[0] = 1, c->p[1] = alpha, c->p[2] = ink;
    return sArena + c->arena;
}

bool GpuFrame::RecFace(Vector2 *vertices, int32 n, int32 r, int32 g, int32 b, int32 alpha, int32 ink)
{
    if (n < 0 || n > 64)
        return false; // not recorded: the caller draws it, after a flush
    uint8 *at = FaceRecord(CMD_FACES, 8 + (uint32)n * sizeof(Vector2), alpha, ink);
    if (!at)
        return false;
    int32 *head = (int32 *)at;
    head[0]     = n;
    head[1]     = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    memcpy(at + 8, vertices, (size_t)n * sizeof(Vector2));
    return true;
}

bool GpuFrame::RecBlendedFace(Vector2 *vertices, uint32 *colors, int32 n, int32 alpha, int32 ink)
{
    if (n < 0 || n > 64)
        return false;
    uint8 *at = FaceRecord(CMD_BLENDED_FACES, 4 + (uint32)n * (sizeof(Vector2) + sizeof(uint32)), alpha, ink);
    if (!at)
        return false;
    *(int32 *)at = n;
    memcpy(at + 4, vertices, (size_t)n * sizeof(Vector2));
    memcpy(at + 4 + n * sizeof(Vector2), colors, (size_t)n * sizeof(uint32));
    return true;
}

bool GpuFrame::RecSprite(int32 x, int32 y, int32 w, int32 h, int32 sprX, int32 sprY, int32 dir, int32 ink, int32 alpha, int32 sheet)
{
    Cmd *c = BeginCmd(CMD_SPRITE);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = w, c->p[3] = h, c->p[4] = sprX, c->p[5] = sprY, c->p[6] = dir, c->p[7] = ink, c->p[8] = alpha, c->p[9] = sheet;
    return true;
}

bool GpuFrame::RecRoto(int32 x, int32 y, int32 pivotX, int32 pivotY, int32 w, int32 h, int32 sprX, int32 sprY, int32 scaleX, int32 scaleY, int32 dir,
                       int16 rot, int32 ink, int32 alpha, int32 sheet)
{
    Cmd *c = BeginCmd(CMD_ROTO);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = pivotX, c->p[3] = pivotY, c->p[4] = w, c->p[5] = h, c->p[6] = sprX, c->p[7] = sprY, c->p[8] = scaleX;
    c->p[9] = scaleY, c->p[10] = dir, c->p[11] = rot, c->p[12] = ink, c->p[13] = alpha, c->p[14] = sheet;
    return true;
}

uint8 *GpuFrame::RecScene3D(uint32 bytes)
{
    Cmd *c = BeginCmd(CMD_SCENE3D, bytes);
    return c ? sArena + c->arena : NULL;
}

bool GpuFrame::RecLayer(TileLayer *layer, int kind)
{
    const int y1 = currentScreen->clipBound_Y1, y2 = currentScreen->clipBound_Y2;
    const unsigned lines = y2 > y1 ? (unsigned)(y2 - y1) : 0;
    Cmd *c = BeginCmd(CMD_LAYER, lines * (unsigned)sizeof(ScanlineInfo));
    if (!c)
        return false;
    c->kind = (uint8)kind;
    c->p[0] = (int32)(uintptr_t)layer;
    if (lines)
        memcpy(sArena + c->arena, &scanlines[y1], lines * sizeof(ScanlineInfo));
    return true;
}

#endif
