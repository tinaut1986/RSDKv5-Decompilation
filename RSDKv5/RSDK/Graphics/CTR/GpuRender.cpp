#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/GpuRender.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>

#include "RSDK/Graphics/CTR/C3DPresent.hpp"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"
#include "RSDK/Graphics/CTR/gpu_shbin.h"
#include "RSDK/Dev/CTR/DevTools.hpp"

// Dev tools timing of the GPU path: geometry build -> PCOPY, atlas conversions -> PXFER, the citro3d frame (the wait
// for the previous GPU frame, the submit) -> PFRAME.
#if MANIA_DEV_TOOLS
#define GPU_TIME(stage, since) RSDK::DevTools::StageAdd(RSDK::DevTools::stage, svcGetSystemTick() - (since))
#else
#define GPU_TIME(stage, since) ((void)0)
#endif

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
    MAX_QUADS    = 8192,
    MAX_BATCHES  = 64,
    MAX_PENDING  = 2048,
    BANKS        = PALETTE_BANK_COUNT,
};

// 12 bytes: position in quarter pixels and texel coordinates as 16-bit integers (gpu.v.pica scales them), so the
// linear heap holds twice the quads it held with floats (the Special Stage draws thousands of faces).
struct Vtx {
    int16 x, y, u, v;
    uint8 r, g, b, a;
};

inline int16 Quarter(float p) { return (int16)(p >= 0 ? p * 4.0f + 0.5f : p * 4.0f - 0.5f); }

enum Tex { TEX_TILE, TEX_SPRITE, TEX_SOFT };

// Software layers: a layer kind the GPU cannot draw (rotozoom, vscroll, a layer split by the water line) is drawn by
// the engine's own function into screens[1] (unused on the 3DS), keyed with a colour no palette produces in practice,
// and its rows are copied into this texture and drawn as one quad in their place in the frame.
enum { SOFT_W = 512, SOFT_H = 256, SOFT_KEY = 0x0020, SOFT_JOBS = 8 };
struct SoftJob {
    int16 x1, y1, x2, y2, texY;
};
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
C3D_Tex sTileTex, sSprTex, sSoftTex;
u16 *sTileData, *sSprData, *sSoftData;
SoftJob sSoftJobs[SOFT_JOBS];
unsigned sSoftJobCount, sSoftRows;
DVLB_s *sDvlb;
shaderProgram_s sProgram;
int sProjLoc, sUvLoc;
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
u16 sTexPal[BANKS][PALETTE_BANK_SIZE]; // the palette as atlas texels (index 0 transparent), kept with the shadow
uint32 sGroupGen[BANKS][32];

// Tile cache: one cell per (bank, tile id)
int16 sTileCell[BANKS][TILE_IDS];
uint32 sTileStamp[BANKS][TILE_IDS];
uint32 sTileGroups[TILE_IDS]; // palette groups the tile's pixels use (valid once converted)
uint32 sTileHash[TILE_COUNT];  // content of each base tile when DrawAniTile last touched it (it rewrites unchanged frames too)
bool sSprFull;                 // the last ATLAS_FULL came from the sprite atlas
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

// Morton offset of (x, y) inside an 8x8 block, indexed y * 8 + x.
struct MortonTable {
    uint8 v[64];
    MortonTable()
    {
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x)
                v[y * 8 + x] = (uint8)Morton(x, y);
    }
};
const MortonTable kMorton;

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

void ResetSprites()
{
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

void ResetCaches()
{
    memset(sTileCell, 0xFF, sizeof(sTileCell)); // -1
    memset(sTileStamp, 0, sizeof(sTileStamp));
    memset(sTileHash, 0, sizeof(sTileHash));
    sTileCellsUsed = 0;
    ++sGen; // everything converted from now on is newer than any group change so far
    ResetSprites();
}

void TrackPalette(bool all = false);

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
    // Optional: without it the software layers are refused as before.
    if (C3D_TexInit(&sSoftTex, SOFT_W, SOFT_H, GPU_RGBA5551)) {
        sSoftData = (u16 *)sSoftTex.data;
        C3D_TexSetFilter(&sSoftTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sSoftTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
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
    sUvLoc   = shaderInstanceGetUniformLocation(sProgram.vertexShader, "uvscale");

    AttrInfo_Init(&sAttr);
    AttrInfo_AddLoader(&sAttr, 0, GPU_SHORT, 4);         // x, y (quarter pixels), u, v (texels)
    AttrInfo_AddLoader(&sAttr, 1, GPU_UNSIGNED_BYTE, 4); // r, g, b, a

    ResetCaches();
    TrackPalette(true);
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
// `all` refreshes every bank (at start: a bank that never changes would keep an empty texel table).
void TrackPalette(bool all)
{
    ++sGen;
    for (unsigned b = 0; b < BANKS; ++b) {
        const uint32 *now = (const uint32 *)fullPalette[b], *was = (const uint32 *)sPalShadow[b];
        if (!all && !memcmp(now, was, sizeof(sPalShadow[b])))
            continue;
        for (unsigned g = 0; g < 32; ++g) // a group is 8 colours, 4 words
            if (now[g * 4] != was[g * 4] || now[g * 4 + 1] != was[g * 4 + 1] || now[g * 4 + 2] != was[g * 4 + 2] || now[g * 4 + 3] != was[g * 4 + 3])
                sGroupGen[b][g] = sGen;
        memcpy(sPalShadow[b], fullPalette[b], sizeof(sPalShadow[b]));
        sTexPal[b][0] = 0;
        for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
            sTexPal[b][i] = ToTexel(fullPalette[b][i]);
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
    if (stamp == sGen)
        return cell;
    // Unchanged colours: the copy is as good as one made now, so it takes this generation and the tile's other
    // uses this frame (the same water tile on many lines) stop at the test above.
    if (stamp && !GroupsChanged(bank, sTileGroups[id], stamp)) {
        sTileStamp[bank][id] = sGen;
        return cell;
    }
    {
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

unsigned sBadData;   // which check of EnsureSprite failed (GpuRender::ReasonName)
char sRefusal[160]; // the last refused command (GpuRender::LastRefusal)

SprSlot *EnsureSprite(unsigned sheet, unsigned bank, int sprX, int sprY, int w, int h, int *refuse)
{
    *refuse   = 0;
    sBadData  = sheet >= SURFACE_COUNT ? 1 : !gfxSurface[sheet].pixels ? 2 : (sprX < 0 || sprY < 0) ? 3 : (w <= 0 || h <= 0) ? 4
                : (sprX >= 16384 || sprY >= 16384 || w >= 2048 || h >= 2048) ? 5 : bank >= BANKS ? 6 : 0;
    if (sBadData) {
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
            if (s.stamp && s.stamp != sGen && !GroupsChanged(bank, s.groups, s.stamp))
                s.stamp = sGen; // still good: see EnsureTile
            if (s.stamp != sGen) {
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
                sSprFull = true;
                *refuse  = GpuRender::REASON_ATLAS_FULL;
                return NULL;
            }
            const unsigned w8 = (w + 7) & ~7u, h8 = (h + 7) & ~7u;
            unsigned ax, ay;
            if (!AllocSprite(w8, h8, &ax, &ay)) {
                sSprFull = true;
                *refuse  = GpuRender::REASON_ATLAS_FULL;
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
    (void)pal;
    const u16 *tex = sTexPal[p.bank];
    uint32 groups  = 0;
    // Four 8x8 blocks, each 64 consecutive texels in Morton order.
    for (unsigned by = 0; by < 2; ++by)
        for (unsigned bx = 0; bx < 2; ++bx) {
            u16 *blk        = &sTileData[(((Y >> 3) + by) * (TILE_ATLAS_W >> 3) + (X >> 3) + bx) << 6];
            const uint8 *sp = src + by * 8 * 16 + bx * 8;
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                    const unsigned idx = sp[y * 16 + x];
                    groups |= (idx ? 1u : 0u) << (idx >> 3);
                    blk[kMorton.v[y * 8 + x]] = tex[idx];
                }
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
    (void)atlasW, (void)atlasH; // the shader's uvscale, set per texture, normalises the texel coordinates
    const int16 X0 = Quarter(x0), X1 = Quarter(x1), Y0 = Quarter(y0), Y1 = Quarter(y1);
    const int16 U0 = (int16)u0, U1 = (int16)u1, V0 = (int16)v0, V1 = (int16)v1;
    v[0] = { X0, Y0, U0, V0, r, g, b, a };
    v[1] = { X1, Y0, U1, V0, r, g, b, a };
    v[2] = { X1, Y1, U1, V1, r, g, b, a };
    v[3] = { X0, Y1, U0, V1, r, g, b, a };
    return true;
}

// A quad with free corners (a rotated sprite): corners in order around the quad, texel coordinates per corner.
bool AddQuadFree(uint8 tex, uint8 blend, const float *px, const float *py, const float *tu, const float *tv, unsigned atlasW, unsigned atlasH, uint8 r,
                 uint8 g, uint8 b, uint8 a)
{
    if (!AddQuad(tex, blend, 0, 0, 0, 0, 0, 0, 0, 0, atlasW, atlasH, r, g, b, a))
        return false;
    Vtx *v = &sVbo[sCur][(sQuadCount - 1) * 4];
    for (int i = 0; i < 4; ++i)
        v[i] = { Quarter(px[i]), Quarter(py[i]), (int16)tu[i], (int16)tv[i], r, g, b, a };
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

// A sprite too big for one atlas entry (menu pictures, wide parallax pieces): the part on screen is cut into chunks of
// CHUNK x CHUNK sheet pixels aligned on the sheet, each cached as a sprite of its own, so frames reuse them.
enum { CHUNK = 128 };

bool EmitSpriteChunked(const Cmd &c, int32 X0, int32 Y0, int32 W0, int32 H0, int32 sprX0, int32 sprY0, int32 direction, int32 sheetID, uint8 blend,
                       int32 alpha)
{
    const bool fx = direction == FLIP_X || direction == FLIP_XY, fy = direction == FLIP_Y || direction == FLIP_XY;
    // The visible screen rectangle, then the sheet range it shows.
    const int32 vx0 = X0 > c.clip[0] ? X0 : c.clip[0], vx1 = X0 + W0 < c.clip[2] ? X0 + W0 : c.clip[2];
    const int32 vy0 = Y0 > c.clip[1] ? Y0 : c.clip[1], vy1 = Y0 + H0 < c.clip[3] ? Y0 + H0 : c.clip[3];
    if (vx0 >= vx1 || vy0 >= vy1)
        return true;
    const int32 su0 = fx ? sprX0 + (X0 + W0 - vx1) : sprX0 + (vx0 - X0), su1 = su0 + (vx1 - vx0);
    const int32 sv0 = fy ? sprY0 + (Y0 + H0 - vy1) : sprY0 + (vy0 - Y0), sv1 = sv0 + (vy1 - vy0);
    uint8 bank;
    if (!UniformBank(vy0, vy1, &bank))
        return Refuse(GpuRender::REASON_PALETTE_LINES);
    const GFXSurface &g = gfxSurface[sheetID < SURFACE_COUNT ? sheetID : 0];
    for (int32 cv = sv0 - (sv0 % CHUNK); cv < sv1; cv += CHUNK) {
        for (int32 cu = su0 - (su0 % CHUNK); cu < su1; cu += CHUNK) {
            // The whole chunk is cached (clamped to the sheet), the visible part of it is drawn.
            const int32 cw = cu + CHUNK <= g.width ? CHUNK : g.width - cu, ch = cv + CHUNK <= g.height ? CHUNK : g.height - cv;
            const int32 u0 = su0 > cu ? su0 : cu, u1 = su1 < cu + cw ? su1 : cu + cw;
            const int32 v0 = sv0 > cv ? sv0 : cv, v1 = sv1 < cv + ch ? sv1 : cv + ch;
            if (u0 >= u1 || v0 >= v1)
                continue;
            int refuse;
            SprSlot *slot = EnsureSprite((unsigned)sheetID, bank, cu, cv, cw, ch, &refuse);
            if (!slot)
                return Refuse((GpuRender::Reason)refuse);
            const float dx0 = (float)(fx ? X0 + (sprX0 + W0 - u1) : X0 + (u0 - sprX0)), dx1 = dx0 + (float)(u1 - u0);
            const float dy0 = (float)(fy ? Y0 + (sprY0 + H0 - v1) : Y0 + (v0 - sprY0)), dy1 = dy0 + (float)(v1 - v0);
            float tu0 = (float)(slot->x + (u0 - cu)), tu1 = (float)(slot->x + (u1 - cu));
            float tv0 = (float)(slot->y + (v0 - cv)), tv1 = (float)(slot->y + (v1 - cv));
            if (fx) {
                const float t = tu0;
                tu0 = tu1, tu1 = t;
            }
            if (fy) {
                const float t = tv0;
                tv0 = tv1, tv1 = t;
            }
            if (!AddQuad(TEX_SPRITE, blend, dx0, dy0, dx1, dy1, tu0, tv0, tu1, tv1, SPR_ATLAS_W, SPR_ATLAS_H, 255, 255, 255, (uint8)alpha))
                return Refuse(GpuRender::REASON_QUAD_LIMIT);
        }
    }
    return true;
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

    if (w0 * h0 > SPR_MAX_PIXELS || w0 > 512 || h0 > 256)
        return EmitSpriteChunked(c, x, y, w0, h0, sprX0, sprY0, direction, sheetID, blend, alpha);

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

// The RGB565 the software renderer would store for a 24-bit colour, widened back to 8 bits by bit replication.
void Widen565(uint32 color, uint8 *r, uint8 *g, uint8 *b)
{
    const uint16 c16  = rgb32To16_B[(color >> 0) & 0xFF] | rgb32To16_G[(color >> 8) & 0xFF] | rgb32To16_R[(color >> 16) & 0xFF];
    const unsigned r5 = c16 >> 11, g6 = (c16 >> 5) & 0x3F, b5 = c16 & 0x1F;
    *r = (uint8)((r5 << 3) | (r5 >> 2)), *g = (uint8)((g6 << 2) | (g6 >> 4)), *b = (uint8)((b5 << 3) | (b5 >> 2));
}

// DrawFace / DrawBlendedFace: a convex polygon (vertices in 16.16 screen coordinates) as a fan of triangles over the
// solid block of the sprite atlas, each triangle a quad with its last corner repeated. `colors` is NULL for one flat
// colour (r, g, b), else 0xRRGGBB per vertex, which the GPU interpolates as the software does along the edges.
bool EmitPolygon(const Cmd &c, const Vector2 *v, int32 n, const uint32 *colors, uint8 r, uint8 g, uint8 b, int32 ink, int32 alpha)
{
    if (n < 3)
        return true;
    uint8 blend;
    bool skip;
    if (!SpriteInk(ink, &alpha, &blend, &skip))
        return false;
    if (skip)
        return true;
    ++sStats.faces;
    // Integers throughout: 16.16 screen coordinates -> quarter pixels (rounded), bounds in 16.16.
    int32 minX = 0x7FFFFFFF, maxX = -0x7FFFFFFF, minY = 0x7FFFFFFF, maxY = -0x7FFFFFFF;
    for (int32 i = 0; i < n; ++i) {
        minX = v[i].x < minX ? v[i].x : minX, maxX = v[i].x > maxX ? v[i].x : maxX;
        minY = v[i].y < minY ? v[i].y : minY, maxY = v[i].y > maxY ? v[i].y : maxY;
    }
    const int32 cx1 = c.clip[0] << 16, cy1 = c.clip[1] << 16, cx2 = c.clip[2] << 16, cy2 = c.clip[3] << 16;
    if (maxX <= cx1 || minX >= cx2 || maxY <= cy1 || minY >= cy2)
        return true;
    if ((minX < cx1 && c.clip[0] > 0) || (minY < cy1 && c.clip[1] > 0) || (maxX > cx2 && c.clip[2] < screens[0].size.x)
        || (maxY > cy2 && c.clip[3] < screens[0].size.y))
        return Refuse(GpuRender::REASON_UNSUPPORTED, colors ? CMD_BLENDED_FACE : CMD_FACE);

    for (int32 i = 1; i + 1 < n; i += 2) {
        // Two triangles per quad where the fan allows it: (0, i, i + 1, i + 2), else (0, i, i + 1, i + 1).
        const int32 k[4] = { 0, i, i + 1, i + 2 < n ? i + 2 : i + 1 };
        if (!AddQuad(TEX_SPRITE, blend, 0, 0, 0, 0, 0, 0, 0, 0, SPR_ATLAS_W, SPR_ATLAS_H, r, g, b, (uint8)alpha))
            return Refuse(GpuRender::REASON_QUAD_LIMIT);
        Vtx *q = &sVbo[sCur][(sQuadCount - 1) * 4];
        for (int j = 0; j < 4; ++j) {
            const Vector2 &p = v[k[j]];
            // Quarter pixels in 16 bits: +-8191 px, far beyond the screen; a wilder vertex is clamped, not wrapped.
            const int32 qx = (p.x + 0x2000) >> 14, qy = (p.y + 0x2000) >> 14;
            q[j].x = (int16)(qx < -32000 ? -32000 : qx > 32000 ? 32000 : qx);
            q[j].y = (int16)(qy < -32000 ? -32000 : qy > 32000 ? 32000 : qy);
            q[j].u = 4, q[j].v = 4; // the solid block of the sprite atlas
            if (colors)
                Widen565(colors[k[j]], &q[j].r, &q[j].g, &q[j].b);
        }
    }
    return true;
}

// Unit circle in 64 steps, for circles and rings.
struct CircleTable {
    float x[65], y[65];
    CircleTable()
    {
        for (int i = 0; i <= 64; ++i)
            x[i] = cosf(i * 6.28318531f / 64), y[i] = sinf(i * 6.28318531f / 64);
    }
};
const CircleTable kCircle;

// One quad of four free corners in screen pixels on the solid block, flat colour.
bool AddSolidQuad(uint8 blend, const float *px, const float *py, uint8 r, uint8 g, uint8 b, uint8 a)
{
    if (!AddQuad(TEX_SPRITE, blend, 0, 0, 0, 0, 0, 0, 0, 0, SPR_ATLAS_W, SPR_ATLAS_H, r, g, b, a))
        return false;
    Vtx *q = &sVbo[sCur][(sQuadCount - 1) * 4];
    for (int j = 0; j < 4; ++j)
        q[j].x = Quarter(px[j]), q[j].y = Quarter(py[j]), q[j].u = 4, q[j].v = 4;
    return true;
}

// DrawCircle (filled, inner radius 0) and DrawCircleOutline (a ring) as polygons of 16-64 steps.
bool EmitRing(const Cmd &c, int32 x, int32 y, int32 inner, int32 outer, uint32 color, int32 alpha, int32 ink, bool screenRelative)
{
    if (outer <= 0 || inner >= outer)
        return true;
    uint8 blend;
    bool skip;
    if (!SpriteInk(ink, &alpha, &blend, &skip))
        return false;
    if (skip)
        return true;
    if (!screenRelative) {
        x = FROM_FIXED(x) - c.pos[0];
        y = FROM_FIXED(y) - c.pos[1];
    }
    if (x + outer < c.clip[0] || x - outer >= c.clip[2] || y + outer < c.clip[1] || y - outer >= c.clip[3])
        return true;
    if ((x - outer < c.clip[0] && c.clip[0] > 0) || (y - outer < c.clip[1] && c.clip[1] > 0) || (x + outer >= c.clip[2] && c.clip[2] < screens[0].size.x)
        || (y + outer >= c.clip[3] && c.clip[3] < screens[0].size.y))
        return Refuse(GpuRender::REASON_UNSUPPORTED, inner > 0 ? CMD_CIRCLE_OUTLINE : CMD_CIRCLE);
    uint8 r, g, b;
    Widen565(color, &r, &g, &b);
    const int step = outer >= 48 ? 1 : outer >= 16 ? 2 : 4; // 64, 32 or 16 segments
    const float cx = x + 0.5f, cy = y + 0.5f, ro = outer + 0.5f, ri = inner > 0 ? inner - 0.5f : 0.0f;
    for (int i = 0; i < 64; i += step) {
        const int k = i + step;
        float px[4], py[4];
        if (ri <= 0) { // a wedge of the disc: centre, two rim points (the last corner repeated)
            px[0] = cx, py[0] = cy;
            px[1] = cx + ro * kCircle.x[i], py[1] = cy + ro * kCircle.y[i];
            px[2] = cx + ro * kCircle.x[k], py[2] = cy + ro * kCircle.y[k];
            px[3] = px[2], py[3] = py[2];
        }
        else {
            px[0] = cx + ro * kCircle.x[i], py[0] = cy + ro * kCircle.y[i];
            px[1] = cx + ro * kCircle.x[k], py[1] = cy + ro * kCircle.y[k];
            px[2] = cx + ri * kCircle.x[k], py[2] = cy + ri * kCircle.y[k];
            px[3] = cx + ri * kCircle.x[i], py[3] = cy + ri * kCircle.y[i];
        }
        if (!AddSolidQuad(blend, px, py, r, g, b, (uint8)alpha))
            return Refuse(GpuRender::REASON_QUAD_LIMIT);
    }
    return true;
}

bool EmitFace(const Cmd &c, const uint8 *arena)
{
    uint8 r, g, b;
    Widen565(((uint32)c.p[1] << 16) | ((uint32)c.p[2] << 8) | (uint32)c.p[3], &r, &g, &b);
    return EmitPolygon(c, (const Vector2 *)arena, c.p[0], NULL, r, g, b, c.p[5], c.p[4]);
}

bool EmitFaces(const Cmd &c, const uint8 *at)
{
    for (int32 f = 0; f < c.p[0]; ++f) {
        const int32 n = ((const int32 *)at)[0];
        uint8 r, g, b;
        Widen565((uint32)((const int32 *)at)[1], &r, &g, &b);
        if (!EmitPolygon(c, (const Vector2 *)(at + 8), n, NULL, r, g, b, c.p[2], c.p[1]))
            return false;
        at += 8 + n * sizeof(Vector2);
    }
    return true;
}

bool EmitBlendedFaces(const Cmd &c, const uint8 *at)
{
    for (int32 f = 0; f < c.p[0]; ++f) {
        const int32 n = *(const int32 *)at;
        if (!EmitPolygon(c, (const Vector2 *)(at + 4), n, (const uint32 *)(at + 4 + n * sizeof(Vector2)), 0, 0, 0, c.p[2], c.p[1]))
            return false;
        at += 4 + n * (sizeof(Vector2) + sizeof(uint32));
    }
    return true;
}

bool EmitBlendedFace(const Cmd &c, const uint8 *arena)
{
    const int32 n = c.p[0];
    return EmitPolygon(c, (const Vector2 *)arena, n, (const uint32 *)(arena + n * sizeof(Vector2)), 0, 0, 0, c.p[2], c.p[1]);
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

bool EmitSoftLayer(const Cmd &c, TileLayer *layer, const ScanlineInfo *sl, int kind)
{
    if (!sSoftData || !C3DPresent::Staging())
        return Refuse(GpuRender::REASON_UNSUPPORTED, 100 + kind);
    const int x1 = c.clip[0] < 0 ? 0 : c.clip[0], x2 = c.clip[2] > screens[0].pitch ? screens[0].pitch : c.clip[2];
    const int y1 = c.clip[1] < 0 ? 0 : c.clip[1], y2 = c.clip[3] > SCREEN_YSIZE ? SCREEN_YSIZE : c.clip[3];
    if (x1 >= x2 || y1 >= y2)
        return true;
    const int h = y2 - y1;
    if (sSoftJobCount >= SOFT_JOBS || sSoftRows + (unsigned)h > SOFT_H)
        return Refuse(GpuRender::REASON_UNSUPPORTED, 100 + kind);

    // The engine's function, on screen 1, with the state the layer was recorded with.
    ScreenInfo &scr  = screens[1];
    scr.size         = screens[0].size;
    scr.center       = screens[0].center;
    scr.pitch        = screens[0].pitch;
    scr.position.x   = c.pos[0];
    scr.position.y   = c.pos[1];
    scr.clipBound_X1 = x1, scr.clipBound_Y1 = y1, scr.clipBound_X2 = x2, scr.clipBound_Y2 = y2;
    for (int y = y1; y < y2; ++y)
        for (int x = x1; x < x2; ++x)
            scr.frameBuffer[y * scr.pitch + x] = SOFT_KEY;
    memcpy(&scanlines[y1], sl, (size_t)h * sizeof(ScanlineInfo));
    static uint8 keepLines[SCREEN_YSIZE];
    memcpy(keepLines, gfxLineBuffer, sizeof(keepLines));
    memcpy(gfxLineBuffer, sLineBuf, sizeof(keepLines));
    ScreenInfo *const keepScreen = currentScreen;
    const bool keepPending       = GpuFrame::pending;
    currentScreen                = &scr;
    GpuFrame::pending            = false; // the draw call must draw, not flush the frame being executed
    switch (kind) {
        case LAYER_HSCROLL: DrawLayerHScroll(layer); break;
        case LAYER_VSCROLL: DrawLayerVScroll(layer); break;
        case LAYER_ROTOZOOM: DrawLayerRotozoom(layer); break;
        default: DrawLayerBasic(layer); break;
    }
    GpuFrame::pending = keepPending;
    currentScreen     = keepScreen;
    memcpy(gfxLineBuffer, keepLines, sizeof(keepLines));

    const unsigned texY        = sSoftRows;
    sSoftJobs[sSoftJobCount++] = { (int16)x1, (int16)y1, (int16)x2, (int16)y2, (int16)texY };
    sSoftRows                  = (texY + (unsigned)h + 7) & ~7u; // jobs start on a block row
    ++sStats.softLayers;
    if (!AddQuad(TEX_SOFT, BLEND_NORMAL, (float)x1, (float)y1, (float)x2, (float)y2, (float)x1, (float)texY, (float)x2, (float)(texY + h), SOFT_W, SOFT_H, 255, 255,
                 255, 255))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

// Copies a software layer's rows from screen 1 into the staging buffer as RGBA5551 (the key colour transparent), in
// plain rows: the display-transfer engine tiles the whole buffer into the texture afterwards (UploadSoft).
void ConvertSoft(const SoftJob &j)
{
    const ScreenInfo &scr = screens[1];
    u16 *staging          = (u16 *)C3DPresent::Staging();
    for (int y = j.y1; y < j.y2; ++y) {
        const uint16 *src = &scr.frameBuffer[y * scr.pitch];
        u16 *dst          = &staging[(j.texY + (y - j.y1)) * SOFT_W];
        for (int x = j.x1; x < j.x2; ++x) {
            const uint16 c = src[x];
            dst[x]         = c == SOFT_KEY ? 0 : (u16)((c & 0xFFC0) | ((c & 0x1F) << 1) | 1);
        }
    }
}

void UploadSoft()
{
    void *staging = C3DPresent::Staging();
    GSPGPU_FlushDataCache(staging, SOFT_W * SOFT_H * 2);
    C3D_SyncDisplayTransfer((u32 *)staging, GX_BUFFER_DIM(SOFT_W, SOFT_H), (u32 *)sSoftTex.data, GX_BUFFER_DIM(SOFT_W, SOFT_H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

// One run of screen lines [dy, dy + n) that all show tile row `row` of the layer starting at pixel row `srcY` inside
// it, with the layer's pixel column of screen column dx0 being `px`; screen columns [dx0, dx1).
bool EmitTileRun(TileLayer *layer, unsigned bank, int row, int srcY, int dy, int n, int dx0, int dx1, int px)
{
    if (row < 0 || row >= layer->ysize) {
        ++sStats.rowsOut;
        return true;
    } // outside the layout: nothing is drawn (the software reads garbage; this is the sane reading)
    int sx  = dx0;
    int col = ((dx0 + px) >> 4) % layer->xsize; // one division per run; the ARM11 has no divide instruction
    if (col < 0)
        col += layer->xsize;
    const uint16 *rowLayout = &layer->layout[row << layer->widthShift];
    for (; sx < dx1; col = col + 1 == layer->xsize ? 0 : col + 1) {
        const int X     = sx + px;
        const int inX   = X & 15;
        const int w     = (16 - inX) < (dx1 - sx) ? (16 - inX) : (dx1 - sx);
        const uint16 t  = rowLayout[col];
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
        return EmitSoftLayer(c, layer, sl, LAYER_HSCROLL); // lines in different palette banks: the water line
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
const char *GpuRender::LastRefusal() { return sRefusal[0] ? sRefusal : "-"; }

const char *GpuRender::ReasonName()
{
    static const char *const kCmd[]   = { "FILL", "LINE", "RECT", "CIRCLE", "CIRCLE OUTLINE", "FACE", "BLENDED FACE", "SPRITE", "ROTO SPRITE", "LAYER", "LINEBUF", "FACES", "BLENDED FACES" };
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
        case REASON_BAD_DATA: {
            static const char *const kBad[] = { "BAD DATA", "BAD SHEET ID", "SHEET NOT LOADED", "NEGATIVE SPRITE POS", "EMPTY SPRITE", "SPRITE TOO BIG", "BAD BANK" };
            return sBadData < sizeof(kBad) / sizeof(kBad[0]) ? kBad[sBadData] : "BAD DATA";
        }
    }
    return "?";
}

bool GpuRender::Render(const Stream &stream)
{
    const u64 tStart = svcGetSystemTick();
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
    if (tileDirtyAny) {
        // Animated tiles rewritten since the last GPU frame: their copies (every bank, every flip variant) go stale.
        tileDirtyAny = false;
        for (unsigned w = 0; w < TILE_COUNT / 32; ++w) {
            uint32 bits  = tileDirty[w];
            tileDirty[w] = 0;
            while (bits) {
                const unsigned t = w * 32 + (unsigned)__builtin_ctz(bits);
                bits &= bits - 1;
                // FNV-1a of the base tile: DrawAniTile rewrites tiles every frame, mostly with the same picture.
                const uint32 *px = (const uint32 *)&tilesetPixels[t * TILE_DATASIZE];
                uint32 h         = 2166136261u;
                for (unsigned k = 0; k < TILE_DATASIZE / 4; ++k)
                    h = (h ^ px[k]) * 16777619u;
                h |= 1; // 0 means "not hashed yet"
                if (h == sTileHash[t])
                    continue;
                sTileHash[t] = h;
                for (unsigned b = 0; b < BANKS; ++b)
                    for (unsigned v = 0; v < 4; ++v)
                        sTileStamp[b][t + v * TILE_COUNT] = 0;
            }
        }
    }

    sCur = (sCur + 1) & 1;
    bool ok;
    unsigned i = 0;
    for (int attempt = 0;; ++attempt) {
    sQuadCount    = 0;
    sBatchCount   = 0;
    sPendingCount = 0;
    sFail.reason  = REASON_OK;
    sSprFull      = false;
    sSoftJobCount = sSoftRows = 0;
    sStats.softLayers = 0;
    sStats.faces      = 0;
    ok            = true;
    memset(sLineBuf, 0, sizeof(sLineBuf));

    for (i = 0; i < stream.count && ok; ++i) {
        const Cmd &c = stream.cmds[i];
        switch (c.type) {
            case CMD_LINEBUF: memcpy(sLineBuf, stream.arena + c.arena, sizeof(sLineBuf)); break;
            case CMD_SPRITE: ok = EmitSprite(c); break;
            case CMD_ROTO: ok = EmitRoto(c); break;
            case CMD_FACE: ok = EmitFace(c, stream.arena + c.arena); break;
            case CMD_BLENDED_FACE: ok = EmitBlendedFace(c, stream.arena + c.arena); break;
            case CMD_FACES: {
                const u64 t0 = svcGetSystemTick();
                ok           = EmitFaces(c, stream.arena + c.arena);
                GPU_TIME(STAGE_SHAPES, t0); // GPU mode: face emission (the software shapes stage is idle then)
                break;
            }
            case CMD_BLENDED_FACES: {
                const u64 t0 = svcGetSystemTick();
                ok           = EmitBlendedFaces(c, stream.arena + c.arena);
                GPU_TIME(STAGE_SHAPES, t0);
                break;
            }
            case CMD_RECT: ok = EmitRect(c); break;
            case CMD_CIRCLE: ok = EmitRing(c, c.p[0], c.p[1], 0, c.p[2], (uint32)c.p[3], c.p[4], c.p[5], c.p[6] != 0); break;
            case CMD_CIRCLE_OUTLINE: ok = EmitRing(c, c.p[0], c.p[1], c.p[2], c.p[3], (uint32)c.p[4], c.p[5], c.p[6], c.p[7] != 0); break;
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
                    ok = EmitSoftLayer(c, layer, sl, c.kind);
                break;
            }
            default: ok = Refuse(REASON_UNSUPPORTED, c.type); break;
        }
    }
    // The sprite atlas filled up mid-frame: empty it and build the frame once more from scratch (the tiles queued so
    // far go back to unconverted, the sprites are all new).
    if (ok || !sSprFull || attempt > 0)
        break;
    for (unsigned k = 0; k < sPendingCount; ++k)
        if (sPending[k].kind == 0)
            sTileStamp[sPending[k].bank][sPending[k].id] = 0;
    ResetSprites();
    ++sStats.atlasRetries;
    }
    if (!ok) {
        // What was refused, for SITE STATE: the command and its parameters.
        if (i > 0) {
            const Cmd &f = stream.cmds[i - 1];
            int n = snprintf(sRefusal, sizeof(sRefusal), "#%u type %u kind %u clip %d %d %d %d |", i - 1, f.type, f.kind, f.clip[0], f.clip[1], f.clip[2], f.clip[3]);
            for (int k = 0; k < 15 && n > 0 && n < (int)sizeof(sRefusal); ++k)
                n += snprintf(sRefusal + n, sizeof(sRefusal) - n, " %ld", (long)f.p[k]);
        }
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
            // Start over next frame: only the sprites when their atlas filled up (the tiles stay valid).
            if (sSprFull)
                ResetSprites();
            else
                ResetCaches();
            sSeenContent = contentVersion;
        }
        return false;
    }

    // ---- the frame is decided: textures, then geometry
    GPU_TIME(STAGE_PCOPY, tStart);
    u64 t = svcGetSystemTick();
    C3D_RenderTarget *target = (C3D_RenderTarget *)C3DPresent::Target();
    // Without SYNCDRAW, which waits for the next vblank (up to 16 ms): FrameBegin already waits until the GPU has
    // finished the previous frame, so the atlas cells rewritten below are no longer being read.
    C3D_FrameBegin(0);
    GPU_TIME(STAGE_PFRAME, t);
    t            = svcGetSystemTick();
    sTileDirtyLo = sSprDirtyLo = ~0u;
    sTileDirtyHi = sSprDirtyHi = 0;
    for (unsigned i = 0; i < sPendingCount; ++i) {
        if (sPending[i].kind == 0)
            ConvertTile(sPending[i]);
        else
            ConvertSprite(sPending[i]);
    }
    for (unsigned i = 0; i < sSoftJobCount; ++i)
        ConvertSoft(sSoftJobs[i]);
    if (sSoftJobCount)
        UploadSoft();
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
    GPU_TIME(STAGE_PXFER, t);
    t = svcGetSystemTick();

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
            C3D_TexBind(0, bt.tex == TEX_TILE ? &sTileTex : bt.tex == TEX_SOFT ? &sSoftTex : &sSprTex);
            const float w = bt.tex == TEX_TILE ? TILE_ATLAS_W : bt.tex == TEX_SOFT ? SOFT_W : SPR_ATLAS_W;
            const float h = bt.tex == TEX_TILE ? TILE_ATLAS_H : bt.tex == TEX_SOFT ? SOFT_H : SPR_ATLAS_H;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvLoc, 1.0f / w, -1.0f / h, 0.0f, 0.0f);
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
    GPU_TIME(STAGE_PFRAME, t);
    return true;
}

#endif
