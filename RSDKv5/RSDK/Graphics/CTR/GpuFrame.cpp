#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <string.h>

#include "RSDK/Dev/CTR/DevTools.hpp"

using namespace RSDK;

bool GpuFrame::active;
bool GpuFrame::pending;

namespace
{

enum CmdType : uint8 {
    CMD_FILL,
    CMD_LINE,
    CMD_RECT,
    CMD_CIRCLE,
    CMD_CIRCLE_OUTLINE,
    CMD_FACE,
    CMD_BLENDED_FACE,
    CMD_SPRITE,
    CMD_ROTO,
    CMD_LAYER,
    CMD_LINEBUF, // gfxLineBuffer changed: the new one is in the arena
};

struct Cmd {
    uint8 type;
    uint8 kind;       // layer type
    int16 clip[4];    // clipBound_X1, Y1, X2, Y2 when the call was made
    int32 p[15];      // parameters, per type
    uint32 arena;     // offset of the variable-size data (vertices, colours, scanlines, line buffer)
    uint32 arenaSize;
};

enum { MAX_CMDS = 2048, ARENA_SIZE = 96 * 1024 };

Cmd *sCmds;
uint8 *sArena;
unsigned sCount, sArenaUsed;
uint8 sLineBuf[SCREEN_YSIZE];
GpuFrame::Stats sStats;
int16 sEndClip[4];

bool Alloc()
{
    if (sCmds)
        return true;
    sCmds  = (Cmd *)malloc(MAX_CMDS * sizeof(Cmd));
    sArena = (uint8 *)malloc(ARENA_SIZE);
    if (!sCmds || !sArena) {
        free(sCmds);
        free(sArena);
        sCmds = NULL;
        sArena = NULL;
        return false;
    }
    return true;
}

// A new command with the screen state it needs; NULL when the buffers are full (the caller then falls back
// to drawing immediately for the rest of the frame).
Cmd *NewCmd(CmdType type, uint32 arenaBytes = 0)
{
    if (sCount >= MAX_CMDS || sArenaUsed + arenaBytes > ARENA_SIZE) {
        GpuFrame::Hazard();
        return NULL;
    }
    Cmd *c = &sCmds[sCount++];
    memset(c, 0, sizeof(*c));
    c->type    = type;
    c->clip[0] = (int16)currentScreen->clipBound_X1;
    c->clip[1] = (int16)currentScreen->clipBound_Y1;
    c->clip[2] = (int16)currentScreen->clipBound_X2;
    c->clip[3] = (int16)currentScreen->clipBound_Y2;
    c->arena     = sArenaUsed;
    c->arenaSize = arenaBytes;
    sArenaUsed += (arenaBytes + 3) & ~3u;
    return c;
}

// The line buffer picks each screen line's palette bank (the water line); the game rewrites it while it
// draws, so a snapshot goes into the stream whenever it differs from the one the stream last set.
bool SyncLineBuffer()
{
    if (!memcmp(sLineBuf, gfxLineBuffer, sizeof(sLineBuf)))
        return true;
    Cmd *c = NewCmd(CMD_LINEBUF, sizeof(sLineBuf));
    if (!c)
        return false;
    memcpy(sArena + c->arena, gfxLineBuffer, sizeof(sLineBuf));
    memcpy(sLineBuf, gfxLineBuffer, sizeof(sLineBuf));
    return true;
}

Cmd *Begin(CmdType type, uint32 arenaBytes = 0)
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
    if (!DEV_SW_RECORD || videoSettings.screenCount != 1 || !Alloc())
        return;
    sCount     = 0;
    sArenaUsed = 0;
    memcpy(sLineBuf, gfxLineBuffer, sizeof(sLineBuf));
    // The stream starts by setting the line buffer the frame starts with.
    Cmd *c = NewCmd(CMD_LINEBUF, sizeof(sLineBuf));
    if (!c)
        return;
    memcpy(sArena + c->arena, sLineBuf, sizeof(sLineBuf));
    active  = true;
    pending = false;
}

void GpuFrame::End()
{
    if (!active)
        return;
    active = false;
    sEndClip[0] = (int16)currentScreen->clipBound_X1;
    sEndClip[1] = (int16)currentScreen->clipBound_Y1;
    sEndClip[2] = (int16)currentScreen->clipBound_X2;
    sEndClip[3] = (int16)currentScreen->clipBound_Y2;
    pending = sCount > 1;
}

void GpuFrame::Hazard()
{
    if (active) {
        sEndClip[0] = (int16)currentScreen->clipBound_X1;
        sEndClip[1] = (int16)currentScreen->clipBound_Y1;
        sEndClip[2] = (int16)currentScreen->clipBound_X2;
        sEndClip[3] = (int16)currentScreen->clipBound_Y2;
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

    // Replays only call the public draw functions, with `active` and `pending` off: they draw.
    const int16 keep[4] = { (int16)currentScreen->clipBound_X1, (int16)currentScreen->clipBound_Y1, (int16)currentScreen->clipBound_X2,
                            (int16)currentScreen->clipBound_Y2 };
    for (unsigned i = 0; i < sCount; ++i) {
        const Cmd &c = sCmds[i];
        const int32 *p = c.p;
        if (c.type == CMD_LINEBUF) {
            memcpy(gfxLineBuffer, sArena + c.arena, sizeof(sLineBuf));
            continue;
        }
        SetClip(c.clip);
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
    (void)keep;
    sCount = sArenaUsed = 0;
}

void GpuFrame::Present()
{
    if (pending)
        Flush();
}

// ---------------------------------------------------------------- recording

bool GpuFrame::RecFill(uint32 color, int32 aR, int32 aG, int32 aB)
{
    Cmd *c = Begin(CMD_FILL);
    if (!c)
        return false;
    c->p[0] = (int32)color, c->p[1] = aR, c->p[2] = aG, c->p[3] = aB;
    return true;
}

bool GpuFrame::RecLine(int32 x1, int32 y1, int32 x2, int32 y2, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = Begin(CMD_LINE);
    if (!c)
        return false;
    c->p[0] = x1, c->p[1] = y1, c->p[2] = x2, c->p[3] = y2, c->p[4] = (int32)color, c->p[5] = alpha, c->p[6] = ink, c->p[7] = sr;
    return true;
}

bool GpuFrame::RecRect(int32 x, int32 y, int32 w, int32 h, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = Begin(CMD_RECT);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = w, c->p[3] = h, c->p[4] = (int32)color, c->p[5] = alpha, c->p[6] = ink, c->p[7] = sr;
    return true;
}

bool GpuFrame::RecCircle(int32 x, int32 y, int32 r, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = Begin(CMD_CIRCLE);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = r, c->p[3] = (int32)color, c->p[4] = alpha, c->p[5] = ink, c->p[6] = sr;
    return true;
}

bool GpuFrame::RecCircleOutline(int32 x, int32 y, int32 ri, int32 ro, uint32 color, int32 alpha, int32 ink, bool32 sr)
{
    Cmd *c = Begin(CMD_CIRCLE_OUTLINE);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = ri, c->p[3] = ro, c->p[4] = (int32)color, c->p[5] = alpha, c->p[6] = ink, c->p[7] = sr;
    return true;
}

bool GpuFrame::RecFace(Vector2 *vertices, int32 n, int32 r, int32 g, int32 b, int32 alpha, int32 ink)
{
    if (n < 0 || n > 64)
        return false; // not recorded: the caller draws it, after a flush
    Cmd *c = Begin(CMD_FACE, (uint32)n * sizeof(Vector2));
    if (!c)
        return false;
    memcpy(sArena + c->arena, vertices, (size_t)n * sizeof(Vector2));
    c->p[0] = n, c->p[1] = r, c->p[2] = g, c->p[3] = b, c->p[4] = alpha, c->p[5] = ink;
    return true;
}

bool GpuFrame::RecBlendedFace(Vector2 *vertices, uint32 *colors, int32 n, int32 alpha, int32 ink)
{
    if (n < 0 || n > 64)
        return false;
    Cmd *c = Begin(CMD_BLENDED_FACE, (uint32)n * (sizeof(Vector2) + sizeof(uint32)));
    if (!c)
        return false;
    memcpy(sArena + c->arena, vertices, (size_t)n * sizeof(Vector2));
    memcpy(sArena + c->arena + n * sizeof(Vector2), colors, (size_t)n * sizeof(uint32));
    c->p[0] = n, c->p[1] = alpha, c->p[2] = ink;
    return true;
}

bool GpuFrame::RecSprite(int32 x, int32 y, int32 w, int32 h, int32 sprX, int32 sprY, int32 dir, int32 ink, int32 alpha, int32 sheet)
{
    Cmd *c = Begin(CMD_SPRITE);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = w, c->p[3] = h, c->p[4] = sprX, c->p[5] = sprY, c->p[6] = dir, c->p[7] = ink, c->p[8] = alpha, c->p[9] = sheet;
    return true;
}

bool GpuFrame::RecRoto(int32 x, int32 y, int32 pivotX, int32 pivotY, int32 w, int32 h, int32 sprX, int32 sprY, int32 scaleX, int32 scaleY, int32 dir,
                       int16 rot, int32 ink, int32 alpha, int32 sheet)
{
    Cmd *c = Begin(CMD_ROTO);
    if (!c)
        return false;
    c->p[0] = x, c->p[1] = y, c->p[2] = pivotX, c->p[3] = pivotY, c->p[4] = w, c->p[5] = h, c->p[6] = sprX, c->p[7] = sprY, c->p[8] = scaleX;
    c->p[9] = scaleY, c->p[10] = dir, c->p[11] = rot, c->p[12] = ink, c->p[13] = alpha, c->p[14] = sheet;
    return true;
}

bool GpuFrame::RecLayer(TileLayer *layer, int kind)
{
    const int y1 = currentScreen->clipBound_Y1, y2 = currentScreen->clipBound_Y2;
    const unsigned lines = y2 > y1 ? (unsigned)(y2 - y1) : 0;
    Cmd *c = Begin(CMD_LAYER, lines * (unsigned)sizeof(ScanlineInfo));
    if (!c)
        return false;
    c->kind = (uint8)kind;
    c->p[0] = (int32)(uintptr_t)layer;
    if (lines)
        memcpy(sArena + c->arena, &scanlines[y1], lines * sizeof(ScanlineInfo));
    return true;
}

#endif
