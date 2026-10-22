#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/GpuRender.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <citro3d.h>
#include <string.h>

#include "RSDK/Graphics/CTR/C3DPresent.hpp"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"
#include "RSDK/Graphics/CTR/gpu_shbin.h"

using namespace RSDK;
using namespace RSDK::GpuFrame;

namespace
{

enum {
    TILE_ATLAS_W = 512,
    TILE_ATLAS_H = 512,
    TILE_CELLS   = (TILE_ATLAS_W / 16) * (TILE_ATLAS_H / 16),
    TILE_IDS     = TILE_COUNT * 4, // a tile index may carry the flip bits of the layout (0xFFF)
    SPR_ATLAS_W  = 1024,
    SPR_ATLAS_H  = 512,
    SPR_SLOTS    = 4096,
    SPR_MAX_PIXELS = 256 * 256,
    MAX_QUADS    = 4096,
    MAX_BATCHES  = 64,
    MAX_PENDING  = 2048,
    BANKS        = PALETTE_BANK_COUNT,
};

struct Vtx {
    float x, y, u, v;
    uint8 r, g, b, a;
};

enum Tex { TEX_TILE, TEX_SPRITE };
enum Blend { BLEND_NORMAL, BLEND_ADD, BLEND_SUB, BLEND_FILL };

struct Batch {
    uint8 tex, blend;
    unsigned first, count; // quads
    uint32 constant;       // BLEND_FILL: the per-channel alpha (R, G, B in the low bytes)
};

struct SprSlot {
    uint64 key; // 0 = free
    uint16 x, y, w, h; // atlas position and padded (multiple of 8) size
    uint16 srcW, srcH;
    uint32 stamp;  // sGen when converted (0: never)
    uint32 groups; // palette groups its pixels use (see sGroupGen)
};

struct Pending {
    uint8 kind; // 0 tile, 1 sprite
    uint8 bank;
    uint16 id;  // tile id, or sprite slot
    uint16 cell;
};

bool sReady, sFailed;
C3D_Tex sTileTex, sSprTex;
u16 *sTileData, *sSprData;
DVLB_s *sDvlb;
shaderProgram_s sProgram;
int sProjLoc;
C3D_AttrInfo sAttr;
C3D_BufInfo sBufInfo;
Vtx *sVbo[2];
u16 *sIdx;
unsigned sCur;

// Per-frame build state
unsigned sQuadCount;
Batch sBatches[MAX_BATCHES];
unsigned sBatchCount;
Pending sPending[MAX_PENDING];
unsigned sPendingCount;
unsigned sSeenContent = ~0u;
GpuRender::Stats sStats;

// Palette change tracking. Every frame the palette is compared with a copy of the last one; the colours are
// taken in 32 groups of 8 and each group remembers the generation (frame) it last changed in. A converted tile or
// sprite keeps the generation it was converted in and the groups its pixels use, and is converted again only when
// one of those groups changed since: a palette cycle on a few colours no longer reconverts everything on screen.
// The comparison catches every writer of fullPalette, hooked or not.
uint32 sGen = 1;
uint16 sPalShadow[BANKS][PALETTE_BANK_SIZE];
uint32 sGroupGen[BANKS][32];

// Tile cache: one cell per (bank, tile id)
int16 sTileCell[BANKS][TILE_IDS];
uint32 sTileStamp[BANKS][TILE_IDS];
uint32 sTileGroups[TILE_IDS]; // palette groups the tile's pixels use (valid once converted)
unsigned sTileCellsUsed;

// Sprite cache: shelves of 8-pixel granularity
SprSlot sSlots[SPR_SLOTS];
struct Shelf {
    uint16 y, h, xUsed;
};
Shelf sShelves[SPR_ATLAS_H / 8];
unsigned sShelfCount;
unsigned sSlotsUsed;
uint16 sShelfNextY;

uint8 sLineBuf[SCREEN_YSIZE];

// Dirty spans of the atlases (byte offsets), flushed before the frame is drawn
unsigned sTileDirtyLo, sTileDirtyHi, sSprDirtyLo, sSprDirtyHi;

inline unsigned Morton(unsigned x, unsigned y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

inline u16 ToTexel(uint16 c565)
{
    const unsigned r = c565 >> 11, g = (c565 >> 5) & 0x3F, b = c565 & 0x1F;
    return (u16)((r << 11) | ((g >> 1) << 6) | (b << 1) | 1);
}

inline void Put(u16 *base, unsigned atlasW, unsigned X, unsigned Y, u16 texel)
{
    base[((((Y >> 3) * (atlasW >> 3) + (X >> 3)) << 6) | Morton(X & 7, Y & 7))] = texel;
}

inline void Dirty(unsigned &lo, unsigned &hi, unsigned atlasW, unsigned X, unsigned Y, unsigned w, unsigned h)
{
    const unsigned first = (((Y >> 3) * (atlasW >> 3) + (X >> 3)) << 6) * 2;
    const unsigned last  = ((((Y + h - 1) >> 3) * (atlasW >> 3) + ((X + w - 1) >> 3) + 1) << 6) * 2;
    if (first < lo)
        lo = first;
    if (last > hi)
        hi = last;
}

void ResetCaches()
{
    memset(sTileCell, 0xFF, sizeof(sTileCell)); // -1
    memset(sTileStamp, 0, sizeof(sTileStamp));
    sTileCellsUsed = 0;
    ++sGen; // everything converted from now on is newer than any group change so far
    memset(sSlots, 0, sizeof(sSlots));
    sShelfCount = sSlotsUsed = 0;
    sShelfNextY = 0;
    // The solid-colour block at the atlas origin (8x8, opaque white) and the shelf it opens.
    sShelves[0] = { 0, 8, 8 };
    sShelfCount = 1;
    sShelfNextY = 8;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            Put(sSprData, SPR_ATLAS_W, x, y, 0xFFFF);
    sSprDirtyLo = 0;
    sSprDirtyHi = 8 * 8 * 2;
}

bool Setup()
{
    if (sReady)
        return true;
    if (sFailed)
        return false;
    sFailed = true;
    if (!C3DPresent::Init())
        return false;

    if (!C3D_TexInit(&sTileTex, TILE_ATLAS_W, TILE_ATLAS_H, GPU_RGBA5551) || !C3D_TexInit(&sSprTex, SPR_ATLAS_W, SPR_ATLAS_H, GPU_RGBA5551))
        return false;
    sTileData = (u16 *)sTileTex.data;
    sSprData  = (u16 *)sSprTex.data;
    memset(sTileData, 0, (size_t)TILE_ATLAS_W * TILE_ATLAS_H * 2);
    memset(sSprData, 0, (size_t)SPR_ATLAS_W * SPR_ATLAS_H * 2);
    C3D_TexSetFilter(&sTileTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&sSprTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTileTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetWrap(&sSprTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    sVbo[0] = (Vtx *)linearAlloc(sizeof(Vtx) * 4 * MAX_QUADS);
    sVbo[1] = (Vtx *)linearAlloc(sizeof(Vtx) * 4 * MAX_QUADS);
    sIdx    = (u16 *)linearAlloc(sizeof(u16) * 6 * MAX_QUADS);
    if (!sVbo[0] || !sVbo[1] || !sIdx)
        return false;
    for (unsigned q = 0; q < MAX_QUADS; ++q) {
        const u16 b = (u16)(q * 4);
        sIdx[q * 6 + 0] = b, sIdx[q * 6 + 1] = b + 1, sIdx[q * 6 + 2] = b + 2;
        sIdx[q * 6 + 3] = b, sIdx[q * 6 + 4] = b + 2, sIdx[q * 6 + 5] = b + 3;
    }
    GSPGPU_FlushDataCache(sIdx, sizeof(u16) * 6 * MAX_QUADS);

    sDvlb = DVLB_ParseFile((u32 *)gpu_shbin, sizeof(gpu_shbin));
    if (!sDvlb)
        return false;
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    sProjLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");

    AttrInfo_Init(&sAttr);
    AttrInfo_AddLoader(&sAttr, 0, GPU_FLOAT, 4);         // x, y, u, v
    AttrInfo_AddLoader(&sAttr, 1, GPU_UNSIGNED_BYTE, 4); // r, g, b, a

    ResetCaches();
    sReady  = true;
    sFailed = false;
    return true;
}

// ---------------------------------------------------------------- frame state

struct Fail {
    GpuRender::Reason reason;
    unsigned cmd;
};
Fail sFail;

bool Refuse(GpuRender::Reason reason, unsigned cmd = 0)
{
    sFail.reason = reason;
    sFail.cmd    = cmd;
    return false;
}

bool UniformBank(int y0, int y1, uint8 *bank)
{
    if (y0 < 0)
        y0 = 0;
    if (y1 > SCREEN_YSIZE)
        y1 = SCREEN_YSIZE;
    if (y1 <= y0) {
        *bank = 0;
        return true;
    }
    const uint8 b = sLineBuf[y0];
    for (int y = y0 + 1; y < y1; ++y)
        if (sLineBuf[y] != b)
            return false;
    *bank = b;
    return true;
}

// ---------------------------------------------------------------- caches

// The palette groups set in `groups` changed after generation `stamp`.
bool GroupsChanged(unsigned bank, uint32 groups, uint32 stamp)
{
    while (groups) {
        const unsigned g = (unsigned)__builtin_ctz(groups);
        if (sGroupGen[bank][g] > stamp)
            return true;
        groups &= groups - 1;
    }
    return false;
}

// Compares the palette with the last frame's and stamps the groups that changed with this frame's generation.
void TrackPalette()
{
    ++sGen;
    for (unsigned b = 0; b < BANKS; ++b) {
        const uint32 *now = (const uint32 *)fullPalette[b], *was = (const uint32 *)sPalShadow[b];
        if (!memcmp(now, was, sizeof(sPalShadow[b])))
            continue;
        for (unsigned g = 0; g < 32; ++g) // a group is 8 colours, 4 words
            if (now[g * 4] != was[g * 4] || now[g * 4 + 1] != was[g * 4 + 1] || now[g * 4 + 2] != was[g * 4 + 2] || now[g * 4 + 3] != was[g * 4 + 3])
                sGroupGen[b][g] = sGen;
        memcpy(sPalShadow[b], fullPalette[b], sizeof(sPalShadow[b]));
    }
}

// Atlas cell of a tile with the palette bank applied; queues its conversion when it is new or stale.
int EnsureTile(unsigned id, unsigned bank)
{
    if (id >= TILE_IDS || bank >= BANKS)
        return -1;
    int cell = sTileCell[bank][id];
    if (cell < 0) {
        if (sTileCellsUsed >= TILE_CELLS)
            return -2; // atlas full
        cell                 = (int)sTileCellsUsed++;
        sTileCell[bank][id]  = (int16)cell;
        sTileStamp[bank][id] = 0;
    }
    const uint32 stamp = sTileStamp[bank][id];
    if (!stamp || (stamp != sGen && GroupsChanged(bank, sTileGroups[id], stamp))) {
        if (sPendingCount >= MAX_PENDING)
            return -3;
        sTileStamp[bank][id] = sGen;
        sPending[sPendingCount++] = { 0, (uint8)bank, (uint16)id, (uint16)cell };
    }
    return cell;
}

uint64 SprKey(unsigned sheet, unsigned bank, unsigned sprX, unsigned sprY, unsigned w, unsigned h)
{
    // bit 0 set (0 is a free slot), sprY bits 2-15, sprX 16-29, h 30-40, w 41-51, bank 52-55, sheet 56-63
    return 1ull | ((uint64)sheet << 56) | ((uint64)bank << 52) | ((uint64)w << 41) | ((uint64)h << 30) | ((uint64)sprX << 16) | ((uint64)sprY << 2);
}

// Shelf allocation in 8-pixel units; false when the atlas is full.
bool AllocSprite(unsigned w8, unsigned h8, unsigned *x, unsigned *y)
{
    int best = -1;
    for (unsigned s = 0; s < sShelfCount; ++s) {
        Shelf &sh = sShelves[s];
        if (sh.h >= h8 && sh.h <= h8 + 8 && sh.xUsed + w8 <= SPR_ATLAS_W && (best < 0 || sh.h < sShelves[best].h))
            best = (int)s;
    }
    if (best < 0) {
        if (sShelfNextY + h8 > SPR_ATLAS_H || sShelfCount >= SPR_ATLAS_H / 8)
            return false;
        sShelves[sShelfCount] = { sShelfNextY, (uint16)h8, 0 };
        sShelfNextY += (uint16)h8;
        best = (int)sShelfCount++;
    }
    *x = sShelves[best].xUsed;
    *y = sShelves[best].y;
    sShelves[best].xUsed += (uint16)w8;
    return true;
}

SprSlot *EnsureSprite(unsigned sheet, unsigned bank, int sprX, int sprY, int w, int h, int *refuse)
{
    *refuse = 0;
    if (sheet >= SURFACE_COUNT || !gfxSurface[sheet].pixels || sprX < 0 || sprY < 0 || w <= 0 || h <= 0 || sprX >= 16384 || sprY >= 16384 || w >= 2048 || h >= 2048 ||
        bank >= BANKS) {
        *refuse = GpuRender::REASON_BAD_DATA;
        return NULL;
    }
    if ((unsigned)(w * h) > SPR_MAX_PIXELS) {
        *refuse = GpuRender::REASON_SPRITE_SIZE;
        return NULL;
    }
    const uint64 key = SprKey(sheet, bank, sprX, sprY, w, h);
    unsigned i       = (unsigned)((key ^ (key >> 29) ^ (key >> 13)) * 2654435761u) % SPR_SLOTS;
    for (unsigned probe = 0; probe < SPR_SLOTS; ++probe, i = (i + 1) % SPR_SLOTS) {
        SprSlot &s = sSlots[i];
        if (s.key == key) {
            if (!s.stamp || (s.stamp != sGen && GroupsChanged(bank, s.groups, s.stamp))) {
                if (sPendingCount >= MAX_PENDING) {
                    *refuse = GpuRender::REASON_QUAD_LIMIT;
                    return NULL;
                }
                s.stamp = sGen;
                sPending[sPendingCount++] = { 1, (uint8)bank, (uint16)i, 0 };
            }
            return &s;
        }
        if (!s.key) {
            if (sSlotsUsed >= SPR_SLOTS * 3 / 4 || sPendingCount >= MAX_PENDING) {
                *refuse = GpuRender::REASON_ATLAS_FULL;
                return NULL;
            }
            const unsigned w8 = (w + 7) & ~7u, h8 = (h + 7) & ~7u;
            unsigned ax, ay;
            if (!AllocSprite(w8, h8, &ax, &ay)) {
                *refuse = GpuRender::REASON_ATLAS_FULL;
                return NULL;
            }
            s.key  = key;
            s.x    = (uint16)ax, s.y = (uint16)ay, s.w = (uint16)w8, s.h = (uint16)h8;
            s.srcW = (uint16)w, s.srcH = (uint16)h;
            s.stamp  = sGen;
            s.groups = 0;
            ++sSlotsUsed;
            sPending[sPendingCount++] = { 1, (uint8)bank, (uint16)i, 0 };
            return &s;
        }
    }
    *refuse = GpuRender::REASON_ATLAS_FULL;
    return NULL;
}

void ConvertTile(const Pending &p)
{
    const uint8 *src = &tilesetPixels[TILE_DATASIZE * p.id];
    const uint16 *pal = fullPalette[p.bank];
    const unsigned X = (p.cell % (TILE_ATLAS_W / 16)) * 16, Y = (p.cell / (TILE_ATLAS_W / 16)) * 16;
    uint32 groups = 0;
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            const unsigned idx = src[y * 16 + x];
            groups |= (idx ? 1u : 0u) << (idx >> 3);
            Put(sTileData, TILE_ATLAS_W, X + x, Y + y, idx ? ToTexel(pal[idx]) : 0);
        }
    sTileGroups[p.id] = groups;
    Dirty(sTileDirtyLo, sTileDirtyHi, TILE_ATLAS_W, X, Y, 16, 16);
    ++sStats.tileConversions;
}

void ConvertSprite(const Pending &p)
{
    SprSlot &s = sSlots[p.id];
    // The key holds the sheet position: unpack it.
    const unsigned sheet = (unsigned)(s.key >> 56), sprX = (unsigned)((s.key >> 16) & 0x3FFF), sprY = (unsigned)((s.key >> 2) & 0x3FFF);
    const GFXSurface &g  = gfxSurface[sheet];
    const uint16 *pal    = fullPalette[p.bank];
    uint32 groups        = 0;
    for (unsigned y = 0; y < s.h; ++y)
        for (unsigned x = 0; x < s.w; ++x) {
            u16 texel = 0;
            if (x < s.srcW && y < s.srcH && (int)(sprX + x) < g.width && (int)(sprY + y) < g.height) {
                const unsigned idx = g.pixels[(sprY + y) * g.width + sprX + x];
                groups |= (idx ? 1u : 0u) << (idx >> 3);
                texel = idx ? ToTexel(pal[idx]) : 0;
            }
            Put(sSprData, SPR_ATLAS_W, s.x + x, s.y + y, texel);
        }
    s.groups = groups;
    Dirty(sSprDirtyLo, sSprDirtyHi, SPR_ATLAS_W, s.x, s.y, s.w, s.h);
    ++sStats.spriteConversions;
}

// ---------------------------------------------------------------- geometry

bool AddQuad(uint8 tex, uint8 blend, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, unsigned atlasW, unsigned atlasH,
             uint8 r, uint8 g, uint8 b, uint8 a, uint32 constant = 0)
{
    if (sQuadCount >= MAX_QUADS)
        return false;
    if (!sBatchCount || sBatches[sBatchCount - 1].tex != tex || sBatches[sBatchCount - 1].blend != blend || blend == BLEND_FILL) {
        if (sBatchCount >= MAX_BATCHES)
            return false;
        sBatches[sBatchCount++] = { tex, blend, sQuadCount, 0, constant };
    }
    ++sBatches[sBatchCount - 1].count;
    Vtx *v = &sVbo[sCur][sQuadCount * 4];
    ++sQuadCount;
    const float iu = 1.0f / atlasW, iv = 1.0f / atlasH;
    // Texels are rows from the top; the PICA samples memory row 0 at v = 1.
    const float U0 = u0 * iu, U1 = u1 * iu, V0 = 1.0f - v0 * iv, V1 = 1.0f - v1 * iv;
    v[0] = { x0, y0, U0, V0, r, g, b, a };
    v[1] = { x1, y0, U1, V0, r, g, b, a };
    v[2] = { x1, y1, U1, V1, r, g, b, a };
    v[3] = { x0, y1, U0, V1, r, g, b, a };
    return true;
}

// A quad with free corners (a rotated sprite): corners in order around the quad, texel coordinates per corner.
bool AddQuadFree(uint8 tex, uint8 blend, const float *px, const float *py, const float *tu, const float *tv, unsigned atlasW, unsigned atlasH, uint8 r,
                 uint8 g, uint8 b, uint8 a)
{
    if (!AddQuad(tex, blend, 0, 0, 0, 0, 0, 0, 0, 0, atlasW, atlasH, r, g, b, a))
        return false;
    Vtx *v         = &sVbo[sCur][(sQuadCount - 1) * 4];
    const float iu = 1.0f / atlasW, iv = 1.0f / atlasH;
    for (int i = 0; i < 4; ++i)
        v[i] = { px[i], py[i], tu[i] * iu, 1.0f - tv[i] * iv, r, g, b, a };
    return true;
}

// ---------------------------------------------------------------- commands

// The blend and alpha of an ink effect for sprites; false refuses the frame, `*skip` drops the sprite (alpha 0).
bool SpriteInk(int32 ink, int32 *alpha, uint8 *blend, bool *skip)
{
    *blend = BLEND_NORMAL;
    *skip  = false;
    switch (ink) {
        case INK_NONE: *alpha = 0xFF; return true;
        case INK_BLEND: *alpha = 0x80; return true;
        case INK_ALPHA:
        case INK_ADD:
        case INK_SUB:
            if (ink == INK_ADD)
                *blend = BLEND_ADD;
            else if (ink == INK_SUB)
                *blend = BLEND_SUB;
            if (*alpha > 0xFF)
                *alpha = 0xFF;
            else if (*alpha <= 0)
                *skip = true;
            return true;
        default: return Refuse(GpuRender::REASON_INK, (unsigned)ink);
    }
}

bool EmitSprite(const Cmd &c)
{
    const int32 *p = c.p;
    int32 x = p[0], y = p[1], width = p[2], height = p[3], sprX = p[4], sprY = p[5];
    const int32 direction = p[6], sheetID = p[9];
    int32 ink = p[7], alpha = p[8];
    const int32 sprX0 = sprX, sprY0 = sprY, w0 = width, h0 = height;

    uint8 blend = BLEND_NORMAL;
    switch (ink) {
        case INK_NONE: alpha = 0xFF; break;
        case INK_BLEND: alpha = 0x80; break;
        case INK_ALPHA:
            if (alpha > 0xFF)
                alpha = 0xFF;
            else if (alpha <= 0)
                return true;
            break;
        case INK_ADD:
            blend = BLEND_ADD;
            if (alpha > 0xFF)
                alpha = 0xFF;
            else if (alpha <= 0)
                return true;
            break;
        case INK_SUB:
            blend = BLEND_SUB;
            if (alpha > 0xFF)
                alpha = 0xFF;
            else if (alpha <= 0)
                return true;
            break;
        default: return Refuse(GpuRender::REASON_INK, (unsigned)ink);
    }

    int32 widthFlip = width, heightFlip = height;
    if (width + x > c.clip[2])
        width = c.clip[2] - x;
    if (x < c.clip[0]) {
        const int32 val = x - c.clip[0];
        sprX -= val;
        width += val;
        widthFlip += 2 * val;
        x = c.clip[0];
    }
    if (height + y > c.clip[3])
        height = c.clip[3] - y;
    if (y < c.clip[1]) {
        const int32 val = y - c.clip[1];
        sprY -= val;
        height += val;
        heightFlip += 2 * val;
        y = c.clip[1];
    }
    if (width <= 0 || height <= 0)
        return true;

    uint8 bank;
    if (!UniformBank(y, y + height, &bank))
        return Refuse(GpuRender::REASON_PALETTE_LINES);
    int refuse;
    SprSlot *slot = EnsureSprite((unsigned)sheetID, bank, sprX0, sprY0, w0, h0, &refuse);
    if (!slot)
        return Refuse((GpuRender::Reason)refuse);

    // Source rectangle in atlas texels. A flipped axis runs from the far edge back.
    float su0, su1, sv0, sv1;
    const float ex = (float)slot->x, ey = (float)slot->y;
    if (direction == FLIP_X || direction == FLIP_XY) {
        su0 = ex + (float)(sprX + widthFlip - sprX0);
        su1 = su0 - (float)width;
    }
    else {
        su0 = ex + (float)(sprX - sprX0);
        su1 = su0 + (float)width;
    }
    if (direction == FLIP_Y || direction == FLIP_XY) {
        sv0 = ey + (float)(sprY + heightFlip - sprY0);
        sv1 = sv0 - (float)height;
    }
    else {
        sv0 = ey + (float)(sprY - sprY0);
        sv1 = sv0 + (float)height;
    }
    if (!AddQuad(TEX_SPRITE, blend, (float)x, (float)y, (float)(x + width), (float)(y + height), su0, sv0, su1, sv1, SPR_ATLAS_W, SPR_ATLAS_H, 255, 255, 255,
                 (uint8)alpha))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

// DrawSpriteRotozoom as a quad. The software walks the screen pixels of the bounding box and maps each back into the
// sheet with an affine step; here the same map is inverted to put the sprite's four corners on the screen.
// Screen offset (dx, dy) from (x, y) -> sheet: u = sprX - pivotX + kx*(c*dx - s*dy), v = sprY - pivotY + ky*(s*dx + c*dy)
// (FLIP_X negates the x terms), with kx = 512 / scaleX and c, s the engine's 512-step tables.
bool EmitRoto(const Cmd &c)
{
    const int32 *p = c.p;
    const int32 x = p[0], y = p[1], pivotX = p[2], pivotY = p[3], width = p[4], height = p[5], sprX = p[6], sprY = p[7];
    const int32 scaleX = p[8], scaleY = p[9], direction = p[10], sheetID = p[14];
    const int16 rotation = (int16)p[11];
    int32 alpha          = p[13];
    if (direction != RSDK::FLIP_NONE && direction != RSDK::FLIP_X)
        return true; // the software draws nothing for these either
    if (scaleX <= 0 || scaleY <= 0 || width <= 0 || height <= 0)
        return true;
    uint8 blend;
    bool skip;
    if (!SpriteInk(p[12], &alpha, &blend, &skip))
        return false;
    if (skip)
        return true;

    int32 angle = 0x200 - (rotation & 0x1FF);
    if (!(rotation & 0x1FF))
        angle = rotation & 0x1FF;
    const float cs = sin512LookupTable[angle] / 512.0f, cc = cos512LookupTable[angle] / 512.0f;
    const float kx = 512.0f / scaleX, ky = 512.0f / scaleY, fx = direction == RSDK::FLIP_X ? -1.0f : 1.0f;
    // Screen -> sheet matrix and its inverse
    const float a = fx * kx * cc, b = -fx * kx * cs, m = ky * cs, d = ky * cc;
    const float det = a * d - b * m;
    if (det == 0)
        return true;
    const float ia = d / det, ib = -b / det, ic = -m / det, id = a / det;

    // Sheet corners relative to the sheet point under (x, y), which is (sprX - pivotX, sprY - pivotY)
    const float rx[4] = { (float)pivotX, (float)(pivotX + width), (float)(pivotX + width), (float)pivotX };
    const float ry[4] = { (float)pivotY, (float)pivotY, (float)(pivotY + height), (float)(pivotY + height) };
    float px[4], py[4], tu[4], tv[4], minY = 1e9f, maxY = -1e9f, minX = 1e9f, maxX = -1e9f;
    for (int i = 0; i < 4; ++i) {
        px[i] = x + ia * rx[i] + ib * ry[i];
        py[i] = y + ic * rx[i] + id * ry[i];
        minX = px[i] < minX ? px[i] : minX, maxX = px[i] > maxX ? px[i] : maxX;
        minY = py[i] < minY ? py[i] : minY, maxY = py[i] > maxY ? py[i] : maxY;
    }
    if (maxX <= c.clip[0] || minX >= c.clip[2] || maxY <= c.clip[1] || minY >= c.clip[3])
        return true; // off the clip rectangle
    // Quads are not scissored: a sprite crossing a clip rectangle smaller than the screen is left to the CPU.
    if ((minX < c.clip[0] && c.clip[0] > 0) || (minY < c.clip[1] && c.clip[1] > 0) || (maxX > c.clip[2] && c.clip[2] < screens[0].size.x)
        || (maxY > c.clip[3] && c.clip[3] < screens[0].size.y))
        return Refuse(GpuRender::REASON_UNSUPPORTED, CMD_ROTO);

    uint8 bank;
    if (!UniformBank((int)minY, (int)maxY + 1, &bank))
        return Refuse(GpuRender::REASON_PALETTE_LINES);
    int refuse;
    SprSlot *slot = EnsureSprite((unsigned)sheetID, bank, sprX, sprY, width, height, &refuse);
    if (!slot)
        return Refuse((GpuRender::Reason)refuse);
    for (int i = 0; i < 4; ++i) {
        tu[i] = slot->x + (rx[i] - pivotX);
        tv[i] = slot->y + (ry[i] - pivotY);
    }
    if (!AddQuadFree(TEX_SPRITE, blend, px, py, tu, tv, SPR_ATLAS_W, SPR_ATLAS_H, 255, 255, 255, (uint8)alpha))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

// FillScreen: every channel blended towards the colour with its own alpha (fades and flashes). The GPU's
// constant-colour blend factor is per channel too: dst * (1 - alpha) + colour * alpha.
bool EmitFill(const Cmd &c)
{
    const uint32 color = (uint32)c.p[0];
    const int32 aR = CLAMP(c.p[1], 0, 0xFF), aG = CLAMP(c.p[2], 0, 0xFF), aB = CLAMP(c.p[3], 0, 0xFF);
    if (aR + aG + aB == 0)
        return true;
    const uint16 rr = rgb32To16_B[(color >> 0x10) & 0xFF], gg = rgb32To16_B[(color >> 0x08) & 0xFF], bb = rgb32To16_B[(color >> 0x00) & 0xFF];
    // rgb32To16_B gives a 5-bit value for each channel (the software uses it for all three).
    const uint8 r = (uint8)((rr << 3) | (rr >> 2)), g = (uint8)((gg << 3) | (gg >> 2)), b = (uint8)((bb << 3) | (bb >> 2));
    if (!AddQuad(TEX_SPRITE, BLEND_FILL, 0.0f, 0.0f, (float)screens[0].size.x, (float)screens[0].size.y, 4.0f, 4.0f, 4.0f, 4.0f, SPR_ATLAS_W, SPR_ATLAS_H,
                 r, g, b, 255, (uint32)aR | ((uint32)aG << 8) | ((uint32)aB << 16) | (0xFFu << 24)))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

bool EmitRect(const Cmd &c)
{
    const int32 *p = c.p;
    int32 x = p[0], y = p[1], width = p[2], height = p[3];
    const uint32 color = (uint32)p[4];
    int32 alpha = p[5], ink = p[6];
    const bool screenRelative = p[7] != 0;

    uint8 blend = BLEND_NORMAL;
    switch (ink) {
        case INK_NONE: alpha = 0xFF; break;
        case INK_BLEND: alpha = 0x80; break;
        case INK_ALPHA:
            if (alpha > 0xFF)
                alpha = 0xFF;
            else if (alpha <= 0)
                return true;
            break;
        case INK_ADD:
        case INK_SUB:
            blend = ink == INK_ADD ? BLEND_ADD : BLEND_SUB;
            if (alpha > 0xFF)
                alpha = 0xFF;
            else if (alpha <= 0)
                return true;
            break;
        default: return Refuse(GpuRender::REASON_INK, (unsigned)ink);
    }
    if (!screenRelative) {
        x      = FROM_FIXED(x) - c.pos[0];
        y      = FROM_FIXED(y) - c.pos[1];
        width  = FROM_FIXED(width);
        height = FROM_FIXED(height);
    }
    if (width + x > c.clip[2])
        width = c.clip[2] - x;
    if (x < c.clip[0]) {
        width += x - c.clip[0];
        x = c.clip[0];
    }
    if (height + y > c.clip[3])
        height = c.clip[3] - y;
    if (y < c.clip[1]) {
        height += y - c.clip[1];
        y = c.clip[1];
    }
    if (width <= 0 || height <= 0)
        return true;

    // The colour as the software renderer stores it (RGB565), widened back to 8 bits by bit replication.
    const uint16 c16 = rgb32To16_B[(color >> 0) & 0xFF] | rgb32To16_G[(color >> 8) & 0xFF] | rgb32To16_R[(color >> 16) & 0xFF];
    const unsigned r5 = c16 >> 11, g6 = (c16 >> 5) & 0x3F, b5 = c16 & 0x1F;
    const uint8 r = (uint8)((r5 << 3) | (r5 >> 2)), g = (uint8)((g6 << 2) | (g6 >> 4)), b = (uint8)((b5 << 3) | (b5 >> 2));
    // The solid block at the sprite atlas origin: sample its middle.
    if (!AddQuad(TEX_SPRITE, blend, (float)x, (float)y, (float)(x + width), (float)(y + height), 4.0f, 4.0f, 4.0f, 4.0f, SPR_ATLAS_W, SPR_ATLAS_H, r, g, b,
                 (uint8)alpha))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

// One run of screen lines [dy, dy + n) that all show tile row `row` of the layer starting at pixel row `srcY` inside
// it, with the layer's pixel column of screen column dx0 being `px`; screen columns [dx0, dx1).
bool EmitTileRun(TileLayer *layer, unsigned bank, int row, int srcY, int dy, int n, int dx0, int dx1, int px)
{
    if (row < 0 || row >= layer->ysize) {
        ++sStats.rowsOut;
        return true;
    } // outside the layout: nothing is drawn (the software reads garbage; this is the sane reading)
    int sx = dx0;
    while (sx < dx1) {
        const int X     = sx + px;
        const int col   = (X >> 4) % layer->xsize;
        const int inX   = X & 15;
        const int w     = (16 - inX) < (dx1 - sx) ? (16 - inX) : (dx1 - sx);
        const uint16 t  = layer->layout[col + (row << layer->widthShift)];
        if (t >= 0xFFFF)
            ++sStats.emptyTiles;
        if (t < 0xFFFF) {
            ++sStats.tileQuads;
            const int cell = EnsureTile(t & 0xFFF, bank);
            if (cell == -2)
                return Refuse(GpuRender::REASON_ATLAS_FULL);
            if (cell < 0)
                return Refuse(GpuRender::REASON_TILE_RANGE);
            const float cx = (float)((cell % (TILE_ATLAS_W / 16)) * 16 + inX), cy = (float)((cell / (TILE_ATLAS_W / 16)) * 16 + srcY);
            if (!AddQuad(TEX_TILE, BLEND_NORMAL, (float)sx, (float)dy, (float)(sx + w), (float)(dy + n), cx, cy, cx + (float)w, cy + (float)n, TILE_ATLAS_W,
                         TILE_ATLAS_H, 255, 255, 255, 255))
                return Refuse(GpuRender::REASON_QUAD_LIMIT);
        }
        sx += w;
    }
    return true;
}

bool EmitLayerBasic(const Cmd &c, TileLayer *layer, const ScanlineInfo *sl)
{
    const int y1 = c.clip[1], y2 = c.clip[3], x1 = c.clip[0], x2 = c.clip[2];
    if (x1 >= x2 || y1 >= y2)
        return true;
    // DrawLayerBasic: the layer's pixel row of the first line, then 16-line blocks, each taking its x from the
    // scanline of its first line.
    int layerY = FROM_FIXED(sl[0].position.y);
    int line   = y1;
    while (line < y2) {
        const int rem = 16 - (layerY & 15);
        const int n   = rem < (y2 - line) ? rem : (y2 - line);
        const int xOff = FROM_FIXED(sl[line - y1].position.x);
        if (!EmitTileRun(layer, 0, layerY >> 4, layerY & 15, line, n, x1, x2, x1 + xOff - x1 + 0))
            return false;
        line += n;
        layerY += n;
    }
    return true;
}

bool EmitLayerHScroll(const Cmd &c, TileLayer *layer, const ScanlineInfo *sl)
{
    const int y1 = c.clip[1], y2 = c.clip[3];
    uint8 bank;
    if (!UniformBank(y1, y2, &bank))
        return Refuse(GpuRender::REASON_PALETTE_LINES);
    // screens[0], not currentScreen: the draw loop has moved that on to screen 1 (never set up on the 3DS, pitch 0,
    // which left every tile run empty).
    const int pitch = screens[0].pitch;
    int line        = y1;
    while (line < y2) {
        // A band: consecutive lines with the same x whose layer row advances by one pixel each.
        int x = sl[line - y1].position.x;
        const int tileX = FROM_FIXED(x);
        if (tileX >= TILE_SIZE * layer->xsize)
            x = TO_FIXED(tileX - TILE_SIZE * layer->xsize);
        else if (tileX < 0)
            x = TO_FIXED(tileX + TILE_SIZE * layer->xsize);
        const int yPix = FROM_FIXED(sl[line - y1].position.y);
        int end        = line + 1;
        while (end < y2) {
            const ScanlineInfo &n = sl[end - y1];
            if (n.position.x != sl[line - y1].position.x || FROM_FIXED(n.position.y) != yPix + (end - line))
                break;
            ++end;
        }
        // Split the band at tile-row boundaries.
        int ly = yPix, dy = line;
        while (dy < end) {
            const int rem = 16 - (ly & 15);
            const int n   = rem < (end - dy) ? rem : (end - dy);
            if (!EmitTileRun(layer, bank, ly >> 4, ly & 15, dy, n, 0, pitch, FROM_FIXED(x)))
                return false;
            dy += n;
            ly += n;
        }
        line = end;
    }
    return true;
}

} // namespace

const GpuRender::Stats &GpuRender::GetStats() { return sStats; }

const char *GpuRender::ReasonName()
{
    static const char *const kCmd[]   = { "FILL", "LINE", "RECT", "CIRCLE", "CIRCLE OUTLINE", "FACE", "BLENDED FACE", "SPRITE", "ROTO SPRITE", "LAYER", "LINEBUF" };
    static const char *const kLayer[] = { "HSCROLL LAYER", "VSCROLL LAYER", "ROTOZOOM LAYER", "BASIC LAYER" };
    static const char *const kInk[]   = { "INK NONE", "INK BLEND", "INK ALPHA", "INK ADD", "INK SUB", "INK TINT", "INK MASKED", "INK UNMASKED" };
    const unsigned cmd = sStats.unsupportedCmd;
    switch (sStats.reason) {
        case REASON_OK: return NULL;
        case REASON_NO_GPU: return "GPU INIT";
        case REASON_UNSUPPORTED:
            if (cmd >= 100)
                return cmd - 100 < sizeof(kLayer) / sizeof(kLayer[0]) ? kLayer[cmd - 100] : "LAYER KIND";
            return cmd < sizeof(kCmd) / sizeof(kCmd[0]) ? kCmd[cmd] : "COMMAND";
        case REASON_INK: return cmd < sizeof(kInk) / sizeof(kInk[0]) ? kInk[cmd] : "INK";
        case REASON_PALETTE_LINES: return "WATER LINE";
        case REASON_TILE_RANGE: return "TILE RANGE";
        case REASON_ATLAS_FULL: return "ATLAS FULL";
        case REASON_QUAD_LIMIT: return "QUAD LIMIT";
        case REASON_SPRITE_SIZE: return "SPRITE SIZE";
        case REASON_BAD_DATA: return "BAD DATA";
    }
    return "?";
}

bool GpuRender::Render(const Stream &stream)
{
    sStats.reason = REASON_OK;
    sStats.layerCmds = sStats.tileQuads = sStats.emptyTiles = sStats.rowsOut = 0;
    if (!Setup()) {
        sStats.reason = REASON_NO_GPU;
        return false;
    }
    if (sSeenContent != contentVersion) {
        sSeenContent = contentVersion;
        ResetCaches();
    }
    TrackPalette();

    sCur        = (sCur + 1) & 1;
    sQuadCount  = 0;
    sBatchCount = 0;
    sPendingCount = 0;
    sFail.reason = REASON_OK;
    bool ok = true;
    memset(sLineBuf, 0, sizeof(sLineBuf));

    for (unsigned i = 0; i < stream.count && ok; ++i) {
        const Cmd &c = stream.cmds[i];
        switch (c.type) {
            case CMD_LINEBUF: memcpy(sLineBuf, stream.arena + c.arena, sizeof(sLineBuf)); break;
            case CMD_SPRITE: ok = EmitSprite(c); break;
            case CMD_ROTO: ok = EmitRoto(c); break;
            case CMD_RECT: ok = EmitRect(c); break;
            case CMD_FILL: ok = EmitFill(c); break;
            case CMD_LAYER: {
                TileLayer *layer = (TileLayer *)(uintptr_t)c.p[0];
                const ScanlineInfo *sl = (const ScanlineInfo *)(stream.arena + c.arena);
                ++sStats.layerCmds;
                if (!layer->xsize || !layer->ysize)
                    break;
                if (c.kind == LAYER_BASIC)
                    ok = EmitLayerBasic(c, layer, sl);
                else if (c.kind == LAYER_HSCROLL)
                    ok = EmitLayerHScroll(c, layer, sl);
                else
                    ok = Refuse(REASON_UNSUPPORTED, 100 + c.kind);
                break;
            }
            default: ok = Refuse(REASON_UNSUPPORTED, c.type); break;
        }
    }
    if (!ok) {
        // The conversions this frame queued are dropped with it: mark them unconverted again, or the cells would
        // keep whatever they held (the cache trusts its stamps until a colour the entry uses changes).
        for (unsigned i = 0; i < sPendingCount; ++i) {
            if (sPending[i].kind == 0)
                sTileStamp[sPending[i].bank][sPending[i].id] = 0;
            else
                sSlots[sPending[i].id].stamp = 0;
        }
        sPendingCount         = 0;
        sStats.reason         = sFail.reason;
        sStats.unsupportedCmd = sFail.cmd;
        if (sFail.reason == REASON_ATLAS_FULL) {
            ResetCaches(); // start over next frame
            sSeenContent = contentVersion;
        }
        return false;
    }

    // ---- the frame is decided: textures, then geometry
    C3D_RenderTarget *target = (C3D_RenderTarget *)C3DPresent::Target();
    // Without SYNCDRAW, which waits for the next vblank (up to 16 ms): FrameBegin already waits until the GPU has
    // finished the previous frame, so the atlas cells rewritten below are no longer being read.
    C3D_FrameBegin(0);
    sTileDirtyLo = sSprDirtyLo = ~0u;
    sTileDirtyHi = sSprDirtyHi = 0;
    for (unsigned i = 0; i < sPendingCount; ++i) {
        if (sPending[i].kind == 0)
            ConvertTile(sPending[i]);
        else
            ConvertSprite(sPending[i]);
    }
    if (sTileDirtyHi > sTileDirtyLo) {
        GSPGPU_FlushDataCache((u8 *)sTileData + sTileDirtyLo, sTileDirtyHi - sTileDirtyLo);
        ++sStats.atlasFlushes;
    }
    if (sSprDirtyHi > sSprDirtyLo) {
        GSPGPU_FlushDataCache((u8 *)sSprData + sSprDirtyLo, sSprDirtyHi - sSprDirtyLo);
        ++sStats.atlasFlushes;
    }
    // (After a reset the white block was written before the dirty span was cleared.)
    GSPGPU_FlushDataCache(sSprData, 8 * 8 * 2 * 8);
    GSPGPU_FlushDataCache(sVbo[sCur], sizeof(Vtx) * 4 * sQuadCount);
    sStats.quads = sQuadCount;

    C3D_RenderTargetClear(target, C3D_CLEAR_COLOR, 0x000000FF, 0);
    C3D_FrameDrawOn(target);
    C3D_BindProgram(&sProgram);
    C3D_SetAttrInfo(&sAttr);
    BufInfo_Init(&sBufInfo);
    BufInfo_Add(&sBufInfo, sVbo[sCur], sizeof(Vtx), 2, 0x10);
    C3D_SetBufInfo(&sBufInfo);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, (C3D_Mtx *)C3DPresent::Projection());

    int curBlend = -1, curTex = -1;
    for (unsigned b = 0; b < sBatchCount; ++b) {
        const Batch &bt = sBatches[b];
        if (curTex != bt.tex) {
            C3D_TexBind(0, bt.tex == TEX_TILE ? &sTileTex : &sSprTex);
            curTex = bt.tex;
        }
        if (curBlend != bt.blend || bt.blend == BLEND_FILL) {
            switch (bt.blend) {
                case BLEND_FILL:
                    C3D_BlendingColor(bt.constant);
                    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_ONE_MINUS_CONSTANT_COLOR, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
                    break;
                case BLEND_ADD: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE); break;
                case BLEND_SUB: C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE); break;
                default: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); break;
            }
            curBlend = bt.blend;
        }
        C3D_DrawElements(GPU_TRIANGLES, (int)(bt.count * 6), C3D_UNSIGNED_SHORT, sIdx + bt.first * 6);
    }
    C3D_FrameEnd(0);
    return true;
}

#endif
