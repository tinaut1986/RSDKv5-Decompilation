#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/GpuRender.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>

#include "RSDK/Graphics/CTR/C3DPresent.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"
#include "RSDK/Dev/CTR/Port.hpp"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"
#include "RSDK/Graphics/CTR/renderer_shbin.h"
#include "RSDK/Graphics/CTR/GpuScene3D.hpp"
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
    MAX_BATCHES  = 512,
    MAX_PENDING  = 2048,
    REAL_BANKS   = PALETTE_BANK_COUNT,
    BANKS        = PALETTE_BANK_COUNT * 2, // 8..15: palette snapshots taken inside a frame (CMD_PALETTE)
};

// 12 bytes: position in quarter pixels and texel coordinates as 16-bit integers (gpu.v.pica scales them), so the
// linear heap holds twice the quads it held with floats (the Special Stage draws thousands of faces).
struct Vtx {
    int16 x, y, u, v;
    uint8 r, g, b, a;
};

inline int16 Quarter(float p) { return (int16)(p >= 0 ? p * 4.0f + 0.5f : p * 4.0f - 0.5f); }

// TEX_3D: a 3D scene (Batch::first = index in sDraw3D). TEX_TILE_FINE: the tile atlas with texel coordinates in 1/32
// texel (rotozoom layers: affine runs need sub-texel coordinates).
// TEX_ROTO0..: a rotozoom layer drawn whole into a texture of its own (RotoTex), texel coordinates in 1/16 texel.
// TEX_ROTOMASK0..: that layer's hot-colour mask (RotoTex::mask), drawn with BLEND_HOT.
// TEX_BIG0..: the line strip of a rotozoom layer too big for one texture (Strip), texel coordinates in 1/16 texel.
// TEX_FLOOR0..4: the Special Stage floor windows in bank 0 (FloorWin), 1/16 texel, faded to the fog colour per line.
enum Tex { TEX_TILE, TEX_SPRITE, TEX_SOFT, TEX_3D, TEX_TILE_FINE, TEX_FLOOR0, TEX_FLOOR1, TEX_FLOOR2, TEX_FLOOR3, TEX_FLOOR4, TEX_ROTO0, TEX_ROTO1, TEX_ROTOMASK0, TEX_ROTOMASK1,
           TEX_BIG0 }; // TEX_BIG0 + slot: a big rotozoom layer's line strip (Strip)

// Software layers: a layer kind the GPU cannot draw (rotozoom, vscroll, a layer split by the water line) is drawn by
// the engine's own function into screens[1] (unused on the 3DS), keyed with a colour no palette produces in practice,
// and its rows are copied into this texture and drawn as one quad in their place in the frame.
enum { SOFT_W = 512, SOFT_H = 256, SOFT_KEY = 0x0020, SOFT_JOBS = 8 };
struct SoftJob {
    int16 x1, y1, x2, y2, texY;
};
// BLEND_MASKED / BLEND_UNMASKED: INK_MASKED / INK_UNMASKED, normal alpha blending drawn where the stencil says the
// screen holds / does not hold the mask colour (see the mask frame in Render).
// BLEND_HOT: a rotozoom texture's hot colour (RotoTex): the mask's texels equal to the constant's top byte, painted
// opaque in the constant's colour (0xRRGGBB in the low bytes, as the texture combiner takes it).
// BLEND_TINT: INK_TINT with a grey table (TintModel): the frame drawn so far is copied into sGrabTex and the quad redraws
// it from there through the combiner, grey = k * (r + g + b) + c (Batch::constant: k in the low byte, c above it).
enum Blend { BLEND_NORMAL, BLEND_ADD, BLEND_SUB, BLEND_FILL, BLEND_MASKED, BLEND_UNMASKED, BLEND_HOT, BLEND_TINT };

struct Batch {
    uint8 tex, blend;
    unsigned first, count; // quads
    uint32 constant;       // BLEND_FILL: the per-channel alpha (R, G, B in the low bytes)
    int8 depth;            // stereoscopic disparity in pixels at the full slider (+ behind the screen, - in front)
    uint8 scissor;         // index in sScissors (0: none)
    uint8 fog;             // TEX_ROTO*: the bank copy
    uint8 uvk;             // TEX_ROTO*: texel coordinates in 1/16 texel << uvk (lines with a long span)
    uint8 disjoint;        // its quads never overlap (one a screen line): a masked ink draws them in one go
    uint8 fade;            // faded to the floor's fog colour by the vertex alpha's complement (as TEX_FLOOR*)
    uint8 proj;            // drawn with the rotozoom planes' program (sRotoProg): w and disparity in the vertex colour
};

// Scissor rectangles (screen pixels x0, y0, x1, y1) of this frame: a rotated sprite or a 3D scene under a clip smaller
// than the screen, or cut at the water line into runs of one palette bank. sScissor is the one emitters use now.
enum { MAX_SCISSORS = 64 };
int16 sScissors[MAX_SCISSORS][4];
unsigned sScissorCount;
uint8 sScissor;

// Sets sScissor to the clip rectangle when it is smaller than the screen; false when the table is full.
bool ScissorFor(const int16 *clip)
{
    sScissor = 0;
    if (clip[0] <= 0 && clip[1] <= 0 && clip[2] >= screens[0].size.x && clip[3] >= screens[0].size.y)
        return true;
    for (unsigned i = 1; i < sScissorCount; ++i)
        if (!memcmp(sScissors[i], clip, sizeof(sScissors[i]))) {
            sScissor = (uint8)i;
            return true;
        }
    if (sScissorCount >= MAX_SCISSORS)
        return false;
    memcpy(sScissors[sScissorCount], clip, sizeof(sScissors[0]));
    sScissor = (uint8)sScissorCount++;
    return true;
}

// Stereoscopic 3D: the frame is drawn once per eye, each batch shifted by half its depth the eye's way.
bool sStereo;  // this frame is drawn for both eyes (the 3D slider is up, or DEBUG FORCE 3D)
bool sDepths;  // this frame works out depths: stereo, or DEPTH TINT (which shows them with the slider down too)
bool sUfoStage; // a Special Stage (scene folder UFO*): depths of its own
bool sUfoScene; // a Special Stage, depths or not (the floor window)
bool sTitleScene; // the title screen (scene folder Title): depths of its own (TitleDepth)
float sSlider; // 0..1
int8 sDepth;   // depth of the command being emitted (0 when not stereo: no extra batches)
float sEyeShift; // the eye being drawn: half the full disparity, signed (- left eye), times the slider (0: no 3D)
// Stereo depth scale: pixels of disparity at the full slider for the farthest things (a far background, the horizon).
// SITE SET stereofar N (saved).
#define STEREO_FAR ((int)GpuRender::stereoFar)
int8 sBehind;         // depth sprites take now: the nearest band of the last background layer drawn (0 on the playfield)
unsigned sPlayfieldEnd; // command index of the last playfield layer (depth 0) of the frame: sprites before it may sit behind
int8 sLayerNear, sLayerFar; // the depth range of the layer being emitted (its bands)
char sDepthCur[512], sDepthLast[512]; // SITE DEPTHS: each layer's depth range and the sprites' depths of the last 3D frame
unsigned sDepthLen;
unsigned sBehindSprites; // sprites given a background depth this frame (SITE DEPTHS)

// Stereo depth of a horizontal parallax factor (0x100: moves with the camera, the playfield, on the screen plane; 0:
// still, the farthest). Disparity grows as 1 - factor, as for things at distance Z with factor Zscreen / Z. Factors over
// 0x100 (a foreground nearer than the playfield) come out in front, at most half the scale.
int8 ParallaxDepth(int32 f)
{
    if (f < 0)
        f = 0;
    const int d    = (int)roundf((0x100 - f) * STEREO_FAR / 256.0f);
    const int front = -(STEREO_FAR / 2);
    return (int8)(d < front ? front : d);
}
// Pixels a layer is drawn past each screen edge in stereo: a shifted layer would leave the edge bare (the clear colour
// showed as a blue line). The farthest the current depth moves it in one eye, rounded up.
int EdgeMargin()
{
    if (!sDepths || !sStereo || sUfoStage)
        return 0;
    const int d = sDepth;
    return d ? (int)ceilf(0.5f * (d < 0 ? -d : d) * sSlider) + 1 : 0;
}
// The Special Stage's 3D scenes: the player's model is at Z ~ 41000 (SITE CMDS); things there sit on the screen plane.
const float STEREO_ZC = 41000.0f;
bool sRotoSoft; // this attempt draws rotozoom layers in software (the tile runs ran out of quads)

// Rotozoom layers up to 1024x1024 pixels (the title's island and clouds) are drawn whole, once, into a texture of
// their own in VRAM (repeating, as the layer wraps), and each screen line becomes one quad sampling it along the line:
// ~200 quads instead of ~3500 one-pixel tile runs. Rebuilt when the layer's palette bank, layout or tiles change,
// once they have held for two frames (a fade would rebuild it every frame: the tile runs draw it meanwhile).
// Colours that keep changing while the rest holds (the title's water, indices 140-143 rotated every 6 frames) become
// hot: the texture keeps whatever they were, an A8 mask holds hot colour k + 1 (x 32) where its pixels use them, and
// each frame every hot colour is painted over from the mask in its current colour (a pass of the same line quads).
enum { ROTO_HOT_MAX = 4 };
// A layer whose lines use several palette banks (the Special Stage's fog per line) gets a copy per bank used.
struct RotoBank {
    C3D_Tex tex;
    bool alloc, valid;
    uint32 built, seen; // content hash the copy holds / was seen last frame
    unsigned seenFrames;
    uint16 builtPal[PALETTE_BANK_SIZE]; // the bank the copy was built with
};
// What a RotoTex is drawn from: a rotozoom layer's tiles, or a sprite sheet (DrawDeformedSprite: UFO_Plasma).
struct RotoSrc {
    const TileLayer *layer;
    const GFXSurface *sheet;
    const void *Key() const { return layer ? (const void *)layer : (const void *)sheet; }
};
struct RotoTex {
    const void *key; // the layer or the sheet; NULL: a free slot
    unsigned w, h;
    RotoBank bank[8];
    C3D_Tex mask;
    bool maskAlloc, maskValid;
    uint8 hot[ROTO_HOT_MAX]; // hot palette indices
    unsigned hotCount;
    uint32 used[8]; // palette indices its pixels use (only those colours count as a change); all until usedOk
    bool usedOk;
    uint32 tileUsed[TILE_COUNT / 32]; // base tiles the layer's map uses (a layer; a sheet has none)
    uint32 tileGen;                   // bumped when one of those tiles changed (the tileset's epoch counts every tile)
    uint32 lastUsed; // sRotoFrame when last drawn
};
enum { ROTO_TEX_SLOTS = 3, ROTO_BANK_BUDGET = 1024 * 1024 };
// VRAM a Special Stage texture leaves free for the 3D models (on the 2DS the floor windows took it first and every
// frame with a 3D scene went to the CPU): what the scene's models not in VRAM yet will take (ModelVramNeed), and a
// margin. (A fixed 800 KB left no room for a third floor level: stage 2's models take ~600 KB.)
enum { VRAM_MARGIN = 32 * 1024 };
u32 sModelVram; // VRAM the 3D models hold now
u32 ModelVramNeed(const Model &md);

u32 VramReserve()
{
    u32 need = 0;
    for (int m = 0; m < MODEL_COUNT; ++m)
        need += ModelVramNeed(modelList[m]);
    return (need > sModelVram ? need - sModelVram : 0) + VRAM_MARGIN;
}
RotoTex sRotoTex[ROTO_TEX_SLOTS];
const void *sRotoTexFailed; // a layer or sheet whose texture could not be allocated (not tried again until the next scene)
uint32 sTileEpoch;               // bumped when animated tiles change the tileset
uint32 sTileChanged[TILE_COUNT / 32]; // base tiles whose pixels changed this frame (the floor windows check theirs)
bool sTileChangedAny;
char sRotoRuns[ROTO_TEX_SLOTS][32];         // per texture slot: its last layer's lines and the quads they took (SITE REFUSED)
const char *sRotoTexWhy = "-";   // why the last rotozoom layer was not drawn from its texture (SITE REFUSED)
uint32 sRotoFrame;               // GPU frames, to free textures no longer drawn

void FreeRotoTex(RotoTex &r)
{
    for (RotoBank &b : r.bank)
        if (b.alloc)
            C3D_TexDelete(&b.tex);
    if (r.maskAlloc)
        C3D_TexDelete(&r.mask);
    memset(&r, 0, sizeof(r));
}

struct SprSlot {
    uint64 key; // 0 = free
    uint16 x, y, w, h; // atlas position and padded (multiple of 8) size
    uint16 srcW, srcH;
    uint32 stamp;  // sGen when converted (0: never)
    uint32 groups; // palette groups its pixels use (see sGroupGen)
    uint32 conv;   // sGen of the cell's content (0: none): a reconversion rewrites only the colours changed since
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
// The rotozoom planes' program (roto.v.pica, DVLE 2): a run of lines as one quad with perspective (Batch::proj). Its
// projection and uvscale sit in the 2D program's registers (checked at init: otherwise the runs stay one quad a line).
shaderProgram_s sRotoProg;
int sRotoEyeLoc = -1;
bool sRotoProgOk;
C3D_AttrInfo sAttr;
C3D_BufInfo sBufInfo;
Vtx *sVbo[2];
u16 *sIdx;
unsigned sCur;

// ---- 3D scenes (GpuScene3D, scene3d.v.pica)
// A model's vertices (every frame: x, y, z, nx / 4, ny / 4, nz / 4 as int16), colours and triangle indices live in VRAM,
// copied there once per scene content by the transfer engine (the CPU cannot write VRAM from an installed title).
struct GModel {
    u16 *vtx;
    u8 *col; // RGBA bytes per vertex, NULL: the instance colour
    u16 *idx;
    u32 idxCount;
    u32 vbytes, ibytes, cbytes; // the VRAM blocks' sizes (SITE MODELCHECK)
    u16 vc;
    bool tried, ok;
    float radius; // the farthest vertex from the model's origin, any frame (model units): its scene's depth range
};
GModel sModels[MODEL_COUNT];
unsigned sModelsVersion = ~0u;
DVLB_s *s3Dvlb;
shaderProgram_s s3Prog;
bool s3Ready;
struct {
    int proj, wrow, nrow, lerp, scr, ortho, band, lit, dif, difs, spcs, eye;
} s3Loc;
C3D_AttrInfo s3AttrCol, s3AttrFixed;
struct Draw3D {
    const GpuScene3D::Head *h;
    const GpuScene3D::Inst *in;
};
enum { MAX_DRAW3D = 256 }; // the second Special Stage draws over 64 scenes a frame (one per model group)
Draw3D sDraw3D[MAX_DRAW3D];
unsigned sDraw3DCount;

// Per-frame build state
unsigned sQuadCount;
Batch sBatches[MAX_BATCHES];
unsigned sBatchCount;
Pending sPending[MAX_PENDING];
unsigned sPendingCount;
unsigned sSeenContent = ~0u;
GpuRender::Stats sStats;
float sC3DSum[5];      // SITE C3D: sums since the last TakeC3DTimes
unsigned sC3DFrames;

// Palette change tracking. Every frame the palette is compared with a copy of the last one; the colours are
// taken in 32 groups of 8 and each group remembers the generation (frame) it last changed in. A converted tile or
// sprite keeps the generation it was converted in and the groups its pixels use, and is converted again only when
// one of those groups changed since: a palette cycle on a few colours no longer reconverts everything on screen.
// The comparison catches every writer of fullPalette, hooked or not.
uint32 sGen = 1;
uint16 sPalShadow[BANKS][PALETTE_BANK_SIZE];
u16 sTexPal[BANKS][PALETTE_BANK_SIZE]; // the palette as atlas texels (index 0 transparent), kept with the shadow
uint32 sGroupGen[BANKS][32];
uint32 sColorGen[BANKS][PALETTE_BANK_SIZE]; // the same per colour, for the partial sprite reconversions
// The bank a command's palette bank stands for: itself, or the virtual bank (8..15) holding the snapshot that bank had
// when the command was recorded. Virtual banks are handed out in order of appearance each frame, so a boss flashing
// the same way every frame keeps its atlas copies.
uint8 sBankMap[REAL_BANKS];
unsigned sVirtUsed;

// Tile cache: one cell per (bank, tile id)
int16 sTileCell[BANKS][TILE_IDS];
uint32 sTileStamp[BANKS][TILE_IDS];
uint32 sTileGroups[TILE_IDS];  // palette groups the tile's pixels use
uint8 sTileGroupsOk[TILE_IDS]; // sTileGroups is known (computed once per tile content, not per conversion)
uint32 sTileHash[TILE_COUNT];  // content of each base tile when DrawAniTile last touched it (it rewrites unchanged frames too)
bool sSprFull;                 // the last ATLAS_FULL came from the sprite atlas
unsigned sTileCellsUsed;
int16 sFreeCells[TILE_CELLS]; // cells given back by EvictTiles
unsigned sFreeCount;

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
    ++sTileEpoch;
    memset(sTileCell, 0xFF, sizeof(sTileCell)); // -1
    memset(sTileStamp, 0, sizeof(sTileStamp));
    memset(sTileHash, 0, sizeof(sTileHash));
    memset(sTileGroupsOk, 0, sizeof(sTileGroupsOk));
    sTileCellsUsed = 0;
    sFreeCount     = 0;
    ++sGen; // everything converted from now on is newer than any group change so far
    ResetSprites();
}

void TrackPalette(bool all = false, const uint16 (*base)[256] = NULL);

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
    // Optional: without it the software layers are refused as before. In VRAM: only the GPU reads it and the transfer
    // engine fills it, and the linear heap had no 256 KB left for it (ROTOZOOM LAYER frames in the Special Stage).
    if (C3D_TexInitVRAM(&sSoftTex, SOFT_W, SOFT_H, GPU_RGBA5551) || C3D_TexInit(&sSoftTex, SOFT_W, SOFT_H, GPU_RGBA5551)) {
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

    // One binary for both programs (gpu.v.pica DVLE 0, scene3d.v.pica DVLE 1): switching between programs that share
    // their code uploads no shader code (a 3D scene between 2D batches cost ~1.3 KB of commands with two binaries).
    sDvlb = DVLB_ParseFile((u32 *)renderer_shbin, sizeof(renderer_shbin));
    if (!sDvlb || sDvlb->numDVLE < 2)
        return false;
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    sProjLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");
    sUvLoc   = shaderInstanceGetUniformLocation(sProgram.vertexShader, "uvscale");

    if (sDvlb->numDVLE >= 3) {
        shaderProgramInit(&sRotoProg);
        shaderProgramSetVsh(&sRotoProg, &sDvlb->DVLE[2]);
        sRotoEyeLoc = shaderInstanceGetUniformLocation(sRotoProg.vertexShader, "eye");
        sRotoProgOk = sRotoEyeLoc >= 0 && shaderInstanceGetUniformLocation(sRotoProg.vertexShader, "projection") == sProjLoc &&
                      shaderInstanceGetUniformLocation(sRotoProg.vertexShader, "uvscale") == sUvLoc;
    }

    AttrInfo_Init(&sAttr);
    AttrInfo_AddLoader(&sAttr, 0, GPU_SHORT, 4);         // x, y (quarter pixels), u, v (texels)
    AttrInfo_AddLoader(&sAttr, 1, GPU_UNSIGNED_BYTE, 4); // r, g, b, a

    // The 3D scene program (optional: without it the scenes go to the software)
    s3Dvlb = sDvlb;
    if (s3Dvlb) {
        shaderProgramInit(&s3Prog);
        shaderProgramSetVsh(&s3Prog, &s3Dvlb->DVLE[1]);
        const shaderInstance_s *vs = s3Prog.vertexShader;
        s3Loc.proj = shaderInstanceGetUniformLocation(vs, "projection");
        s3Loc.wrow = shaderInstanceGetUniformLocation(vs, "wrow");
        s3Loc.nrow = shaderInstanceGetUniformLocation(vs, "nrow");
        s3Loc.lerp = shaderInstanceGetUniformLocation(vs, "lerp");
        s3Loc.scr  = shaderInstanceGetUniformLocation(vs, "scr");
        s3Loc.ortho = shaderInstanceGetUniformLocation(vs, "ortho");
        s3Loc.band = shaderInstanceGetUniformLocation(vs, "band");
        s3Loc.lit  = shaderInstanceGetUniformLocation(vs, "lit");
        s3Loc.dif  = shaderInstanceGetUniformLocation(vs, "dif");
        s3Loc.difs = shaderInstanceGetUniformLocation(vs, "difs");
        s3Loc.spcs = shaderInstanceGetUniformLocation(vs, "spcs");
        s3Loc.eye  = shaderInstanceGetUniformLocation(vs, "eye");
        for (int k = 0; k < 2; ++k) {
            C3D_AttrInfo *a = k ? &s3AttrFixed : &s3AttrCol;
            AttrInfo_Init(a);
            AttrInfo_AddLoader(a, 0, GPU_SHORT, 3); // position, frame A
            AttrInfo_AddLoader(a, 1, GPU_SHORT, 3); // normal / 4, frame A
            AttrInfo_AddLoader(a, 2, GPU_SHORT, 3); // position, frame B
            AttrInfo_AddLoader(a, 3, GPU_SHORT, 3); // normal / 4, frame B
            if (k)
                AttrInfo_AddFixed(a, 4); // colour of the instance
            else
                AttrInfo_AddLoader(a, 4, GPU_UNSIGNED_BYTE, 4);
        }
        s3Ready = true;
    }

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

// The water line: lines y0..y1 use more than one palette bank. `emit` is called once per run of lines with one bank,
// with the command's clip cut to that run (it then sees one bank, and draws only those lines).
template <typename Emit> bool PerBankRun(const Cmd &c, int y0, int y1, Emit emit)
{
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > SCREEN_YSIZE ? SCREEN_YSIZE : y1;
    for (int ya = y0; ya < y1;) {
        int yb = ya + 1;
        while (yb < y1 && sLineBuf[yb] == sLineBuf[ya])
            ++yb;
        Cmd cc    = c;
        cc.clip[1] = (int16)(ya > c.clip[1] ? ya : c.clip[1]);
        cc.clip[3] = (int16)(yb < c.clip[3] ? yb : c.clip[3]);
        if (cc.clip[1] < cc.clip[3] && !emit(cc))
            return false;
        ya = yb;
    }
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
// Bank b of the cache now holds `pal`: the groups that changed get this generation.
void TrackBank(unsigned b, const uint16 *pal, bool all)
{
    const uint32 *now = (const uint32 *)pal, *was = (const uint32 *)sPalShadow[b];
    if (!all && !memcmp(now, was, sizeof(sPalShadow[b])))
        return;
    for (unsigned g = 0; g < 32; ++g) // a group is 8 colours, 4 words
        if (all || now[g * 4] != was[g * 4] || now[g * 4 + 1] != was[g * 4 + 1] || now[g * 4 + 2] != was[g * 4 + 2] || now[g * 4 + 3] != was[g * 4 + 3]) {
            sGroupGen[b][g] = sGen;
            for (unsigned i = g * 8; i < g * 8 + 8; ++i)
                if (all || pal[i] != sPalShadow[b][i])
                    sColorGen[b][i] = sGen;
        }
    memcpy(sPalShadow[b], pal, sizeof(sPalShadow[b]));
    sTexPal[b][0] = 0;
    for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
        sTexPal[b][i] = ToTexel(pal[i]);
}

// `base`: the palette the frame started with (it changed while recording), else the current one.
void TrackPalette(bool all, const uint16 (*base)[256])
{
    ++sGen;
    for (unsigned b = 0; b < (all ? (unsigned)BANKS : (unsigned)REAL_BANKS); ++b)
        TrackBank(b, base ? base[b % REAL_BANKS] : fullPalette[b % REAL_BANKS], all);
}

// CMD_PALETTE: each bank that differs from what the frame started with maps to a virtual bank holding it.
bool ApplySnapshot(const uint16 (*pal)[256])
{
    for (unsigned b = 0; b < REAL_BANKS; ++b) {
        if (!memcmp(pal[b], sPalShadow[b], sizeof(sPalShadow[b]))) {
            sBankMap[b] = (uint8)b;
            continue;
        }
        unsigned v = REAL_BANKS;
        for (; v < REAL_BANKS + sVirtUsed; ++v) // the same snapshot again (Super Sonic's colours, then restored)
            if (!memcmp(pal[b], sPalShadow[v], sizeof(sPalShadow[v])))
                break;
        if (v == REAL_BANKS + sVirtUsed) {
            if (sVirtUsed >= BANKS - REAL_BANKS)
                return false;
            ++sVirtUsed;
            TrackBank(v, pal[b], false);
        }
        sBankMap[b] = (uint8)v;
    }
    return true;
}

// The tile atlas is full: give back the cells of every (bank, tile) not used in this frame. The Special Stage floor
// sees most of its 756 tiles over a run, each in up to 7 palette banks (the fog), but only a few hundred at a time.
void EvictTiles()
{
    for (unsigned b = 0; b < BANKS; ++b)
        for (unsigned id = 0; id < TILE_IDS; ++id)
            if (sTileCell[b][id] >= 0 && sTileStamp[b][id] != sGen) {
                sFreeCells[sFreeCount++] = sTileCell[b][id];
                sTileCell[b][id]         = -1;
                sTileStamp[b][id]        = 0;
            }
    ++sStats.tileEvictions;
}

// A tile whose palette group changed after `stamp` still has its picture if none of the colours it uses did (Press Garden
// cycles 232-239 each frame: groups are 8 colours, most tiles use only some of them).
bool TileColorsChanged(unsigned bank, unsigned id, uint32 stamp)
{
    const uint8 *px = &tilesetPixels[TILE_DATASIZE * id];
    for (unsigned i = 0; i < TILE_DATASIZE; ++i)
        if (px[i] && sColorGen[bank][px[i]] > stamp)
            return true;
    return false;
}

// Atlas cell of a tile with the palette bank applied; queues its conversion when it is new or stale.
int EnsureTile(unsigned id, unsigned bank)
{
    if (bank < REAL_BANKS)
        bank = sBankMap[bank];
    if (id >= TILE_IDS || bank >= BANKS)
        return -1;
    int cell = sTileCell[bank][id];
    if (cell < 0) {
        if (!sFreeCount && sTileCellsUsed >= TILE_CELLS)
            EvictTiles();
        if (sFreeCount)
            cell = sFreeCells[--sFreeCount];
        else if (sTileCellsUsed < TILE_CELLS)
            cell = (int)sTileCellsUsed++;
        else
            return -2; // atlas full: every cell is in use this frame
        sTileCell[bank][id]  = (int16)cell;
        sTileStamp[bank][id] = 0;
    }
    const uint32 stamp = sTileStamp[bank][id];
    if (stamp == sGen)
        return cell;
    // Unchanged colours: the copy is as good as one made now, so it takes this generation and the tile's other
    // uses this frame (the same water tile on many lines) stop at the test above.
    if (stamp && (!GroupsChanged(bank, sTileGroups[id], stamp) || !TileColorsChanged(bank, id, stamp))) {
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
    // bit 0 set (0 is a free slot), sprY bits 1-16, sprX 17-30, h 31-41, w 42-52, bank 53-56, sheet 57-62. sprY has 16
    // bits: sheet 0 is the whole tileset as a 16-pixel-wide column (65536 rows), which the collapsing floors draw from.
    return 1ull | ((uint64)sheet << 57) | ((uint64)bank << 53) | ((uint64)w << 42) | ((uint64)h << 31) | ((uint64)sprX << 17) | ((uint64)sprY << 1);
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
    if (best < 0 && (sShelfNextY + h8 > SPR_ATLAS_H || sShelfCount >= SPR_ATLAS_H / 8)) {
        // No room for a new shelf: any taller shelf with room will do (it wastes its extra height, but the atlas
        // filled up with half-empty shelves of many heights in the title screen: ATLAS FULL several times a second).
        for (unsigned s = 0; s < sShelfCount; ++s) {
            Shelf &sh = sShelves[s];
            if (sh.h >= h8 && sh.xUsed + w8 <= SPR_ATLAS_W && (best < 0 || sh.h < sShelves[best].h))
                best = (int)s;
        }
        if (best < 0)
            return false;
    }
    if (best < 0) {
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

#if MANIA_DEV_TOOLS
// SITE CONV: the last frame's conversions, to tell a real colour change from a group that changed elsewhere. Each
// conversion counts the texels that came out different from what the cell held; the biggest sprites are listed.
struct ConvEntry {
    uint16 sheet, w, h, bank;
    uint32 groups, changed;
    bool fresh;
};
ConvEntry sConvTop[6];
unsigned sConvTopCount, sConvTiles, sConvTilesSame, sConvSprites, sConvSpritesSame, sConvSpritePixels, sConvSpriteChanged;
char sConvReport[600] = "-";
#endif

SprSlot *EnsureSprite(unsigned sheet, unsigned bank, int sprX, int sprY, int w, int h, int *refuse)
{
    *refuse   = 0;
    if (bank < REAL_BANKS)
        bank = sBankMap[bank];
    sBadData  = sheet >= SURFACE_COUNT ? 1 : !gfxSurface[sheet].pixels ? 2 : (sprX < 0 || sprY < 0) ? 3 : (w <= 0 || h <= 0) ? 4
                : (sprX >= 16384 || sprY >= 65536 || w >= 2048 || h >= 2048) ? 5 : bank >= BANKS ? 6 : 0;
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
            s.conv   = 0;
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
    const unsigned X = (p.cell % (TILE_ATLAS_W / 16)) * 16, Y = (p.cell / (TILE_ATLAS_W / 16)) * 16;
    const u16 *tex   = sTexPal[p.bank];
    if (!sTileGroupsOk[p.id]) {
        uint32 groups = 0;
        for (unsigned i = 0; i < TILE_DATASIZE; ++i)
            groups |= (src[i] ? 1u : 0u) << (src[i] >> 3);
        sTileGroups[p.id]   = groups;
        sTileGroupsOk[p.id] = 1;
    }
    // Four 8x8 blocks, each 64 texels in Morton order: Morton(x, y) = mx[x] | my[y] (the bits interleave).
    static const uint8 my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
#if MANIA_DEV_TOOLS
    unsigned changed = 0;
#endif
    for (unsigned by = 0; by < 2; ++by)
        for (unsigned bx = 0; bx < 2; ++bx) {
            u16 *blk        = &sTileData[(((Y >> 3) + by) * (TILE_ATLAS_W >> 3) + (X >> 3) + bx) << 6];
            const uint8 *sp = src + by * 8 * 16 + bx * 8;
            for (unsigned y = 0; y < 8; ++y, sp += 16) {
                u16 *b = blk + my[y];
#if MANIA_DEV_TOOLS
                changed |= (b[0] ^ tex[sp[0]]) | (b[1] ^ tex[sp[1]]) | (b[4] ^ tex[sp[2]]) | (b[5] ^ tex[sp[3]]) | (b[16] ^ tex[sp[4]])
                           | (b[17] ^ tex[sp[5]]) | (b[20] ^ tex[sp[6]]) | (b[21] ^ tex[sp[7]]);
#endif
                b[0] = tex[sp[0]], b[1] = tex[sp[1]], b[4] = tex[sp[2]], b[5] = tex[sp[3]];
                b[16] = tex[sp[4]], b[17] = tex[sp[5]], b[20] = tex[sp[6]], b[21] = tex[sp[7]];
            }
        }
#if MANIA_DEV_TOOLS
    ++sConvTiles;
    sConvTilesSame += !changed;
#endif
    Dirty(sTileDirtyLo, sTileDirtyHi, TILE_ATLAS_W, X, Y, 16, 16);
    ++sStats.tileConversions;
}

void ConvertSprite(const Pending &p)
{
    SprSlot &s = sSlots[p.id];
    // The key holds the sheet position: unpack it.
    const unsigned sheet = (unsigned)(s.key >> 57), sprX = (unsigned)((s.key >> 17) & 0x3FFF), sprY = (unsigned)((s.key >> 1) & 0xFFFF);
    const GFXSurface &g  = gfxSurface[sheet];
    const u16 *tex       = sTexPal[p.bank]; // the bank as the frame used it (a virtual one: a snapshot)
    // The part of the cell the sheet covers; the rest stays transparent
    // (DrawTile draws a tile as a sprite of sheet 0 at row 16 * (tile & 0xFFF): the flip bits put it past the sheet's
    // height, in the flipped copies tilesetPixels keeps after the plain ones: Stardust Speedway's frames.)
    const int gHeight = sheet == 0 ? g.height * 4 : g.height;
    const unsigned w = (int)(sprX + s.srcW) <= g.width ? s.srcW : g.width > (int)sprX ? g.width - sprX : 0;
    const unsigned h = (int)(sprY + s.srcH) <= gHeight ? s.srcH : gHeight > (int)sprY ? gHeight - sprY : 0;
    static const uint8 my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    static const uint8 mx[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
    unsigned written = 0;
    if (s.conv) {
        // The cell holds the conversion of generation s.conv: rewrite only the texels of the colours changed since.
        // Studiopolis' shop windows (256x256) share a group with a pulsing colour: ~100 of their texels change.
        bool dirty[PALETTE_BANK_SIZE];
        bool any = false;
        dirty[0] = false;
        for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
            any |= dirty[i] = sColorGen[p.bank][i] > s.conv;
        if (any)
            for (unsigned y = 0; y < h; ++y) {
                const uint8 *src = &g.pixels[(sprY + y) * g.width + sprX];
                const unsigned Y = s.y + y;
                u16 *row         = &sSprData[((Y >> 3) * (SPR_ATLAS_W >> 3) + (s.x >> 3)) << 6] + my[Y & 7];
                for (unsigned x = 0; x < w; ++x)
                    if (dirty[src[x]]) {
                        row[((x >> 3) << 6) + mx[x & 7]] = tex[src[x]];
                        ++written;
                    }
            }
    }
    else {
        // A whole conversion, by 8x8 blocks of the atlas (the PICA's Morton order inside each)
        uint32 groups = 0;
        for (unsigned by = 0; by < s.h; by += 8)
            for (unsigned bx = 0; bx < s.w; bx += 8) {
                u16 *blk = &sSprData[(((s.y + by) >> 3) * (SPR_ATLAS_W >> 3) + ((s.x + bx) >> 3)) << 6];
                for (unsigned y = 0; y < 8; ++y) {
                    u16 *b = blk + my[y];
                    if (by + y >= h) {
                        for (unsigned x = 0; x < 8; ++x)
                            b[mx[x]] = 0;
                        continue;
                    }
                    const uint8 *src = &g.pixels[(sprY + by + y) * g.width + sprX + bx];
                    for (unsigned x = 0; x < 8; ++x) {
                        const unsigned idx = bx + x < w ? src[x] : 0;
                        groups |= (idx ? 1u : 0u) << (idx >> 3);
                        b[mx[x]] = tex[idx];
                    }
                }
            }
        s.groups = groups;
        written  = s.w * s.h;
    }
    s.conv = sGen;
#if MANIA_DEV_TOOLS
    ++sConvSprites;
    sConvSpritesSame += !written;
    sConvSpritePixels += s.w * s.h;
    sConvSpriteChanged += written;
    {
        // Keep the biggest few, largest first
        const ConvEntry e = { (uint16)sheet, s.w, s.h, p.bank, s.groups, written, written == (unsigned)s.w * s.h };
        unsigned k = sConvTopCount < 6 ? sConvTopCount++ : 6;
        if (k == 6 && sConvTop[5].w * sConvTop[5].h < e.w * e.h)
            k = 5;
        if (k < 6) {
            while (k > 0 && sConvTop[k - 1].w * sConvTop[k - 1].h < e.w * e.h) {
                sConvTop[k] = sConvTop[k - 1];
                --k;
            }
            sConvTop[k] = e;
        }
    }
#endif
    if (written)
        Dirty(sSprDirtyLo, sSprDirtyHi, SPR_ATLAS_W, s.x, s.y, s.w, s.h);
    ++sStats.spriteConversions;
}

// ---------------------------------------------------------------- geometry

bool AddQuad(uint8 tex, uint8 blend, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, unsigned atlasW, unsigned atlasH,
             uint8 r, uint8 g, uint8 b, uint8 a, uint32 constant = 0)
{
    if (sQuadCount >= MAX_QUADS)
        return false;
    if (!sBatchCount || sBatches[sBatchCount - 1].tex != tex || sBatches[sBatchCount - 1].blend != blend || blend == BLEND_FILL ||
        sBatches[sBatchCount - 1].depth != sDepth || sBatches[sBatchCount - 1].scissor != sScissor
        || (blend == BLEND_TINT && sBatches[sBatchCount - 1].constant != constant)) {
        if (sBatchCount >= MAX_BATCHES)
            return false;
        sBatches[sBatchCount++] = { tex, blend, sQuadCount, 0, constant, sDepth, sScissor };
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

// A tile quad: integer screen pixels and atlas texels (the hot path: thousands a frame).
inline bool AddTileQuad(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1)
{
    if (sQuadCount >= MAX_QUADS)
        return false;
    if (!sBatchCount || sBatches[sBatchCount - 1].tex != TEX_TILE || sBatches[sBatchCount - 1].blend != BLEND_NORMAL ||
        sBatches[sBatchCount - 1].depth != sDepth || sBatches[sBatchCount - 1].scissor != sScissor) {
        if (sBatchCount >= MAX_BATCHES)
            return false;
        sBatches[sBatchCount++] = { TEX_TILE, BLEND_NORMAL, sQuadCount, 0, 0, sDepth, sScissor };
    }
    ++sBatches[sBatchCount - 1].count;
    Vtx *v                     = &sVbo[sCur][sQuadCount++ * 4];
    const int16 X0 = (int16)(x0 << 2), X1 = (int16)(x1 << 2), Y0 = (int16)(y0 << 2), Y1 = (int16)(y1 << 2);
    v[0] = { X0, Y0, (int16)u0, (int16)v0, 255, 255, 255, 255 };
    v[1] = { X1, Y0, (int16)u1, (int16)v0, 255, 255, 255, 255 };
    v[2] = { X1, Y1, (int16)u1, (int16)v1, 255, 255, 255, 255 };
    v[3] = { X0, Y1, (int16)u0, (int16)v1, 255, 255, 255, 255 };
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

// ---------------------------------------------------------------- INK_TINT

// The frame drawn so far, copied here by a BLEND_TINT batch (the render target's tiled RGB565 buffer, 240 x 400, into a
// texture 256 x 512 with the same tile rows). Made at the first tint of a scene, freed with the scene's other textures.
C3D_Tex sGrabTex;
bool sGrabReady;

bool EnsureGrab()
{
    if (sGrabReady)
        return true;
    // VRAM first (only the GPU touches it), then the linear heap
    sGrabReady = C3D_TexInitVRAM(&sGrabTex, 256, 512, GPU_RGB565) || C3D_TexInit(&sGrabTex, 256, 512, GPU_RGB565);
    if (sGrabReady) {
        C3D_TexSetFilter(&sGrabTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sGrabTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    return sGrabReady;
}

void FreeGrab()
{
    if (sGrabReady)
        C3D_TexDelete(&sGrabTex);
    sGrabReady = false;
}

// The tint table as the combiner can do it: every colour to a grey min(255, k * (r + g + b) + c) on 8-bit channels (the
// Plus pause menu: (r + g + b) * 256 / 680; the engine's default: the 5-bit average + 6). k and c in 1/255 come back
// packed as Batch::constant; false for other tables (FXRuby's inversion, the pre-Plus pause menu's HSL), which stay on
// the CPU. Checked on samples every time: a game may rewrite its table.
bool TintModel(const uint16 *table, uint32 *packed)
{
    auto widen = [](uint16 c, int *r, int *g, int *b) {
        const int r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        *r = (r5 << 3) | (r5 >> 2), *g = (g6 << 2) | (g6 >> 4), *b = (b5 << 3) | (b5 >> 2);
    };
    int r, g, b, sr, sg, sb;
    widen(table[0], &r, &g, &b);
    const int c = r; // black: the offset alone
    const uint16 mid = 0x4208; // a dark grey (8, 16, 8): far below the cap
    widen(mid, &sr, &sg, &sb);
    widen(table[mid], &r, &g, &b);
    const int sum = sr + sg + sb;
    const int k = ((r - c) * 255 + sum / 2) / sum;
    if (c < 0 || c > 255 || k <= 0 || k > 255)
        return false;
    uint32 x = 12345;
    for (int n = 0; n < 256; ++n) {
        x = x * 1103515245u + 12345u;
        const uint16 i = n < 2 ? (n ? 0xFFFF : 0x0000) : (uint16)(x >> 16);
        widen(i, &sr, &sg, &sb);
        widen(table[i], &r, &g, &b);
        int want = (k * (sr + sg + sb) + 127) / 255 + c;
        want     = want > 255 ? 255 : want;
        // One 5-bit step and a half of slack: the tables round in 5-bit units, the combiner in 8-bit ones.
        if (abs(r - want) > 12 || abs(g - want) > 12 || abs(b - want) > 12)
            return false;
    }
    *packed = (uint32)k | ((uint32)c << 8);
    return true;
}

// A tint quad of four free corners: texel coordinates are the grab texture's own for the screen point (the target is
// the top screen turned: its memory row is the screen x, its column 240 minus the screen y). Never shifted for an eye: it
// must read the place it covers.
bool AddTintQuad(const float *px, const float *py, uint32 packed)
{
    const int8 depth = sDepth;
    sDepth           = 0;
    const bool ok    = AddQuad(TEX_SPRITE, BLEND_TINT, 0, 0, 0, 0, 0, 0, 0, 0, 256, 512, 255, 255, 255, 255, packed);
    sDepth           = depth;
    if (!ok)
        return false;
    Vtx *q = &sVbo[sCur][(sQuadCount - 1) * 4];
    for (int j = 0; j < 4; ++j)
        q[j].x = Quarter(px[j]), q[j].y = Quarter(py[j]), q[j].u = (int16)(240.0f - py[j] + 0.5f), q[j].v = (int16)(px[j] + 0.5f);
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
        case INK_MASKED: *blend = BLEND_MASKED, *alpha = 0xFF; return true;
        case INK_UNMASKED: *blend = BLEND_UNMASKED, *alpha = 0xFF; return true;
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
    // Stereo: a piece at a depth is shifted sideways, so with the clip the whole screen wide it is cut a margin past each
    // edge, not at the edge (the title's mountains lost a strip at one edge in each eye)
    const int32 mx  = c.clip[0] <= 0 && c.clip[2] >= 400 ? EdgeMargin() : 0;
    const int32 vx0 = X0 > c.clip[0] - mx ? X0 : c.clip[0] - mx, vx1 = X0 + W0 < c.clip[2] + mx ? X0 + W0 : c.clip[2] + mx;
    const int32 vy0 = Y0 > c.clip[1] ? Y0 : c.clip[1], vy1 = Y0 + H0 < c.clip[3] ? Y0 + H0 : c.clip[3];
    if (vx0 >= vx1 || vy0 >= vy1)
        return true;
    const int32 su0 = fx ? sprX0 + (X0 + W0 - vx1) : sprX0 + (vx0 - X0), su1 = su0 + (vx1 - vx0);
    const int32 sv0 = fy ? sprY0 + (Y0 + H0 - vy1) : sprY0 + (vy0 - Y0), sv1 = sv0 + (vy1 - vy0);
    uint8 bank;
    if (!UniformBank(vy0, vy1, &bank))
        return PerBankRun(c, vy0, vy1, [&](const Cmd &cc) {
            return EmitSpriteChunked(cc, X0, Y0, W0, H0, sprX0, sprY0, direction, sheetID, blend, alpha);
        });
    const GFXSurface &g = gfxSurface[sheetID < SURFACE_COUNT ? sheetID : 0];
    for (int32 cv = sv0 - (sv0 % CHUNK); cv < sv1; cv += CHUNK) {
        for (int32 cu = su0 - (su0 % CHUNK); cu < su1; cu += CHUNK) {
            // The chunk cached is the grid cell cut to the sprite's frame and the sheet (a 512x32 strip takes 128x32
            // chunks, not 128x128: the menu's diorama filled the atlas); the visible part of it is drawn.
            int32 ku0 = cu > sprX0 ? cu : sprX0, ku1 = cu + CHUNK < sprX0 + W0 ? cu + CHUNK : sprX0 + W0;
            int32 kv0 = cv > sprY0 ? cv : sprY0, kv1 = cv + CHUNK < sprY0 + H0 ? cv + CHUNK : sprY0 + H0;
            ku1 = ku1 < g.width ? ku1 : g.width;
            kv1 = kv1 < g.height ? kv1 : g.height;
            const int32 u0 = su0 > ku0 ? su0 : ku0, u1 = su1 < ku1 ? su1 : ku1;
            const int32 v0 = sv0 > kv0 ? sv0 : kv0, v1 = sv1 < kv1 ? sv1 : kv1;
            if (u0 >= u1 || v0 >= v1)
                continue;
            int refuse;
            SprSlot *slot = EnsureSprite((unsigned)sheetID, bank, ku0, kv0, ku1 - ku0, kv1 - kv0, &refuse);
            if (!slot)
                return Refuse((GpuRender::Reason)refuse);
            const float dx0 = (float)(fx ? X0 + (sprX0 + W0 - u1) : X0 + (u0 - sprX0)), dx1 = dx0 + (float)(u1 - u0);
            const float dy0 = (float)(fy ? Y0 + (sprY0 + H0 - v1) : Y0 + (v0 - sprY0)), dy1 = dy0 + (float)(v1 - v0);
            float tu0 = (float)(slot->x + (u0 - ku0)), tu1 = (float)(slot->x + (u1 - ku0));
            float tv0 = (float)(slot->y + (v0 - kv0)), tv1 = (float)(slot->y + (v1 - kv0));
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
        case INK_MASKED: blend = BLEND_MASKED, alpha = 0xFF; break;
        case INK_UNMASKED: blend = BLEND_UNMASKED, alpha = 0xFF; break;
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
    // Stereo: with the clip the whole screen wide the sprite is cut a margin past each edge (a depth shifts it sideways)
    const int32 mx = c.clip[0] <= 0 && c.clip[2] >= 400 ? EdgeMargin() : 0;
    if (width + x > c.clip[2] + mx)
        width = c.clip[2] + mx - x;
    if (x < c.clip[0] - mx) {
        const int32 val = x - (c.clip[0] - mx);
        sprX -= val;
        width += val;
        widthFlip += 2 * val;
        x = c.clip[0] - mx;
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
        return PerBankRun(c, y, y + height, [](const Cmd &cc) { return EmitSprite(cc); });
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
    // Lines in more than one palette bank (the water line): once per run of one bank, each scissored to its lines.
    // (Only the lines inside the clip count: a run's call sees its own clip, all one bank.)
    const int by0 = (int)minY > c.clip[1] ? (int)minY : c.clip[1], by1 = (int)maxY + 1 < c.clip[3] ? (int)maxY + 1 : c.clip[3];
    uint8 bank;
    if (!UniformBank(by0, by1, &bank))
        return PerBankRun(c, by0, by1, [](const Cmd &cc) { return EmitRoto(cc); });
    // A sprite crossing a clip rectangle smaller than the screen is scissored to it.
    const bool crosses = (minX < c.clip[0] && c.clip[0] > 0) || (minY < c.clip[1] && c.clip[1] > 0) ||
                         (maxX > c.clip[2] && c.clip[2] < screens[0].size.x) || (maxY > c.clip[3] && c.clip[3] < screens[0].size.y);
    if (crosses && !ScissorFor(c.clip))
        return Refuse(GpuRender::REASON_UNSUPPORTED, CMD_ROTO);
    int refuse;
    SprSlot *slot = EnsureSprite((unsigned)sheetID, bank, sprX, sprY, width, height, &refuse);
    if (!slot)
        return Refuse((GpuRender::Reason)refuse);
    for (int i = 0; i < 4; ++i) {
        tu[i] = slot->x + (rx[i] - pivotX);
        tv[i] = slot->y + (ry[i] - pivotY);
    }
    const bool added = AddQuadFree(TEX_SPRITE, blend, px, py, tu, tv, SPR_ATLAS_W, SPR_ATLAS_H, 255, 255, 255, (uint8)alpha);
    sScissor         = 0;
    if (!added)
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
    bool skip = false;
    uint32 tint = 0;
    if (ink == INK_TINT) {
        if (!(const uint16 *)tintLookupTable)
            return true;
        if (!TintModel((const uint16 *)tintLookupTable, &tint) || !EnsureGrab())
            return Refuse(GpuRender::REASON_INK, (unsigned)ink);
        blend = BLEND_TINT;
    }
    else if (!SpriteInk(ink, &alpha, &blend, &skip))
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
        if (!(blend == BLEND_TINT ? AddTintQuad(px, py, tint) : AddSolidQuad(blend, px, py, r, g, b, (uint8)alpha)))
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

// ---------------------------------------------------------------- 3D scenes

void FreeModels()
{
    for (GModel &g : sModels) {
        if (g.vtx)
            vramFree(g.vtx);
        if (g.col)
            vramFree(g.col);
        if (g.idx)
            vramFree(g.idx);
    }
    memset(sModels, 0, sizeof(sModels));
    sModelVram = 0;
}

// Copies `bytes` (a multiple of 16) from linear memory to VRAM with the transfer engine, as citro3d's
// C3D_TexLoadImage does for a VRAM texture (dimensions 0, flags 8: a plain texture copy). The first try passed the size
// as the line width and the console froze on entering the Special Stage (not yet known which part hung).
void CopyToVram(const void *src, void *dst, u32 bytes)
{
    GSPGPU_FlushDataCache(src, bytes);
    C3D_SyncTextureCopy((u32 *)src, 0, (u32 *)dst, 0, bytes, 8);
}

// Fills a VRAM block of `bytes` through the 256 KB staging buffer, `fill` writing each chunk (offset, size, buffer).
template <typename F> bool UploadVram(void *dst, u32 bytes, F fill)
{
    u8 *staging = (u8 *)C3DPresent::Staging();
    if (!staging)
        return false;
    const u32 chunk = SOFT_W * SOFT_H * 2;
    for (u32 off = 0; off < bytes; off += chunk) {
        const u32 n = bytes - off < chunk ? bytes - off : chunk;
        C3DPresent::StagingWait(); // the last chunk's copy may still be reading it
        fill(off, n, staging);
        CopyToVram(staging, (u8 *)dst + off, (n + 15) & ~15u);
    }
    return true;
}

// The VRAM EnsureModel takes for a model (0: not drawable, as EnsureModel decides).
u32 ModelVramNeed(const Model &md)
{
    const unsigned frames = md.frameCount ? md.frameCount : 1, vc = md.vertCount;
    if (!md.vertices || !md.indices || !vc || !md.indexCount || (md.faceVertCount != 3 && md.faceVertCount != 4))
        return 0;
    const unsigned faces = md.indexCount / md.faceVertCount;
    const u32 idx        = faces * (md.faceVertCount == 4 ? 6 : 3);
    const bool colors    = md.flags == 5 && md.colors;
    return (((u32)vc * frames * 12 + 15) & ~15u) + ((idx * 2 + 15) & ~15u) + (colors ? (((u32)vc * 4 + 15) & ~15u) : 0);
}

// A model's VRAM contents, `n` bytes from `off` of block `which` (0 vertices, 1 triangles, 2 colours): what EnsureModel
// uploads and SITE MODELCHECK compares with what the VRAM holds.
void FillModel(const Model &md, const GModel &g, int which, u32 off, u32 n, u8 *buf)
{
    const unsigned frames = md.frameCount ? md.frameCount : 1, vc = md.vertCount;
    if (which == 0) {
        int16 *o = (int16 *)buf;
        for (u32 i = 0; i < n / 2; ++i) {
            const u32 e = off / 2 + i, v = e / 6, k = e % 6;
            if (v >= (u32)vc * frames) {
                o[i] = 0;
                continue;
            }
            const ModelVertex &mv = md.vertices[v];
            o[i] = (int16)(k == 0 ? mv.x : k == 1 ? mv.y : k == 2 ? mv.z : k == 3 ? mv.nx >> 2 : k == 4 ? mv.ny >> 2 : mv.nz >> 2);
        }
    }
    else if (which == 1) {
        u16 *o = (u16 *)buf;
        for (u32 i = 0; i < n / 2; ++i) {
            const u32 e = off / 2 + i;
            if (e >= g.idxCount) {
                o[i] = 0;
                continue;
            }
            if (md.faceVertCount == 3)
                o[i] = md.indices[e];
            else {
                static const u8 kQuad[6] = { 0, 1, 2, 0, 2, 3 };
                o[i] = md.indices[(e / 6) * 4 + kQuad[e % 6]];
            }
        }
    }
    else
        for (u32 i = 0; i < n; ++i) {
            const u32 e = off + i, v = e / 4, k = e % 4;
            const uint32 c = v < vc ? md.colors[v].color : 0;
            buf[i] = (u8)(k == 0 ? c >> 16 : k == 1 ? c >> 8 : k == 2 ? c : 0xFF);
        }
}

bool EnsureModel(unsigned m)
{
    if (sModelsVersion != contentVersion) {
        FreeModels();
        sModelsVersion = contentVersion;
    }
    GModel &g = sModels[m];
    if (g.tried)
        return g.ok;
    g.tried         = true;
    const Model &md = modelList[m];
    const unsigned frames = md.frameCount ? md.frameCount : 1, vc = md.vertCount;
    if (!md.vertices || !md.indices || !vc || !md.indexCount || (md.faceVertCount != 3 && md.faceVertCount != 4)) {
        DEV_LOG("gpu3d: model %u not drawable (vertices %u, indices %u, face size %u)", m, vc, (unsigned)md.indexCount, (unsigned)md.faceVertCount);
        return false;
    }

    // Vertices: 12 bytes each, every frame after the other.
    const u32 vbytes = ((u32)vc * frames * 12 + 15) & ~15u;
    g.vtx            = (u16 *)vramAlloc(vbytes);
    // Triangles: a quad face becomes two.
    const unsigned faces = md.indexCount / md.faceVertCount;
    g.idxCount           = faces * (md.faceVertCount == 4 ? 6 : 3);
    const u32 ibytes     = (g.idxCount * 2 + 15) & ~15u;
    g.idx                = (u16 *)vramAlloc(ibytes);
    const bool colors    = md.flags == 5 && md.colors; // MODEL_USENORMALS | MODEL_USECOLOURS: the software uses them
    const u32 cbytes     = ((u32)vc * 4 + 15) & ~15u;
    g.col                = colors ? (u8 *)vramAlloc(cbytes) : NULL;
    if (!g.vtx || !g.idx || (colors && !g.col)) {
        DEV_LOG("gpu3d: no VRAM for model %u (%u KB)", m, (unsigned)((vbytes + ibytes + cbytes) >> 10));
        return false;
    }
    sModelVram += vbytes + ibytes + (colors ? cbytes : 0);
    {
        float r2 = 0;
        unsigned wide = 0, firstWide = 0;
        for (u32 v = 0; v < (u32)vc * frames; ++v) {
            const ModelVertex &mv = md.vertices[v];
            const float d = (float)mv.x * mv.x + (float)mv.y * mv.y + (float)mv.z * mv.z;
            r2            = d > r2 ? d : r2;
            if (mv.x != (int16)mv.x || mv.y != (int16)mv.y || mv.z != (int16)mv.z) // the VRAM copy keeps 16 bits
                firstWide = wide++ ? firstWide : v;
        }
        g.radius = sqrtf(r2);
        if (wide)
            DEV_LOG("gpu3d: model %u has %u vertices beyond 16 bits (first: frame %u vertex %u)", m, wide, firstWide / vc, firstWide % vc);
    }
    g.vbytes = vbytes, g.ibytes = ibytes, g.cbytes = colors ? cbytes : 0;
    bool ok = UploadVram(g.vtx, vbytes, [&](u32 off, u32 n, u8 *buf) { FillModel(md, g, 0, off, n, buf); });
    ok      = ok && UploadVram(g.idx, ibytes, [&](u32 off, u32 n, u8 *buf) { FillModel(md, g, 1, off, n, buf); });
    if (colors)
        ok = ok && UploadVram(g.col, cbytes, [&](u32 off, u32 n, u8 *buf) { FillModel(md, g, 2, off, n, buf); });
    g.vc = (u16)vc;
    g.ok = ok;
    if (ok)
        DEV_LOG("gpu3d: model %u in VRAM (%u x %u vertices, %u triangles, %u KB)", m, vc, frames, (unsigned)(g.idxCount / 3),
                (unsigned)((vbytes + ibytes + cbytes) >> 10));
    return ok;
}

bool EmitScene3D(const Cmd &c, const uint8 *arena)
{
    const GpuScene3D::Head *h = (const GpuScene3D::Head *)arena;
    if (!s3Ready || sDraw3DCount >= MAX_DRAW3D)
        return Refuse(GpuRender::REASON_UNSUPPORTED, CMD_SCENE3D);
    // Under a clip smaller than the screen: scissored
    if (!ScissorFor(c.clip))
        return Refuse(GpuRender::REASON_UNSUPPORTED, CMD_SCENE3D);
    if (h->ink != INK_NONE && h->ink != INK_ALPHA && h->ink != INK_BLEND)
        return Refuse(GpuRender::REASON_INK, (unsigned)h->ink);
    const GpuScene3D::Inst *in = (const GpuScene3D::Inst *)(h + 1);
    for (int32 i = 0; i < h->count; ++i)
        if (in[i].model < 0 || in[i].model >= MODEL_COUNT || !EnsureModel((unsigned)in[i].model))
            return Refuse(GpuRender::REASON_UNSUPPORTED, CMD_SCENE3D);
    if (sBatchCount >= MAX_BATCHES)
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    sDraw3D[sDraw3DCount] = { h, in };
    sBatches[sBatchCount++] = { TEX_3D, BLEND_NORMAL, sDraw3DCount++, 0, 0, 0, sScissor };
    sScissor                = 0;
    sStats.faces += 0;
    return true;
}

// Depth: each scene gets its own 1/64 of the depth range, later scenes nearer (the draw order between scenes and the
// 2D around them is kept; inside a scene the depth test replaces the software's face sort).
// `follows`: the batch before was a 3D scene too (the program, the projection, the eye and the fixed state are set).
void Draw3DSceneGpu(unsigned k, bool follows)
{
    const Draw3D &d           = sDraw3D[k];
    const GpuScene3D::Head *h = d.h;
    const bool ortho          = h->drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED;
    const bool shaded         = h->drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN || ortho;
    if (!follows) {
        C3D_BindProgram(&s3Prog);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s3Loc.proj, (C3D_Mtx *)C3DPresent::Projection());
    }
    if (ortho) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.scr, 1.0f / 256, 1.0f / 256, -(float)h->posX, -(float)h->posY);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.ortho, 1, 0, 0, 0);
    }
    else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.scr, (float)(1 << h->projX), -(float)(1 << h->projY), (float)h->centerX, (float)h->centerY);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.ortho, 0, 1, 0, 0);
    }
    // z / w linear in Z inside the scene's band (1/Z left the near and far sides of a model a few 16-bit steps apart
    // and the last triangle drawn won); later scenes in nearer bands, as the software draws them over the earlier ones.
    // The band spans the scene's own depth, its instances' Z +- their models' reach: a fixed 0..65536 clamped every
    // farther face to one depth (the player is at ~41000) and columns, plants and ramps showed their back faces.
    const unsigned mode = GpuRender::depthMode;
    float zmin = 1e30f, zmax = -1e30f;
    for (int32 i = 0; i < h->count; ++i) {
        const GpuScene3D::Inst &in = d.in[i];
        const float sx = in.world[8] / 256.0f, sy = in.world[9] / 256.0f, sz = in.world[10] / 256.0f;
        const float reach = sModels[in.model].radius * sqrtf(sx * sx + sy * sy + sz * sz);
        const float z     = (float)in.world[11];
        zmin              = z - reach < zmin ? z - reach : zmin;
        zmax              = z + reach > zmax ? z + reach : zmax;
    }
    zmin = zmin < 1.0f && !ortho ? 1.0f : zmin; // (an orthographic model may reach behind Z = 0)
    zmax = zmax < zmin + 1.0f ? zmin + 1.0f : zmax;
    // Bands of the frame's scene count, not MAX_DRAW3D: the shader's 24-bit floats leave only a few hundred steps in a
    // band of 1/256 of the range.
    const float width = 1.0f / (float)(sDraw3DCount ? sDraw3DCount : 1), span = zmax - zmin;
    if (mode & 1) // nearer = lower z / w
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.band, -(float)(k + 1) * width - width * zmin / span, width / span, zmax, 0);
    else // nearer = higher z / w
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.band, -1.0f + (float)(k + 1) * width + width * zmin / span, -width / span, zmax, 0);
    const float alpha = h->ink == INK_NONE ? 255.0f : h->ink == INK_BLEND ? 128.0f : (float)(h->alpha > 0xFF ? 0xFF : h->alpha < 0 ? 0 : h->alpha);
    if (shaded) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.lit, 1.0f / 1024, 0, 0, alpha);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.dif, (float)h->diffuse[0], (float)h->diffuse[1], (float)h->diffuse[2], 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.difs, 1.0f / (1 << h->diffuseIntensity[0]), 1.0f / (1 << h->diffuseIntensity[1]),
                      1.0f / (1 << h->diffuseIntensity[2]), 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.spcs, 1.0f / (1024.0f * (1 << h->specularIntensity[0])), 1.0f / (1024.0f * (1 << h->specularIntensity[1])),
                      1.0f / (1024.0f * (1 << h->specularIntensity[2])), 0);
    }
    else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.lit, 0, 0, 0, alpha);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.dif, 1, 1, 1, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.difs, 1, 1, 1, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.spcs, 0, 0, 0, 0);
    }
    if (!follows) {
        // Stereo: things far behind the player shift up to the full disparity, nearer ones come out (at most half of it)
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.eye, sEyeShift * STEREO_FAR, STEREO_ZC, -0.5f, 1.0f);
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_DepthTest(true, (mode & 2) ? GPU_GREATER : GPU_LESS, GPU_WRITE_ALL);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF); // a mask frame's stencil does not apply to the 3D scenes
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    }

    for (int32 i = 0; i < h->count; ++i) {
        const GpuScene3D::Inst &in = d.in[i];
        const GModel &g            = sModels[in.model];
        for (int r = 0; r < 3; ++r)
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.wrow + r, in.world[r * 4 + 0] / 256.0f, in.world[r * 4 + 1] / 256.0f, in.world[r * 4 + 2] / 256.0f,
                          (float)in.world[r * 4 + 3]);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.nrow, in.normal[3] * 4 / 256.0f, in.normal[4] * 4 / 256.0f, in.normal[5] * 4 / 256.0f, 0);
        const float t = in.animated ? in.timer / 256.0f : 0.0f;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s3Loc.lerp, t, 1.0f - t, 0, 0);
        const unsigned fa = in.animated ? (unsigned)in.frameA : 0, fb = in.animated ? (unsigned)in.frameB : 0;
        C3D_BufInfo bi;
        BufInfo_Init(&bi);
        BufInfo_Add(&bi, (u8 *)g.vtx + fa * g.vc * 12, 12, 2, 0x10);
        BufInfo_Add(&bi, (u8 *)g.vtx + fb * g.vc * 12, 12, 2, 0x32);
        if (g.col) {
            BufInfo_Add(&bi, g.col, 4, 1, 0x4);
            C3D_SetAttrInfo(&s3AttrCol);
        }
        else {
            C3D_SetAttrInfo(&s3AttrFixed);
            C3D_FVec *fc = C3D_FixedAttribGetWritePtr(4);
            fc->x = (float)((in.color >> 16) & 0xFF), fc->y = (float)((in.color >> 8) & 0xFF), fc->z = (float)(in.color & 0xFF), fc->w = 255.0f;
        }
        C3D_SetBufInfo(&bi);
        C3D_DrawElements(GPU_TRIANGLES, (int)g.idxCount, C3D_UNSIGNED_SHORT, g.idx);
    }
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
        case INK_MASKED: blend = BLEND_MASKED, alpha = 0xFF; break;
        case INK_UNMASKED: blend = BLEND_UNMASKED, alpha = 0xFF; break;
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
        case INK_TINT:
            if (!(const uint16 *)tintLookupTable)
                return true;
            blend = BLEND_TINT;
            break;
        default: return Refuse(GpuRender::REASON_INK, (unsigned)ink);
    }
    uint32 tint = 0;
    if (blend == BLEND_TINT && (!TintModel((const uint16 *)tintLookupTable, &tint) || !EnsureGrab()))
        return Refuse(GpuRender::REASON_INK, (unsigned)ink);
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
    if (blend == BLEND_TINT) {
        const float px[4] = { (float)x, (float)(x + width), (float)(x + width), (float)x };
        const float py[4] = { (float)y, (float)y, (float)(y + height), (float)(y + height) };
        return AddTintQuad(px, py, tint) || Refuse(GpuRender::REASON_QUAD_LIMIT);
    }

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

// DrawLine: the software's clipping, then one thin quad (the Special Stage's wireframe item boxes, the giant ring's
// portal).
bool EmitLine(const Cmd &c)
{
    const int32 *p = c.p;
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
        default: return Refuse(GpuRender::REASON_INK, (unsigned)ink); // masked, tint: not for lines yet
    }
    int32 drawX1 = p[0], drawY1 = p[1], drawX2 = p[2], drawY2 = p[3];
    if (!screenRelative) {
        drawX1 = FROM_FIXED(p[0]) - c.pos[0];
        drawY1 = FROM_FIXED(p[1]) - c.pos[1];
        drawX2 = FROM_FIXED(p[2]) - c.pos[0];
        drawY2 = FROM_FIXED(p[3]) - c.pos[1];
    }
    const int32 cx1 = c.clip[0], cy1 = c.clip[1], cx2 = c.clip[2], cy2 = c.clip[3];
    auto outcode = [&](int32 x, int32 y, bool strict) {
        int32 f = 0;
        if (strict ? x >= cx2 : x > cx2)
            f = 2;
        else if (x < cx1)
            f = 1;
        if (strict ? y >= cy2 : y > cy2)
            f |= 8;
        else if (y < cy1)
            f |= 4;
        return f;
    };
    int32 flags1 = outcode(drawX1, drawY1, true), flags2 = outcode(drawX2, drawY2, true);
    while (flags1 || flags2) {
        if (flags1 & flags2)
            return true;
        const int32 curFlags = flags1 ? flags1 : flags2;
        int32 x = 0, y = 0;
        if (curFlags & 8) {
            int32 div = drawY2 - drawY1;
            if (!div)
                div = 1;
            x = drawX1 + ((drawX2 - drawX1) * (((cy2 - drawY1) << 8) / div) >> 8);
            y = cy2;
        }
        else if (curFlags & 4) {
            int32 div = drawY2 - drawY1;
            if (!div)
                div = 1;
            x = drawX1 + ((drawX2 - drawX1) * (((cy1 - drawY1) << 8) / div) >> 8);
            y = cy1;
        }
        else if (curFlags & 2) {
            int32 div = drawX2 - drawX1;
            if (!div)
                div = 1;
            x = cx2;
            y = drawY1 + ((drawY2 - drawY1) * (((cx2 - drawX1) << 8) / div) >> 8);
        }
        else if (curFlags & 1) {
            int32 div = drawX2 - drawX1;
            if (!div)
                div = 1;
            x = cx1;
            y = drawY1 + ((drawY2 - drawY1) * (((cx1 - drawX1) << 8) / div) >> 8);
        }
        if (curFlags == flags1)
            drawX1 = x, drawY1 = y, flags1 = outcode(x, y, false);
        else
            drawX2 = x, drawY2 = y, flags2 = outcode(x, y, false);
    }
    auto clampTo = [](int32 v, int32 lo, int32 hi) { return v > hi ? hi : v < lo ? lo : v; };
    drawX1 = clampTo(drawX1, cx1, cx2), drawY1 = clampTo(drawY1, cy1, cy2);
    drawX2 = clampTo(drawX2, cx1, cx2), drawY2 = clampTo(drawY2, cy1, cy2);

    // One quad: a parallelogram a pixel thick across the line's major axis, which covers a pixel of each column (or
    // row) as the software's steps do, give or take one at the half-way rounding. Walking the steps and gathering runs
    // made a quad a pixel on diagonals (the Special Stage's wireframe item boxes: ~3 ms of the 2DS's frame).
    const uint16 c16 = rgb32To16_B[(color >> 0) & 0xFF] | rgb32To16_G[(color >> 8) & 0xFF] | rgb32To16_R[(color >> 16) & 0xFF];
    const unsigned r5 = c16 >> 11, g6 = (c16 >> 5) & 0x3F, b5 = c16 & 0x1F;
    const uint8 r = (uint8)((r5 << 3) | (r5 >> 2)), g = (uint8)((g6 << 2) | (g6 >> 4)), b = (uint8)((b5 << 3) | (b5 >> 2));
    float px[4], py[4];
    if (abs(drawX2 - drawX1) >= abs(drawY2 - drawY1)) { // x-major: a pixel tall
        if (drawX2 < drawX1) {
            int32 t = drawX1;
            drawX1 = drawX2, drawX2 = t;
            t = drawY1, drawY1 = drawY2, drawY2 = t;
        }
        px[0] = (float)drawX1, py[0] = (float)drawY1;
        px[1] = drawX2 + 1.0f, py[1] = (float)drawY2;
        px[2] = drawX2 + 1.0f, py[2] = drawY2 + 1.0f;
        px[3] = (float)drawX1, py[3] = drawY1 + 1.0f;
    }
    else { // y-major: a pixel wide
        if (drawY2 < drawY1) {
            int32 t = drawX1;
            drawX1 = drawX2, drawX2 = t;
            t = drawY1, drawY1 = drawY2, drawY2 = t;
        }
        px[0] = (float)drawX1, py[0] = (float)drawY1;
        px[1] = drawX1 + 1.0f, py[1] = (float)drawY1;
        px[2] = drawX2 + 1.0f, py[2] = drawY2 + 1.0f;
        px[3] = (float)drawX2, py[3] = drawY2 + 1.0f;
    }
    static const float kWhite[4] = { 4, 4, 4, 4 }; // the solid block at the sprite atlas's origin
    if (!AddQuadFree(TEX_SPRITE, blend, px, py, kWhite, kWhite, SPR_ATLAS_W, SPR_ATLAS_H, r, g, b, (uint8)alpha))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

void ConvertSoft(const SoftJob &j);
void UploadSoftRows(unsigned texY, unsigned rows);

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
    // The recorded entries back where they were taken from: lines, or columns for a vertical-scroll layer (RecLayer).
    if (kind == LAYER_VSCROLL) {
        if (c.clip[2] > c.clip[0])
            memcpy(&scanlines[c.clip[0]], sl, (size_t)(c.clip[2] - c.clip[0]) * sizeof(ScanlineInfo));
    }
    else if (c.clip[3] > c.clip[1])
        memcpy(&scanlines[c.clip[1]], sl, (size_t)(c.clip[3] - c.clip[1]) * sizeof(ScanlineInfo));
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

    // Into its rows of the texture now: the next software layer clears the same rows of screen 1 (two in one frame
    // over the same lines: the Special Stage 2's small floor vanished under the big one).
    const unsigned texY        = sSoftRows;
    sSoftJobs[sSoftJobCount++] = { (int16)x1, (int16)y1, (int16)x2, (int16)y2, (int16)texY };
    sSoftRows                  = (texY + (unsigned)h + 7) & ~7u; // jobs start on a block row
    ConvertSoft(sSoftJobs[sSoftJobCount - 1]);
    UploadSoftRows(texY, (unsigned)h);
    ++sStats.softLayers;
    if (!AddQuad(TEX_SOFT, BLEND_NORMAL, (float)x1, (float)y1, (float)x2, (float)y2, (float)x1, (float)texY, (float)x2, (float)(texY + h), SOFT_W, SOFT_H, 255, 255,
                 255, 255))
        return Refuse(GpuRender::REASON_QUAD_LIMIT);
    return true;
}

// Copies a software layer's rows from screen 1 into the staging buffer as RGBA5551 (the key colour transparent), in
// plain rows: the display-transfer engine tiles them into the texture right after (UploadSoftRows).
void ConvertSoft(const SoftJob &j)
{
    const ScreenInfo &scr = screens[1];
    C3DPresent::StagingWait();
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

// Tiles rows [texY, texY + rows) of the staging buffer into the texture (texY on a block row; rows rounded up to one).
void UploadSoftRows(unsigned texY, unsigned rows)
{
    rows = (rows + 7) & ~7u;
    if (texY + rows > SOFT_H)
        rows = SOFT_H - texY;
    u16 *staging = (u16 *)C3DPresent::Staging() + texY * SOFT_W;
    GSPGPU_FlushDataCache(staging, rows * SOFT_W * 2);
    C3D_SyncDisplayTransfer((u32 *)staging, GX_BUFFER_DIM(SOFT_W, rows), (u32 *)((u16 *)sSoftTex.data + texY * SOFT_W), GX_BUFFER_DIM(SOFT_W, rows),
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
            const int cx = (cell % (TILE_ATLAS_W / 16)) * 16 + inX, cy = (cell / (TILE_ATLAS_W / 16)) * 16 + srcY;
            if (!AddTileQuad(sx, dy, sx + w, dy + n, cx, cy, cx + w, cy + n))
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
        const int margin = EdgeMargin();
        if (!EmitTileRun(layer, 0, layerY >> 4, layerY & 15, line, n, x1 - margin, x2 + margin, xOff))
            return false;
        line += n;
        layerY += n;
        // The software wraps at the layer's last row: a screen that crosses the layer's bottom continues at its top (Press
        // Garden's far background when the camera is high; the rows past the end drew nothing, the sky showed through).
        if (layerY >= layer->ysize << 4)
            layerY -= layer->ysize << 4;
    }
    return true;
}

// DrawLayerVScroll: the recorded entries are per screen column (position.x: the layer's pixel column, advancing a pixel a
// column; position.y: the band's vertical scroll). A run of columns inside one tile column with the same y is a strip of
// tile pieces, as many quads as the strip has tile rows. (The software drew every such layer: ~14 ms a frame on the 2DS in
// Metallic Madness 2.)
bool EmitLayerVScroll(const Cmd &c, TileLayer *layer, const ScanlineInfo *sl)
{
    const int x1 = c.clip[0], x2 = c.clip[2], y1 = c.clip[1] < 0 ? 0 : c.clip[1], y2 = c.clip[3] > SCREEN_YSIZE ? SCREEN_YSIZE : c.clip[3];
    if (x1 >= x2 || y1 >= y2 || !layer->xsize || !layer->ysize)
        return true;
    const int height   = (int)layer->ysize << 4;
    const unsigned bank = sLineBuf[0] & 7; // the software draws the whole layer with the palette of line 0's bank
    int cx             = x1;
    while (cx < x2) {
        const ScanlineInfo &s0 = sl[cx - x1];
        const int xp           = FROM_FIXED(s0.position.x);
        int w                  = 1;
        while (cx + w < x2 && ((xp + w) & 15) && sl[cx + w - x1].position.y == s0.position.y && FROM_FIXED(sl[cx + w - x1].position.x) == xp + w)
            ++w;
        int yp = FROM_FIXED(s0.position.y);
        if (yp >= height)
            yp -= height;
        else if (yp < 0)
            yp += height;
        const int col = xp >> 4, inX = xp & 15;
        if (col < 0 || col >= layer->xsize || yp < 0 || yp >= height)
            return Refuse(GpuRender::REASON_TILE_RANGE);
        int row  = yp >> 4;
        int srcY = yp & 15;
        for (int dy = 0; dy < y2;) {
            const int n = 16 - srcY < y2 - dy ? 16 - srcY : y2 - dy;
            const int a = dy > y1 ? dy : y1, b = dy + n;
            const uint16 t = layer->layout[col + (row << layer->widthShift)];
            if (a < b && t < 0xFFFF) {
                ++sStats.tileQuads;
                const int cell = EnsureTile(t & 0xFFF, bank);
                if (cell == -2)
                    return Refuse(GpuRender::REASON_ATLAS_FULL);
                if (cell < 0)
                    return Refuse(GpuRender::REASON_TILE_RANGE);
                const int tx = (cell % (TILE_ATLAS_W / 16)) * 16 + inX, ty = (cell / (TILE_ATLAS_W / 16)) * 16 + srcY + (a - dy);
                if (!AddTileQuad(cx, a, cx + w, b, tx, ty, tx + w, ty + (b - a)))
                    return Refuse(GpuRender::REASON_QUAD_LIMIT);
            }
            else if (t >= 0xFFFF)
                ++sStats.emptyTiles;
            dy += n;
            srcY = 0;
            if (++row == (int)layer->ysize)
                row = 0;
        }
        cx += w;
    }
    return true;
}

// A rotozoom layer (the Special Stage floor, the title's island): per line the software walks the layer from
// scanline.position by scanline.deform a pixel, so each line is cut into the runs of pixels that stay in one tile, and
// each run is a 1-pixel-high quad of that tile with affine texel coordinates (1/32 texel, TEX_TILE_FINE). The ends of
// a run are its first and last samples kept 2/32 inside the tile, so the interpolation never reads the next atlas
// cell. Empty tiles (0xFFFF) are skipped, which is most of the Special Stage map (its far lines cross hundreds of
// empty tiles: walking them is cheap, drawing them is not needed). A layer that would take too many runs or quads (a
// horizon full of tiles) goes back to the software.
enum { ROTO_MAX_RUNS = 65536, ROTO_MAX_QUADS = 6000 }; // the walk is cheap (runs over empty tiles cost little): quads are the limit

// Where a line's walk along one axis enters the next tile, as exact integer steps (no division per run): with N the
// distance in 16.16 to the next tile edge counted from the line's start, the crossing is pixel ceil(N / |d|); each tile
// adds 1 << 20 to N, i.e. q = (1 << 20) / |d| pixels and r = (1 << 20) % |d| of remainder.
struct RotoAxis {
    int next;          // first pixel of the line in the next tile (INT_MAX: never)
    uint32 d, q, r, e; // |deform|; whole pixels and remainder a tile; e = next * d - N, 0 <= e < d
    void Init(uint32 pos, int32 deform)
    {
        if (!deform) {
            next = 0x7FFFFFFF;
            return;
        }
        d = (uint32)(deform < 0 ? -deform : deform);
        q = (1u << 20) / d, r = (1u << 20) % d;
        // Going up the edge is the tile's end (N = 2^20 - f); going down a pixel is still in the tile while its
        // position is >= the tile's start, so the crossing is floor(f / d) + 1 = ceil((f + 1) / d).
        const uint32 f = pos & 0xFFFFF, N = deform > 0 ? (1u << 20) - f : f + 1;
        const uint32 k = (N + d - 1) / d;
        next = (int)k;
        e    = k * d - N;
    }
    void Step()
    {
        if (r > e)
            next += (int)q + 1, e = d - (r - e);
        else
            next += (int)q, e -= r;
    }
};

// Stereo depth of a rotozoom layer's lines: each line's depth from its distance (the walk's step grows with it), the
// lines at the player's feet (line 200, the Special Stage camera's) on the screen plane, the horizon the farthest. The
// title screen's island and clouds: the island's nearest line (screen line 239, TitleBG_Scanline_Island's i = 87, a
// step of 2 * 0xA00000 / (8 * 87)) on the screen plane, both layers measured against it. 0: no depths.
float RotoFeet(const ScanlineInfo *first, int y1, int y2)
{
    if (sTitleScene)
        return 2.0f * (float)(0xA00000 / (8 * 87));
    if (!sDepths)
        return 0;
    const int fy = 200 < y1 ? y1 : 200 >= y2 ? y2 - 1 : 200;
    return sqrtf((float)first[fy - y1].deform.x * first[fy - y1].deform.x + (float)first[fy - y1].deform.y * first[fy - y1].deform.y);
}

int8 RotoLineDepth(float feet, int32 dx, int32 dy)
{
    const float d = sqrtf((float)dx * dx + (float)dy * dy);
    float f       = d > 0 ? 1.0f - feet / d : 1.0f;
    f             = f < -0.5f ? -0.5f : f > 1.0f ? 1.0f : f;
    return (int8)(f * STEREO_FAR + (f < 0 ? -0.5f : 0.5f));
}

// Draws the whole layer (or sheet) into its texture: per 16x16 tile, four 8x8 Morton blocks, through the staging buffer.
bool BuildRotoTex(C3D_Tex &t, unsigned w, unsigned h, const RotoSrc &src, const u16 *tex)
{
    const unsigned rowBytes = w * 8 * 2; // one row of 8x8 blocks
    static const uint8 my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    if (src.sheet)
        return UploadVram(t.data, w * h * 2, [&](u32 off, u32 n, u8 *staging) {
            u16 *dst           = (u16 *)staging;
            const unsigned br0 = off / rowBytes, rows = n / rowBytes;
            for (unsigned br = br0; br < br0 + rows; ++br)
                for (unsigned bx = 0; bx < (w >> 3); ++bx) {
                    u16 *blk = &dst[((br - br0) * (w >> 3) + bx) << 6];
                    for (unsigned y = 0; y < 8; ++y) {
                        const uint8 *sp = &src.sheet->pixels[((br * 8 + y) << src.sheet->lineSize) + bx * 8];
                        u16 *b          = blk + my[y];
                        b[0] = tex[sp[0]], b[1] = tex[sp[1]], b[4] = tex[sp[2]], b[5] = tex[sp[3]];
                        b[16] = tex[sp[4]], b[17] = tex[sp[5]], b[20] = tex[sp[6]], b[21] = tex[sp[7]];
                    }
                }
        });
    const TileLayer *layer  = src.layer;
    const unsigned wShift   = layer->widthShift;
    return UploadVram(t.data, w * h * 2, [&](u32 off, u32 n, u8 *staging) {
        u16 *dst            = (u16 *)staging;
        const unsigned br0  = off / rowBytes, rows = n / rowBytes;
        for (unsigned br = br0; br < br0 + rows; ++br) {
            const unsigned ty = br >> 1, by = br & 1;
            for (unsigned tx = 0; tx < (w >> 4); ++tx) {
                const unsigned t = layer->layout[tx + (ty << wShift)] & 0xFFF;
                const uint8 *src = &tilesetPixels[TILE_DATASIZE * t + by * 8 * 16];
                for (unsigned bx = 0; bx < 2; ++bx) {
                    u16 *blk        = &dst[((br - br0) * (w >> 3) + tx * 2 + bx) << 6];
                    const uint8 *sp = src + bx * 8;
                    for (unsigned y = 0; y < 8; ++y, sp += 16) {
                        u16 *b = blk + my[y];
                        b[0] = tex[sp[0]], b[1] = tex[sp[1]], b[4] = tex[sp[2]], b[5] = tex[sp[3]];
                        b[16] = tex[sp[4]], b[17] = tex[sp[5]], b[20] = tex[sp[6]], b[21] = tex[sp[7]];
                    }
                }
            }
        }
    });
}

// What the texture depends on: the colours its pixels use (hot ones aside), the layout, the tileset's epoch (a sheet:
// its pixels' address).
uint32 RotoContentHash(const RotoTex &r, const RotoSrc &src, const uint16 *pal)
{
    const TileLayer *layer = src.layer;
    uint32 used[8];
    memcpy(used, r.used, sizeof(used));
    for (unsigned k = 0; k < r.hotCount; ++k)
        used[r.hot[k] >> 5] &= ~(1u << (r.hot[k] & 31));
    uint32 hash = 2166136261u ^ (src.sheet ? sTileEpoch : r.tileGen) * 16777619u;
    for (unsigned i = 0; i < PALETTE_BANK_SIZE; ++i)
        if (used[i >> 5] & (1u << (i & 31)))
            hash = (hash ^ pal[i]) * 16777619u;
    if (src.sheet)
        return (hash ^ (uint32)(uintptr_t)src.sheet->pixels) * 16777619u;
    const uint32 *lw     = (const uint32 *)layer->layout;
    const unsigned words = ((1u << layer->widthShift) << layer->heightShift) / 2;
    for (unsigned i = 0; i < words; ++i)
        hash = (hash ^ lw[i]) * 16777619u;
    return hash;
}

// The hot-colour mask: per texel (k + 1) * 32 where the pixel uses hot colour k, else 0; Morton blocks of bytes.
bool BuildRotoMask(RotoTex &r, const RotoSrc &src)
{
    if (src.sheet)
        return false; // no hot colours for sheets (EmitRotoTex)
    const TileLayer *layer = src.layer;
    uint8 slot[PALETTE_BANK_SIZE];
    memset(slot, 0, sizeof(slot));
    for (unsigned k = 0; k < r.hotCount; ++k)
        slot[r.hot[k]] = (uint8)((k + 1) * 32);
    const unsigned w = r.w, rowBytes = w * 8; // one row of 8x8 blocks
    const unsigned wShift = layer->widthShift;
    static const uint8 my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    return UploadVram(r.mask.data, w * r.h, [&](u32 off, u32 n, u8 *dst) {
        const unsigned br0 = off / rowBytes, rows = n / rowBytes;
        for (unsigned br = br0; br < br0 + rows; ++br) {
            const unsigned ty = br >> 1, by = br & 1;
            for (unsigned tx = 0; tx < (w >> 4); ++tx) {
                const unsigned t = layer->layout[tx + (ty << wShift)] & 0xFFF;
                const uint8 *src = &tilesetPixels[TILE_DATASIZE * t + by * 8 * 16];
                for (unsigned bx = 0; bx < 2; ++bx) {
                    uint8 *blk      = &dst[((br - br0) * (w >> 3) + tx * 2 + bx) << 6];
                    const uint8 *sp = src + bx * 8;
                    for (unsigned y = 0; y < 8; ++y, sp += 16) {
                        uint8 *b = blk + my[y];
                        b[0] = slot[sp[0]], b[1] = slot[sp[1]], b[4] = slot[sp[2]], b[5] = slot[sp[3]];
                        b[16] = slot[sp[4]], b[17] = slot[sp[5]], b[20] = slot[sp[6]], b[21] = slot[sp[7]];
                    }
                }
            }
        }
    });
}

// The layer's lines [y1, y2) as one quad each from its texture. *handled false: not possible this frame (too big, no
// VRAM, lines in different banks, content still changing, coordinates out of range): the caller draws tile runs.
// The Special Stage floor's fog (FloorWin, below): whether its banks are fades of bank 0 (FloorFog held), and a
// line's fade in its bank (0..255, the owner's percentage applied).
bool FloorFadeReady();
unsigned FloorFade(unsigned bank);

// `src` a layer, or a sheet drawn with `blend0` and the vertex colour `rgba0` (a layer: BLEND_NORMAL, opaque white).
bool EmitRotoTex(const RotoSrc &src, unsigned w, unsigned h, const ScanlineInfo *first, int x1, int x2, int y1, int y2, float feet, bool *handled,
                 uint8 blend0 = BLEND_NORMAL, uint32 rgba0 = 0xFFFFFFFFu)
{
    *handled = false;
    if (GpuRender::depthMode & 512) // debug: the tile runs as before
        return true;
    const void *key = src.Key();
    if (w > 1024 || h > 1024) // the floor windows' business (sFloorWhy)
        return true;
    sRotoTexWhy = key == sRotoTexFailed ? "no vram" : "banks";
    if (key == sRotoTexFailed)
        return true;
    // The palette banks its lines use: a copy of the texture each (the Special Stage's fog banks per line). Within
    // ROTO_BANK_BUDGET of VRAM, the title's one-bank island aside.
    // In a Special Stage whose fog banks are fades (the floor's check), a layer keeps bank 0 only and each line fades by
    // the vertex alpha as the floor windows do: one copy to build instead of one a bank (Special Stage 4's water
    // animates every frame in six banks).
    const bool fadeMode = sUfoScene && src.layer && blend0 == BLEND_NORMAL && FloorFadeReady();
    unsigned banks = 0, bankCount = 0;
    for (int y = y1; y < y2; ++y)
        banks |= 1u << (fadeMode ? 0 : sLineBuf[y] & 7);
    for (unsigned b = 0; b < 8; ++b)
        bankCount += (banks >> b) & 1;
    if (bankCount > 1 && w * h * 2 * bankCount > ROTO_BANK_BUDGET)
        return true;
    sRotoTexWhy = "slots";

    // The layer's slot (a layer slot holds another scene's layer of another size after a scene change: anew), else a
    // free one, else one not drawn for a second (its VRAM goes back).
    RotoTex *r = NULL;
    for (unsigned i = 0; i < ROTO_TEX_SLOTS && !r; ++i)
        if (sRotoTex[i].key == key) {
            r = &sRotoTex[i];
            if (r->w != w || r->h != h) {
                FreeRotoTex(*r);
                r = NULL;
            }
        }
    if (!r) {
        for (unsigned i = 0; i < ROTO_TEX_SLOTS && !r; ++i)
            if (!sRotoTex[i].key)
                r = &sRotoTex[i];
        for (unsigned i = 0; i < ROTO_TEX_SLOTS && !r; ++i)
            if (sRotoFrame - sRotoTex[i].lastUsed > 60) {
                FreeRotoTex(sRotoTex[i]);
                r = &sRotoTex[i];
            }
        if (!r) {
            static char why[96];
            int n = snprintf(why, sizeof(why), "slots (wanted %ux%u):", w, h);
            for (unsigned i = 0; i < ROTO_TEX_SLOTS; ++i)
                n += snprintf(why + n, sizeof(why) - n, " %ux%u age %u", sRotoTex[i].w, sRotoTex[i].h, (unsigned)(sRotoFrame - sRotoTex[i].lastUsed));
            sRotoTexWhy = why;
            return true;
        }
        r->key = key, r->w = w, r->h = h;
        memset(r->used, 0xFF, sizeof(r->used));
    }

    // Only the tiles the map uses count: Flying Battery animates tiles of other layers every other frame, and its
    // 512x1024 layer was rebuilt (~26 ms) each time with the tileset's epoch.
    if (src.layer && sTileChangedAny) {
        memset(r->tileUsed, 0, sizeof(r->tileUsed));
        const unsigned n = (1u << src.layer->widthShift) << src.layer->heightShift;
        for (unsigned i = 0; i < n; ++i) {
            const unsigned t = src.layer->layout[i];
            if (t != 0xFFFF)
                r->tileUsed[(t & (TILE_COUNT - 1)) >> 5] |= 1u << (t & 31);
        }
        for (unsigned w = 0; w < TILE_COUNT / 32; ++w)
            if (r->tileUsed[w] & sTileChanged[w]) {
                ++r->tileGen;
                break;
            }
    }

    // Each bank's copy: made, current (its colours, the layout, the tileset), or rebuilt once the change has held.
    for (unsigned b = 0; b < 8; ++b) {
        if (!(banks & (1u << b)))
            continue;
        RotoBank &rb = r->bank[b];
        if (!rb.alloc) {
            // Leave room for the Special Stage's 3D models (VRAM_RESERVE).
            if ((sUfoScene && vramSpaceFree() < w * h * 2 + VramReserve()) || !C3D_TexInitVRAM(&rb.tex, (u16)w, (u16)h, GPU_RGBA5551)) {
                sRotoTexWhy = "no vram";
                if (!b || bankCount == 1)
                    sRotoTexFailed = key;
                return true;
            }
            C3D_TexSetFilter(&rb.tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&rb.tex, GPU_REPEAT, GPU_REPEAT);
            rb.alloc = true, rb.valid = false, rb.seenFrames = 0;
        }
        const uint16 *pal = sPalShadow[sBankMap[b]];
        uint32 hash       = RotoContentHash(*r, src, pal);
        if (rb.valid && rb.built != hash) {
            // Only a few used colours changed, nothing else: they become hot (the copies stay, the mask paints them).
            uint8 changed[ROTO_HOT_MAX];
            unsigned n = 0;
            bool few   = true;
            for (unsigned i = 1; i < PALETTE_BANK_SIZE && few; ++i) {
                if (!(r->used[i >> 5] & (1u << (i & 31))) || pal[i] == rb.builtPal[i])
                    continue;
                bool isHot = false;
                for (unsigned k = 0; k < r->hotCount; ++k)
                    isHot |= r->hot[k] == i;
                if (isHot)
                    continue;
                if (r->hotCount + n >= ROTO_HOT_MAX)
                    few = false;
                else
                    changed[n++] = (uint8)i;
            }
            if (few && n && blend0 == BLEND_NORMAL && !src.sheet) { // (a sheet has no hot mask: BuildRotoMask refuses it; Oil Ocean's smog cycled
                // a few colours, went hot and was refused every frame, the whole frame to the CPU at ~20 fps)
                const unsigned keep = r->hotCount;
                memcpy(r->hot + r->hotCount, changed, n);
                r->hotCount += n;
                if (RotoContentHash(*r, src, rb.builtPal) == RotoContentHash(*r, src, pal)) {
                    // Every copy's hash leaves the new hot colours out from now on.
                    for (unsigned c = 0; c < 8; ++c)
                        if (r->bank[c].valid)
                            r->bank[c].built = RotoContentHash(*r, src, r->bank[c].builtPal);
                    r->maskValid = false;
                    hash         = rb.built;
                }
                else
                    r->hotCount = keep; // something else changed too: an ordinary rebuild
            }
        }
        if (!rb.valid || rb.built != hash) {
            rb.seenFrames = rb.seen == hash ? rb.seenFrames + 1 : 1;
            rb.seen       = hash;
            sRotoTexWhy   = "changing";
            // A small texture is rebuilt at once, every frame if need be (Special Stage 4's water, 256x256, animates its
            // tiles every frame: as tile runs it took ~10 ms and 4500 quads); a big one waits for the change to hold.
            if (rb.seenFrames < 2 && w * h > 256 * 256)
                return true;
            const u64 tBuild = svcGetSystemTick();
            const bool built = BuildRotoTex(rb.tex, w, h, src, sTexPal[sBankMap[b]]);
            sStats.rotoBuildUs += (unsigned)((svcGetSystemTick() - tBuild) / (SYSCLOCK_ARM11 / 1000000));
            if (!built)
                return true;
            memcpy(rb.builtPal, pal, sizeof(rb.builtPal));
            if (!r->usedOk && src.sheet) {
                memset(r->used, 0, sizeof(r->used));
                for (unsigned y = 0; y < h; ++y) {
                    const uint8 *sp = &src.sheet->pixels[y << src.sheet->lineSize];
                    for (unsigned x = 0; x < w; ++x)
                        r->used[sp[x] >> 5] |= 1u << (sp[x] & 31);
                }
                r->used[0] &= ~1u;
                r->usedOk = true;
                for (unsigned c = 0; c < 8; ++c)
                    if (r->bank[c].valid)
                        r->bank[c].built = RotoContentHash(*r, src, r->bank[c].builtPal);
            }
            if (!r->usedOk) {
                // The colours it uses, from its distinct tiles (until now every colour counted).
                const TileLayer *layer = src.layer;
                static uint8 seenTile[TILE_COUNT * 4];
                memset(seenTile, 0, sizeof(seenTile));
                memset(r->used, 0, sizeof(r->used));
                const unsigned texels = (1u << layer->widthShift) << layer->heightShift;
                for (unsigned i = 0; i < texels; ++i) {
                    const unsigned t = layer->layout[i] & 0xFFF;
                    if (seenTile[t])
                        continue;
                    seenTile[t]      = 1;
                    const uint8 *src = &tilesetPixels[TILE_DATASIZE * t];
                    for (unsigned k = 0; k < TILE_DATASIZE; ++k)
                        r->used[src[k] >> 5] |= 1u << (src[k] & 31);
                }
                r->used[0] &= ~1u; // index 0 is transparent whatever its colour
                r->usedOk = true;
                for (unsigned c = 0; c < 8; ++c) // their hashes counted every colour
                    if (r->bank[c].valid)
                        r->bank[c].built = RotoContentHash(*r, src, r->bank[c].builtPal);
            }
            rb.built = rb.seen = RotoContentHash(*r, src, pal);
            rb.valid           = true;
            r->maskValid       = false;
            ++sStats.rotoTexBuilds;
        }
    }
    if (r->hotCount && !r->maskValid) {
        if (!r->maskAlloc) {
            if (!C3D_TexInitVRAM(&r->mask, (u16)w, (u16)h, GPU_A8)) {
                r->hotCount = 0; // no room: the hot colours stay as built (they stop changing on screen)
                for (unsigned c = 0; c < 8; ++c)
                    if (r->bank[c].valid)
                        r->bank[c].built = RotoContentHash(*r, src, r->bank[c].builtPal);
                return true;
            }
            C3D_TexSetFilter(&r->mask, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&r->mask, GPU_REPEAT, GPU_REPEAT);
            r->maskAlloc = true;
        }
        if (!BuildRotoMask(*r, src))
            return true;
        r->maskValid = true;
    }

    // Each line: the software samples pixel cx at pos + cx * deform (16.16 texels, wrapping); the quad's ends are half
    // a step outside the first and last samples, so the GPU's samples at pixel centres land on the same texels. In
    // 1/16 texel from the start's position inside the texture; a line that would not fit an int16 refuses.
    const unsigned slot   = (unsigned)(r - sRotoTex);
    const int lineSize    = x2 - x1;
    const uint32 maskX    = (w << 16) - 1, maskY = (h << 16) - 1;
    const unsigned passes = 1 + (r->maskValid && !fadeMode && !(GpuRender::depthMode & 2048) ? r->hotCount : 0); // bit 2048 (debug): no hot-colour passes
    const unsigned need   = (unsigned)(y2 - y1) * passes;
    sRotoTexWhy = "quads";
    if (sQuadCount + need > MAX_QUADS)
        return true;
    // From here on the layer is drawn from the texture.
    *handled    = true;
    r->lastUsed = sRotoFrame;
    unsigned q = sQuadCount;
    uint32 *w32 = (uint32 *)&sVbo[sCur][q * 4];
    // The lines first: where the layer maps each one, how far it is, which bank. Then neighbours on one plane share a
    // quad: a quad one line high costs the GPU about as much as a taller one (the title's island, clouds and their hot
    // passes were ~950 quads of 400 x 1 a frame in 3D, ~20 us each on the 2DS). A plane seen at an angle maps screen
    // line y to texels (A + B * x) / q(y) with q linear in y (q: the inverse of the line's texel step), which the PICA
    // interpolates exactly given each vertex's w = 1 / q (sRotoProg; the stereo disparity, linear in q too, also per
    // vertex). So lines i..e share a quad while every line between stays within a quarter texel of that projective
    // interpolation of the first's and the last's ends (an affine merge of lines whose step changes drew the quad's
    // two triangles with different mappings: the island's near rows sheared along the diagonal).
    // SITE SET depthmode 32768: one quad a line, to compare.
    struct RLine {
        int64 u0, v0, u1, v1;
        double fu0, fv0, fu1, fv1; // the same as doubles (converting an int64 is a library call)
        double q; // 1 / the texel step (16.16) along the line; 0: cannot share a quad
        unsigned b, k;
        int8 depth;
    };
    static RLine ln[SCREEN_YSIZE];
    const int nLines = y2 - y1;
    for (int i = 0; i < nLines; ++i) {
        const int y           = y1 + i;
        const ScanlineInfo &s = first[i];
        const unsigned lb     = sLineBuf[y] & 7;
        RLine &L              = ln[i];
        L.b                   = fadeMode ? 0 : lb;
        L.depth               = feet > 0 ? RotoLineDepth(feet, s.deform.x, s.deform.y) : sDepth;
        const int64 px = (int64)((uint32)s.position.x & maskX), py = (int64)((uint32)s.position.y & maskY);
        L.u0 = px - s.deform.x / 2, L.v0 = py - s.deform.y / 2;
        L.u1 = L.u0 + (int64)s.deform.x * lineSize, L.v1 = L.v0 + (int64)s.deform.y * lineSize;
        L.fu0 = (double)L.u0, L.fv0 = (double)L.v0, L.fu1 = (double)L.u1, L.fv1 = (double)L.v1;
        const double step = sqrt((double)s.deform.x * s.deform.x + (double)s.deform.y * s.deform.y);
        L.q               = step > 0 ? 1.0 / step : 0;
        // 1/16 texel, or coarser where the line spans thousands of texels (far lines of a small repeating layer).
        int64 big = 0;
        for (int64 v : { L.u0, L.u1, L.v0, L.v1 })
            big = (v < 0 ? -v : v) > big ? (v < 0 ? -v : v) : big;
        unsigned k = 0;
        while ((big >> (12 + k)) >= 32000 && k < 12)
            ++k;
        L.k = k;
    }
    // A value of the plane through lines a and e at t (0 at a's centre, 1 at e's), and its q.
    auto planeQ = [](const RLine &A, const RLine &E, double t) { return A.q + (E.q - A.q) * t; };
    auto planeAt = [](const RLine &A, const RLine &E, double t, double qt, double va, double ve) {
        return ((1 - t) * A.q * va + t * E.q * ve) / qt;
    };
    static int gStart[SCREEN_YSIZE], gLen[SCREEN_YSIZE];
    int groups = 0;
    const bool merge = src.layer && !fadeMode && sRotoProgOk && !(GpuRender::depthMode & 32768); // not a sheet (the plasma): its lines are not a plane
    {
        const double tol  = 16384; // a quarter texel (16.16)
        const int64 limU = (int64)w << 14, limV = (int64)h << 14;
        auto absd        = [](int64 v) { return v < 0 ? -v : v; };
        // Whether every line between a and e is on their plane (and the quad's edges at q > 0). Compared times q (no
        // division a line): q * value is linear in the line.
        auto onPlane = [&](int a, int e) {
            const RLine &A = ln[a], &E = ln[e];
            const double h = 0.5 / (e - a);
            if (planeQ(A, E, -h) <= 0 || planeQ(A, E, 1 + h) <= 0)
                return false;
            const double dt = 1.0 / (e - a);
            const double au0 = A.q * A.fu0, av0 = A.q * A.fv0, au1 = A.q * A.fu1, av1 = A.q * A.fv1;
            const double du0 = (E.q * E.fu0 - au0) * dt, dv0 = (E.q * E.fv0 - av0) * dt, du1 = (E.q * E.fu1 - au1) * dt, dv1 = (E.q * E.fv1 - av1) * dt;
            const double dq = (E.q - A.q) * dt;
            for (int j = a + 1; j < e; ++j) {
                const RLine &M = ln[j];
                const double s = j - a, qt = A.q + dq * s, lim = tol * qt;
                if (fabs(au0 + du0 * s - qt * M.fu0) > lim || fabs(av0 + dv0 * s - qt * M.fv0) > lim || fabs(au1 + du1 * s - qt * M.fu1) > lim ||
                    fabs(av1 + dv1 * s - qt * M.fv1) > lim)
                    return false;
            }
            return true;
        };
        for (int i = 0; i < nLines;) {
            // The longest run that could be one plane: one bank, one texel unit, no wrap of the layer between neighbours.
            int e = i;
            while (merge && ln[i].q > 0 && e + 1 < nLines) {
                const RLine &a = ln[i], &n = ln[e + 1], &pr = ln[e];
                if (n.b != a.b || n.k != a.k || n.q <= 0)
                    break;
                if (absd(n.u0 - pr.u0) > limU || absd(n.u1 - pr.u1) > limU || absd(n.v0 - pr.v0) > limV || absd(n.v1 - pr.v1) > limV)
                    break; // the layer wrapped between them
                ++e;
            }
            // The longest run of those on one plane: the whole when it is (the title's), else found by doubling the
            // run and halving the step back (n log n checks, not n^2).
            if (e > i && !onPlane(i, e)) {
                const int top = e;
                int good = i, step = 1;
                while (good + step <= top && (good + step == i + 1 ? true : onPlane(i, good + step)))
                    good += step, step *= 2;
                for (step /= 2; step > 0; step /= 2)
                    if (good + step < top && onPlane(i, good + step))
                        good += step;
                e = good > i && onPlane(i, good) ? good : i;
            }
            gStart[groups] = i, gLen[groups] = e - i + 1, ++groups;
            i = e + 1;
        }
    }
    {
        static char why[48];
        snprintf(why, sizeof(why), "- %ux%u banks %02x hot %u", w, h, banks, r->maskValid ? r->hotCount : 0);
        sRotoTexWhy = why;
        snprintf(sRotoRuns[slot], sizeof(sRotoRuns[slot]), "%ux%u %d lines %d quads", w, h, nLines, groups);
    }
    for (unsigned pass = 0; pass < passes; ++pass)
    for (int g = 0; g < groups; ++g) {
        const int i = gStart[g], n = gLen[g], y = y1 + i;
        const RLine &A = ln[i], &E = ln[i + n - 1];
        const unsigned b = A.b;
        const int8 depth = merge ? 0 : A.depth; // merged: the disparity is in the vertices
        // Pass 0 the line's bank's copy; pass k the mask's hot colour k - 1 in that bank now (as the atlases quantise
        // it).
        uint8 ptex = (uint8)(TEX_ROTO0 + slot), pblend = blend0;
        uint32 constant = 0;
        if (pass) {
            const u16 t       = ToTexel(sPalShadow[sBankMap[b]][r->hot[pass - 1]]);
            const unsigned r5 = t >> 11, g5 = (t >> 6) & 31, b5 = (t >> 1) & 31;
            const unsigned cr = (r5 << 3) | (r5 >> 2), cg = (g5 << 3) | (g5 >> 2), cb = (b5 << 3) | (b5 >> 2);
            ptex     = (uint8)(TEX_ROTOMASK0 + slot);
            pblend   = BLEND_HOT;
            constant = ((uint32)(pass * 32) << 24) | (cb << 16) | (cg << 8) | cr;
        }
        const unsigned k = A.k;
        const unsigned sh = 12 + k;
        const int64 rnd   = (int64)1 << (sh - 1);
        const uint8 sub = pass ? 0 : (uint8)b; // the bank copy (Batch::fog holds it for TEX_ROTO*)
        Batch *last     = sBatchCount ? &sBatches[sBatchCount - 1] : NULL;
        if (!last || last->tex != ptex || last->blend != pblend || last->constant != constant || last->depth != depth || last->scissor != sScissor ||
            last->fog != sub || last->uvk != k || last->fade != (uint8)fadeMode || last->proj != (uint8)merge) {
            if (sBatchCount >= MAX_BATCHES) {
                sQuadCount = q;
                return Refuse(GpuRender::REASON_QUAD_LIMIT);
            }
            sBatches[sBatchCount++] = { ptex, pblend, q, 0, constant, depth, sScissor, sub, (uint8)k, 1, (uint8)fadeMode, (uint8)merge };
            last                    = &sBatches[sBatchCount - 1];
        }
        auto uv = [&](int64 u, int64 v) { return (uint32)(uint16)(int16)((u + rnd) >> sh) | ((uint32)(uint16)(int16)((v + rnd) >> sh) << 16); };
        const uint32 X0 = (uint32)(x1 << 2), X1 = (uint32)(x2 << 2);
        const uint32 Y0 = (uint32)(y << 2) << 16, Y1 = (uint32)((y + n) << 2) << 16;
        if (merge) {
            // The quad's top and bottom edges half a line beyond the first's and the last's centres, on their plane; w
            // = 1 / q scaled to at most 1 (only the ratio counts), disparity * 8 + 128 (RotoLineDepth's, unrounded).
            const double h = n > 1 ? 0.5 / (n - 1) : 0;
            const double tt = -h, tb = 1 + h;
            const double qt = n > 1 ? planeQ(A, E, tt) : A.q, qb = n > 1 ? planeQ(A, E, tb) : A.q;
            auto at = [&](double t, double qv, double va, double ve) { return n > 1 ? (int64)planeAt(A, E, t, qv, va, ve) : (int64)va; };
            const int64 tu0 = at(tt, qt, A.fu0, E.fu0), tv0 = at(tt, qt, A.fv0, E.fv0), tu1 = at(tt, qt, A.fu1, E.fu1), tv1 = at(tt, qt, A.fv1, E.fv1);
            const int64 bu0 = at(tb, qb, A.fu0, E.fu0), bv0 = at(tb, qb, A.fv0, E.fv0), bu1 = at(tb, qb, A.fu1, E.fu1), bv1 = at(tb, qb, A.fv1, E.fv1);
            const double qMin = qt < qb ? qt : qb;
            auto extra = [&](double qv) {
                int wv = (int)(qMin / qv * 65535.0 + 0.5);
                wv     = wv < 1 ? 1 : wv;
                float d = sDepth;
                if (feet > 0) {
                    float f = 1.0f - feet * (float)qv;
                    f       = f < -0.5f ? -0.5f : f > 1.0f ? 1.0f : f;
                    d       = f * STEREO_FAR;
                }
                int dv = (int)lroundf(d * 8.0f) + 128;
                dv     = dv < 0 ? 0 : dv > 255 ? 255 : dv;
                return (uint32)(wv >> 8) | ((uint32)(wv & 255) << 8) | ((uint32)dv << 16);
            };
            const uint32 et = extra(qt), eb = extra(qb);
            w32[0] = X0 | Y0, w32[1] = uv(tu0, tv0), w32[2] = et;
            w32[3] = X1 | Y0, w32[4] = uv(tu1, tv1), w32[5] = et;
            w32[6] = X1 | Y1, w32[7] = uv(bu1, bv1), w32[8] = eb;
            w32[9] = X0 | Y1, w32[10] = uv(bu0, bv0), w32[11] = eb;
        }
        else {
            const uint32 rgba = pass ? 0xFFFFFFFFu : fadeMode ? 0x00FFFFFFu | ((uint32)(255 - FloorFade(sLineBuf[y] & 7)) << 24) : rgba0;
            w32[0] = X0 | Y0, w32[1] = uv(A.u0, A.v0), w32[2] = rgba;
            w32[3] = X1 | Y0, w32[4] = uv(A.u1, A.v1), w32[5] = rgba;
            w32[6] = X1 | Y1, w32[7] = uv(A.u1, A.v1), w32[8] = rgba;
            w32[9] = X0 | Y1, w32[10] = uv(A.u0, A.v0), w32[11] = rgba;
        }
        w32 += 12, ++q;
        ++last->count;
    }
    sStats.tileQuads += q - sQuadCount;
    sQuadCount = q;
    return true;
}

// ---------------------------------------------------------------- the Special Stage floor windows
//
// The Special Stage floor (512x512 tiles, 8192 px) does not fit a texture. Its near lines are drawn from two windows of
// the map kept in repeating 512x512 textures, where a map pixel lives at its position modulo the window (so when a
// window moves only the cells entering it are written: a strided copy for a column of cells, a plain one for a row):
// level 0 at full resolution (cells of a tile, 512 px of the map) for the lines up to a step of 1 map pixel a screen
// pixel, level 1 at half (each texel the map pixel in the middle of its 2x2, 1024 px) up to 2; the far floor (the
// most tiles a line) in 256-texel windows: an eighth (2048 px) up to 4, a sixteenth (4096 px) up to 8, and the whole
// map at a thirty-second for every farther line (kFloorStep), up to the horizon: lines left to the tile runs kept their
// exact fog and showed as an unfaded strip at the end of the track. Each line is one quad from the finest window that
// holds it; the rest stay tile runs.
// Fog: UFO_Setup makes the banks 1-7 fades of bank 0 towards bank 7 (bank k = bank 0 + (bank 7 - bank 0) * f_k) and
// picks one per line by distance. A window keeps the map in bank 0 only, and a line is one quad whose texture the
// combiner fades by f_k towards one colour, bank 7's average over the floor's pixels (stage 2: 62 % navy, the rest
// grey and orange, which fade to navy instead of their own dark tone; the owner sees no difference). The exact fade
// (a second texture in bank 7 and a second quad a line) took twice the VRAM and the fill. (A pass per fog group over a
// group mask was slower on the 2DS than the tile runs; a second texture unit cannot be unbound in citro3d 1.7.1:
// C3D_TexBind(1, NULL) crashed.) Checked when the palette changes: banks that are not such fades send the floor back
// to tile runs.
// VRAM: 512 KB a level (the far one 128 KB), each made when VramReserve() stays free for the 3D models.
enum { FLOOR_LEVELS = 5 };
// Per level: texels (square), map pixels per texel (log2), texels a cell, and the largest step (map pixels a screen
// pixel) of the lines it takes; a level takes no line under 3/4 of the previous level's limit (too blurred).
const unsigned kFloorTexW[FLOOR_LEVELS] = { 512, 512, 256, 256, 256 }, kFloorShift[FLOOR_LEVELS] = { 0, 1, 3, 4, 5 };
const unsigned kFloorCell[FLOOR_LEVELS] = { 16, 8, 8, 8, 8 };
const float kFloorStep[FLOOR_LEVELS] = { 1.0f, 2.0f, 4.0f, 8.0f, 1e9f };
struct FloorLevel {
    C3D_Tex tex;      // the map in bank 0
    bool alloc, valid;
    unsigned texW;    // texels (square)
    unsigned shift;   // map pixels per texel, log2
    unsigned cellTex; // texels a cell (16: a tile; 8: a block)
    int ox, oy;       // origin in cells (unwrapped)
    unsigned Cells() const { return texW / cellTex; }
    bool Whole(const TileLayer *layer) const // the whole map: never moves, holds every line (the texture repeats as it)
    {
        return (texW << shift) >= ((unsigned)TILE_SIZE << layer->widthShift) && (texW << shift) >= ((unsigned)TILE_SIZE << layer->heightShift);
    }
    int CellPx() const { return (int)(cellTex << shift); }
};
struct FloorWin {
    FloorLevel level[FLOOR_LEVELS];
    bool fogOk;
    const TileLayer *layer;
    uint32 built, seen; // content hash the windows hold / was seen last frame
    unsigned seenFrames;
    uint8 fogAlpha[PALETTE_BANK_COUNT]; // f_k * 255
    u32 fogColor;                       // what the lines fade to: bank 7's average over the floor (0xAABBGGRR)
    uint32 used[8];
    uint32 weight[PALETTE_BANK_SIZE];   // the floor's pixels of each palette index (map cells x tile pixels)
    uint32 tileUsed[TILE_COUNT / 32];   // base tiles the map uses
    uint32 tileGen;                     // bumped when one of them changes (UFO_Water animates tiles the floor never uses)
    bool usedOk;
};
FloorWin sFloor;
bool sFloorFailed;

bool FloorFadeReady() { return GpuRender::floorWindows && sFloor.fogOk && sFloor.built; }
unsigned FloorFade(unsigned bank) { return sFloor.fogAlpha[bank & 7] * (unsigned)GpuRender::floorFogPercent / 100; }
const char *sFloorWhy = "-"; // the floor windows' state for SITE REFUSED (the last frame with a floor)
uint8 sLineDone[SCREEN_YSIZE]; // per line of the rotozoom layer being emitted: 1 + the window level drawing it, 0 none
// SITE FLOOR: the last floor frame's lines as the windows drew them (DumpFloor)
struct FloorLineLog {
    int32 px, py, dx, dy;   // the scanline: position and step (16.16 map pixels)
    uint32 uv0, uv1;        // the quad's texel coordinates (1/16 texel, u | v << 16)
    uint8 level;            // 1 + the level drawing it, 0 none (tile runs)
};
FloorLineLog sFloorLog[SCREEN_YSIZE];
int sFloorLogY1, sFloorLogY2, sFloorLogX1, sFloorLogX2;
int sFloorLogOx[FLOOR_LEVELS], sFloorLogOy[FLOOR_LEVELS];
bool sFloorLogValid[FLOOR_LEVELS];
const TileLayer *sFloorLogLayer;
const u16 *sFloorPal0; // the texel table of bank 0 the cells are written with

void FreeFloor()
{
    for (FloorLevel &l : sFloor.level)
        if (l.alloc)
            C3D_TexDelete(&l.tex);
    memset(&sFloor, 0, sizeof(sFloor));
    sFloorFailed = false;
}

// Strided copy into VRAM: `bytes` of `src` in runs of `run` bytes, each followed by `gap` bytes skipped in `dst`.
void CopyToVramStrided(const void *src, void *dst, u32 bytes, u32 run, u32 gap)
{
    GSPGPU_FlushDataCache(src, bytes);
    C3D_SyncTextureCopy((u32 *)src, GX_BUFFER_DIM(run >> 4, 0), (u32 *)dst, GX_BUFFER_DIM(run >> 4, gap >> 4), bytes, 8);
}

inline unsigned FloorTileAt(const TileLayer *layer, int tx, int ty)
{
    const unsigned mx = (1u << layer->widthShift) - 1, my = (1u << layer->heightShift) - 1;
    return layer->layout[((unsigned)tx & mx) + (((unsigned)ty & my) << layer->widthShift)] & 0xFFF;
}

// One cell of a level (world cell (cx, cy)) into `col` (bank 0) at block (bx, by) of a buffer `blocksW` blocks wide.
void FloorCell(const FloorLevel &l, const TileLayer *layer, u16 *col, unsigned blocksW, unsigned bx, unsigned by, int cx, int cy)
{
    const u16 *t0 = sFloorPal0;
    static const uint8 my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    if (!l.shift) { // a tile: four blocks
        const unsigned t = FloorTileAt(layer, cx, cy);
        for (unsigned j = 0; j < 2; ++j)
            for (unsigned i = 0; i < 2; ++i) {
                const unsigned blk = ((by + j) * blocksW + bx + i) << 6;
                const uint8 *sp    = &tilesetPixels[TILE_DATASIZE * t + j * 8 * 16 + i * 8];
                for (unsigned y = 0; y < 8; ++y, sp += 16) {
                    u16 *b = col + blk + my[y];
                    b[0] = t0[sp[0]], b[1] = t0[sp[1]], b[4] = t0[sp[2]], b[5] = t0[sp[3]];
                    b[16] = t0[sp[4]], b[17] = t0[sp[5]], b[20] = t0[sp[6]], b[21] = t0[sp[7]];
                }
            }
        return;
    }
    // A block of reduced texels: each the map pixel at the middle of its (1 << shift) square.
    const unsigned blk = (by * blocksW + bx) << 6, step = 1u << l.shift;
    const int px0 = cx * l.CellPx(), py0 = cy * l.CellPx();
    static const uint8 mx[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
    for (unsigned v = 0; v < 8; ++v) {
        const int wy = py0 + (int)(v * step + step / 2);
        for (unsigned u = 0; u < 8; ++u) {
            const int wx     = px0 + (int)(u * step + step / 2);
            const unsigned t = FloorTileAt(layer, wx >> 4, wy >> 4);
            const uint8 idx  = tilesetPixels[TILE_DATASIZE * t + (wy & 15) * 16 + (wx & 15)];
            const unsigned m = blk + my[v] + mx[u];
            col[m] = t0[idx];
        }
    }
}

// Writes cell column sx (for origin x ox, y oy) or cell row sy of a level.
// The windows' moves are copied to VRAM through the staging buffer, waiting for each copy. Queued copies
// (GX_TextureCopy from a ring buffer, 2026-10-23) corrupted textures on the 2DS: a refused frame or a scene change
// reused the ring or freed the VRAM before the queue ran them (Azahar runs them at once and showed nothing).
u8 *FloorRingTake(u32) { return NULL; }

// `bytes` of `src` into VRAM in runs of `run` bytes, `gap` bytes skipped after each (0, 0: plain); queued when `async`.
void FloorCopy(const void *src, void *dst, u32 bytes, u32 run, u32 gap, bool async)
{
    GSPGPU_FlushDataCache(src, bytes);
    const u32 indim = run ? GX_BUFFER_DIM(run >> 4, 0) : 0, outdim = run ? GX_BUFFER_DIM(run >> 4, gap >> 4) : 0;
    if (async)
        GX_TextureCopy((u32 *)src, indim, (u32 *)dst, outdim, bytes, 8);
    else
        C3D_SyncTextureCopy((u32 *)src, indim, (u32 *)dst, outdim, bytes, 8);
}

bool FloorWriteColumn(FloorLevel &l, const TileLayer *layer, int ox, int oy, unsigned sx)
{
    const unsigned cells = l.Cells(), bpc = l.cellTex / 8, blocksW = l.texW / 8;
    const u32 colBytes = cells * bpc * bpc * 128;
    u8 *staging = FloorRingTake(colBytes);
    const bool async = staging != NULL;
    if (!staging)
        staging = (u8 *)C3DPresent::Staging();
    if (!staging)
        return false;
    C3DPresent::StagingWait();
    u16 *col = (u16 *)staging;
    const int cx = ox + (int)((sx - (unsigned)ox) & (cells - 1));
    for (unsigned sy = 0; sy < cells; ++sy) {
        const int cy = oy + (int)((sy - (unsigned)oy) & (cells - 1));
        FloorCell(l, layer, col, bpc, 0, sy * bpc, cx, cy);
    }
    FloorCopy(col, (u8 *)l.tex.data + sx * bpc * 128, colBytes, bpc * 128, (blocksW - bpc) * 128, async);
    ++sStats.floorWrites;
    return true;
}

bool FloorWriteRow(FloorLevel &l, const TileLayer *layer, int ox, int oy, unsigned sy)
{
    const unsigned cells = l.Cells(), bpc = l.cellTex / 8, blocksW = l.texW / 8;
    const u32 colBytes = blocksW * bpc * 128;
    u8 *staging = FloorRingTake(colBytes);
    const bool async = staging != NULL;
    if (!staging)
        staging = (u8 *)C3DPresent::Staging();
    if (!staging)
        return false;
    C3DPresent::StagingWait();
    u16 *col = (u16 *)staging;
    const int cy = oy + (int)((sy - (unsigned)oy) & (cells - 1));
    for (unsigned sx = 0; sx < cells; ++sx) {
        const int cx = ox + (int)((sx - (unsigned)ox) & (cells - 1));
        FloorCell(l, layer, col, blocksW, sx * bpc, 0, cx, cy);
    }
    FloorCopy(col, (u8 *)l.tex.data + sy * bpc * blocksW * 128, colBytes, 0, 0, async);
    ++sStats.floorWrites;
    return true;
}

// Places a level's window at origin (wantX, wantY) cells: the cells entering it, or all of it.
bool FloorMoveCells(FloorLevel &l, const TileLayer *layer, int wantX, int wantY, bool all);
bool FloorMove(FloorLevel &l, const TileLayer *layer, int wantX, int wantY, bool all)
{
    const u64 t0 = svcGetSystemTick();
    const bool ok = FloorMoveCells(l, layer, wantX, wantY, all);
    sStats.floorMoveUs += (unsigned)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000));
    return ok;
}

bool FloorMoveCells(FloorLevel &l, const TileLayer *layer, int wantX, int wantY, bool all)
{
    const int cells = (int)l.Cells();
    const int dxC = wantX - l.ox, dyC = wantY - l.oy;
    if (all || dxC >= cells || dxC <= -cells || dyC >= cells || dyC <= -cells) {
        for (unsigned sx = 0; sx < (unsigned)cells; ++sx)
            if (!FloorWriteColumn(l, layer, wantX, wantY, sx))
                return false;
    }
    else {
        // Columns entering (for the new x origin and the old y one), then rows entering (new origin).
        for (int k = 0; k < (dxC < 0 ? -dxC : dxC); ++k) {
            const int cx = dxC > 0 ? l.ox + cells + k : wantX + k;
            if (!FloorWriteColumn(l, layer, wantX, l.oy, (unsigned)cx & (unsigned)(cells - 1)))
                return false;
        }
        for (int k = 0; k < (dyC < 0 ? -dyC : dyC); ++k) {
            const int cy = dyC > 0 ? l.oy + cells + k : wantY + k;
            if (!FloorWriteRow(l, layer, wantX, wantY, (unsigned)cy & (unsigned)(cells - 1)))
                return false;
        }
    }
    l.ox = wantX, l.oy = wantY;
    return true;
}

// The palette indices the floor's pixels use, from its distinct tiles (once per layer and tileset epoch).
void FloorUsed(const TileLayer *layer)
{
    FloorWin &f = sFloor;
    if (!f.usedOk) {
        static uint32 tileCount[TILE_COUNT * 4];
        memset(tileCount, 0, sizeof(tileCount));
        memset(f.used, 0, sizeof(f.used));
        memset(f.weight, 0, sizeof(f.weight));
        memset(f.tileUsed, 0, sizeof(f.tileUsed));
        const unsigned n = (1u << layer->widthShift) << layer->heightShift;
        for (unsigned i = 0; i < n; ++i)
            if (layer->layout[i] != 0xFFFF)
                ++tileCount[layer->layout[i] & 0xFFF];
        for (unsigned t = 0; t < TILE_COUNT * 4; ++t) {
            if (!tileCount[t])
                continue;
            f.tileUsed[(t & (TILE_COUNT - 1)) >> 5] |= 1u << (t & 31);
            const uint8 *src = &tilesetPixels[TILE_DATASIZE * (t & (TILE_COUNT - 1))];
            for (unsigned k = 0; k < TILE_DATASIZE; ++k) {
                f.used[src[k] >> 5] |= 1u << (src[k] & 31);
                f.weight[src[k]] += tileCount[t];
            }
        }
        f.used[0] &= ~1u;
        f.weight[0] = 0;
        f.usedOk    = true;
    }
}

// Bank 7 averaged over the floor's pixels (0xAABBGGRR).
u32 FloorFogAverage(const uint16 *b7)
{
    const FloorWin &f = sFloor;
    double sum[3] = {}, w = 0;
    for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
        if (f.weight[i]) {
            const uint16 c = b7[i];
            sum[0] += (double)((c >> 11) << 3) * f.weight[i], sum[1] += (double)(((c >> 5) & 63) << 2) * f.weight[i];
            sum[2] += (double)((c & 31) << 3) * f.weight[i];
            w += f.weight[i];
        }
    if (w <= 0)
        return 0xFF000000u;
    const unsigned r = (unsigned)(sum[0] / w), g = (unsigned)(sum[1] / w), b = (unsigned)(sum[2] / w);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// The fades from the palette: false when the banks are not fades of bank 0 towards bank 7.
bool FloorFog(const TileLayer *layer)
{
    FloorWin &f = sFloor;
    // The banks as the frame uses them (sBankMap: a snapshot taken mid-frame).
    const uint16 *b0 = sPalShadow[sBankMap[0]], *b7 = sPalShadow[sBankMap[7]];
    auto ch          = [](uint16 c, int k) { return k == 0 ? (c >> 11) << 3 : k == 1 ? ((c >> 5) & 63) << 2 : (c & 31) << 3; };
    // Each bank's fade: the best f over the used colours, then every colour within a step and a half of what it gives.
    f.fogAlpha[0] = 0;
    for (unsigned k = 1; k < PALETTE_BANK_COUNT; ++k) {
        const uint16 *bk = sPalShadow[sBankMap[k]];
        float num = 0, den = 0;
        for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
            if (f.used[i >> 5] & (1u << (i & 31)))
                for (int c = 0; c < 3; ++c) {
                    const float a = (float)ch(b0[i], c), b = (float)ch(b7[i], c), v = (float)ch(bk[i], c);
                    num += (v - a) * (b - a), den += (b - a) * (b - a);
                }
        float fk = den > 0 ? num / den : 0;
        fk       = fk < 0 ? 0 : fk > 1 ? 1 : fk;
        for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
            if (f.used[i >> 5] & (1u << (i & 31)))
                for (int c = 0; c < 3; ++c) {
                    const float a = (float)ch(b0[i], c), b = (float)ch(b7[i], c), v = (float)ch(bk[i], c);
                    const float e = a + (b - a) * fk - v;
                    if (e > 12.0f || e < -12.0f)
                        return false;
                }
        f.fogAlpha[k] = (uint8)(fk * 255.0f + 0.5f);
    }
    // The colour the lines fade to: black (the owner remembers the far floor just darker, not bluer), by 60 % of the
    // game's fade (floorFogPercent: all of it hid the far floor). Bank 7 weighted by the floor's pixels
    // (FloorFogAverage) is the game's own on average: navy in stage 2.
    f.fogColor = GpuRender::floorFogAverage ? FloorFogAverage(b7) : 0xFF000000u;
    return true;
}

// Draws from the windows the lines [y1, y2) they hold and marks them in sLineDone; the rest are left to the tile runs.
bool EmitFloorWindow(TileLayer *layer, const ScanlineInfo *first, int x1, int x2, int y1, int y2, float feet)
{
    memset(sLineDone, 0, sizeof(sLineDone));
    if (!GpuRender::floorWindows || !sUfoScene || (GpuRender::depthMode & 512) || sFloorFailed) {
        sFloorWhy = GpuRender::floorWindows ? sFloorWhy : "off";
        return true;
    }
    // Only a layer too big for a texture of its own (the floor), and one at a time: the stage's other rotozoom layers
    // (UFO2's small 3D floor, in fog banks per line) are left to the tile runs.
    FloorWin &f = sFloor;
    if (((unsigned)TILE_SIZE << layer->widthShift) <= 1024 && ((unsigned)TILE_SIZE << layer->heightShift) <= 1024)
        return true;
    if (f.layer != layer) {
        if (f.layer)
            return true;
        f.layer = layer;
    }
    const int lineSize = x2 - x1;

    // Per level the lines it is meant for (a step up to its limit) and their samples' box in map pixels (unwrapped).
    float minX[FLOOR_LEVELS], maxX[FLOOR_LEVELS], minY[FLOOR_LEVELS], maxY[FLOOR_LEVELS];
    int count[FLOOR_LEVELS] = {};
    for (int k = 0; k < FLOOR_LEVELS; ++k)
        minX[k] = minY[k] = 1e9f, maxX[k] = maxY[k] = -1e9f;
    for (int y = y1; y < y2; ++y) {
        const ScanlineInfo &s = first[y - y1];
        const float step      = sqrtf((float)s.deform.x * s.deform.x + (float)s.deform.y * s.deform.y) / 65536.0f;
        int k = 0;
        while (k < FLOOR_LEVELS && step > kFloorStep[k])
            ++k;
        if (k == FLOOR_LEVELS)
            continue;
        const float ax = s.position.x / 65536.0f, ay = s.position.y / 65536.0f;
        const float bx = ax + s.deform.x / 65536.0f * lineSize, by = ay + s.deform.y / 65536.0f * lineSize;
        minX[k] = fminf(minX[k], fminf(ax, bx)), maxX[k] = fmaxf(maxX[k], fmaxf(ax, bx));
        minY[k] = fminf(minY[k], fminf(ay, by)), maxY[k] = fmaxf(maxY[k], fmaxf(ay, by));
        ++count[k];
    }
    int countAll = 0;
    for (int k = 0; k < FLOOR_LEVELS; ++k)
        countAll += count[k];
    if (countAll < 8) {
        sFloorWhy = "few lines";
        return true;
    }

    // The content: the colours the floor uses in every bank, tileset epoch, layout pointer. Rebuilt once it has held
    // (other colours change every frame in some stages).
    // Only the tiles the map uses count (Special Stage 4's water animates tiles of another layer every frame: with the
    // tileset's epoch the windows were rebuilt every frame and never drawn, ~13 fps).
    FloorUsed(layer);
    if (sTileChangedAny) {
        bool hit = false;
        for (unsigned w = 0; w < TILE_COUNT / 32 && !hit; ++w)
            hit = (f.tileUsed[w] & sTileChanged[w]) != 0;
        if (hit) {
            ++f.tileGen;
            f.usedOk = false;
            FloorUsed(layer);
        }
    }
    uint32 hash = 2166136261u ^ f.tileGen * 16777619u ^ (uint32)(uintptr_t)layer->layout;
    // SITE REFUSED's "changing (...)": what changed since the last frame, the tiles or which colour of which bank.
    static uint16 lastPal[PALETTE_BANK_COUNT][PALETTE_BANK_SIZE];
    static uint32 lastGen;
    static char changeWhy[48];
    changeWhy[0] = 0;
    if (f.tileGen != lastGen)
        snprintf(changeWhy, sizeof(changeWhy), "tiles");
    lastGen = f.tileGen;
    for (unsigned b = 0; b < PALETTE_BANK_COUNT; ++b) {
        const uint16 *pal = sPalShadow[sBankMap[b]];
        for (unsigned i = 1; i < PALETTE_BANK_SIZE; ++i)
            if (f.used[i >> 5] & (1u << (i & 31))) {
                hash = (hash ^ pal[i]) * 16777619u;
                if (pal[i] != lastPal[b][i] && !changeWhy[0])
                    snprintf(changeWhy, sizeof(changeWhy), "bank %u colour %u %04x->%04x", b, i, lastPal[b][i], pal[i]);
                lastPal[b][i] = pal[i];
            }
    }
    // The levels' textures, finest first; a level that did not fit is tried again while the floor is drawn (VRAM frees
    // once the 3D models are in).
    bool added = false;
    for (int k = 0; k < FLOOR_LEVELS; ++k) {
        FloorLevel &l = f.level[k];
        if (l.alloc)
            continue;
        if (k && !f.level[k - 1].alloc)
            break;
        l.texW = kFloorTexW[k], l.shift = kFloorShift[k], l.cellTex = kFloorCell[k];
        if (vramSpaceFree() < l.texW * l.texW * 2 + VramReserve())
            break;
        if (!C3D_TexInitVRAM(&l.tex, (u16)l.texW, (u16)l.texW, GPU_RGBA5551))
            break;
        C3D_TexSetFilter(&l.tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&l.tex, GPU_REPEAT, GPU_REPEAT);
        l.alloc = added = true;
    }
    if (!f.level[0].alloc) {
        sFloorWhy = "no vram";
        return true;
    }
    sFloorPal0 = sTexPal[sBankMap[0]];
    // Where each window should be: centred on its lines' box.
    int wantX[FLOOR_LEVELS], wantY[FLOOR_LEVELS];
    for (int k = 0; k < FLOOR_LEVELS; ++k) {
        const FloorLevel &l = f.level[k];
        const float half    = (float)(l.texW << l.shift) / 2;
        wantX[k] = l.alloc && count[k] ? (int)floorf(((minX[k] + maxX[k]) * 0.5f - half) / l.CellPx()) : l.ox;
        wantY[k] = l.alloc && count[k] ? (int)floorf(((minY[k] + maxY[k]) * 0.5f - half) / l.CellPx()) : l.oy;
        if (l.alloc && l.Whole(layer))
            wantX[k] = wantY[k] = 0;
    }
    if (added && f.built == hash && f.level[0].valid)
        for (int k = 0; k < FLOOR_LEVELS; ++k) {
            FloorLevel &l = f.level[k];
            if (l.alloc && !l.valid)
                l.valid = FloorMove(l, layer, wantX[k], wantY[k], true);
        }
    if (f.built != hash || !f.level[0].valid) {
        f.seenFrames = f.seen == hash ? f.seenFrames + 1 : 1;
        f.seen       = hash;
        static char why[64];
        snprintf(why, sizeof(why), "changing (%s)", changeWhy[0] ? changeWhy : "-");
        sFloorWhy = why;
        if (f.seenFrames < 2)
            return true;
        f.fogOk = FloorFog(layer);
        if (!f.fogOk) {
            sFloorWhy = "fog";
            for (FloorLevel &l : f.level)
                l.valid = false;
            return true;
        }
        for (int k = 0; k < FLOOR_LEVELS; ++k) {
            FloorLevel &l = f.level[k];
            l.valid       = false;
            if (l.alloc && FloorMove(l, layer, wantX[k], wantY[k], true))
                l.valid = true;
        }
        f.built = hash;
        ++sStats.rotoTexBuilds;
    }
    else
        for (int k = 0; k < FLOOR_LEVELS; ++k) {
            // A window moves only when its lines' box leaves it (a cell of margin): the cells that enter.
            FloorLevel &l = f.level[k];
            if (!l.valid || !count[k] || l.Whole(layer))
                continue;
            const float c = (float)l.CellPx(), cells = (float)l.Cells();
            const bool inside = minX[k] >= (l.ox + 1) * c && maxX[k] < (l.ox + cells - 1) * c && minY[k] >= (l.oy + 1) * c &&
                                maxY[k] < (l.oy + cells - 1) * c;
            if (!inside && !FloorMove(l, layer, wantX[k], wantY[k], false))
                l.valid = false;
        }

    sFloorLogY1 = y1, sFloorLogY2 = y2, sFloorLogX1 = x1, sFloorLogX2 = x2, sFloorLogLayer = layer;
    for (int k = 0; k < FLOOR_LEVELS; ++k)
        sFloorLogOx[k] = f.level[k].ox, sFloorLogOy[k] = f.level[k].oy, sFloorLogValid[k] = f.level[k].valid;
    for (int y = y1; y < y2; ++y) {
        const ScanlineInfo &s = first[y - y1];
        sFloorLog[y] = { s.position.x, s.position.y, s.deform.x, s.deform.y, 0, 0, 0 };
    }
    // Each line from the finest window holding it whole (its quad's ends, half a step outside the samples).
    unsigned lines[FLOOR_LEVELS] = {};
    for (int y = y1; y < y2; ++y) {
        const ScanlineInfo &s = first[y - y1];
        const float ax = (s.position.x - s.deform.x * 0.5f) / 65536.0f, ay = (s.position.y - s.deform.y * 0.5f) / 65536.0f;
        const float bx = ax + s.deform.x / 65536.0f * lineSize, by = ay + s.deform.y / 65536.0f * lineSize;
        const float step = sqrtf((float)s.deform.x * s.deform.x + (float)s.deform.y * s.deform.y) / 65536.0f;
        for (int k = 0; k < FLOOR_LEVELS; ++k) {
            const FloorLevel &l = f.level[k];
            // (the whole map takes any line the others did not)
            if (!l.valid || ((step > kFloorStep[k] || (k && step <= kFloorStep[k - 1] * 0.75f)) && !l.Whole(layer)))
                continue;
            const float c = (float)l.CellPx(), x0 = l.ox * c, y0 = l.oy * c, x1w = (l.ox + l.Cells()) * c - 1, y1w = (l.oy + l.Cells()) * c - 1;
            if (l.Whole(layer)) {
                // Any line, as long as its texel coordinates fit their 16 bits (a line up to two maps long).
                const float wmax = 2.0f * (float)(l.texW << l.shift);
                if (fabsf(bx - ax) > wmax || fabsf(by - ay) > wmax || fminf(ax, bx) < -wmax || fmaxf(ax, bx) > 2 * wmax || fminf(ay, by) < -wmax
                    || fmaxf(ay, by) > 2 * wmax)
                    continue;
            }
            else if (fminf(ax, bx) < x0 || fmaxf(ax, bx) > x1w || fminf(ay, by) < y0 || fmaxf(ay, by) > y1w)
                continue;
            sLineDone[y - y1] = (uint8)(1 + k);
            ++lines[k];
            break;
        }
    }
    static char why[48];
    unsigned linesAll = 0;
    for (int k = 0; k < FLOOR_LEVELS; ++k)
        linesAll += lines[k];
    snprintf(why, sizeof(why), "%u+%u+%u+%u+%u/%d lines", lines[0], lines[1], lines[2], lines[3], lines[4], countAll);
    sFloorWhy = why;
    if (!linesAll)
        return true;
    if (sQuadCount + linesAll > MAX_QUADS) {
        memset(sLineDone, 0, sizeof(sLineDone));
        return true;
    }
    // A quad a line, its alpha 1 - f: the combiner fades the texture to the fog colour by it (the draw loop). Coordinates
    // in 1/16 texel from a multiple of the window's map size below its origin (2048 texels at most), as its texels
    // repeat.
    unsigned q  = sQuadCount;
    uint32 *w32 = (uint32 *)&sVbo[sCur][q * 4];
    for (int k = 0; k < FLOOR_LEVELS; ++k) {
        if (!lines[k])
            continue;
        const FloorLevel &l = f.level[k];
        const int64 span    = (int64)(l.texW << l.shift);
        const int64 baseX = (int64)floorf((float)l.ox * l.CellPx() / span) * span, baseY = (int64)floorf((float)l.oy * l.CellPx() / span) * span;
        const unsigned sh = 12 + l.shift; // 16.16 map pixels -> 1/16 texel
        for (int y = y1; y < y2; ++y) {
            if (sLineDone[y - y1] != 1 + k)
                continue;
            const unsigned fade = f.fogAlpha[sLineBuf[y] & 7] * (unsigned)GpuRender::floorFogPercent / 100;
            const ScanlineInfo &s = first[y - y1];
            const int8 depth      = feet > 0 ? RotoLineDepth(feet, s.deform.x, s.deform.y) : sDepth;
            const uint8 ptex      = (uint8)(TEX_FLOOR0 + k), pblend = BLEND_NORMAL;
            Batch *last           = sBatchCount ? &sBatches[sBatchCount - 1] : NULL;
            if (!last || last->tex != ptex || last->blend != pblend || last->depth != depth || last->scissor != sScissor) {
                if (sBatchCount >= MAX_BATCHES) {
                    sQuadCount = q;
                    return Refuse(GpuRender::REASON_QUAD_LIMIT);
                }
                sBatches[sBatchCount++] = { ptex, pblend, q, 0, 0, depth, sScissor };
                last                    = &sBatches[sBatchCount - 1];
            }
            const int64 u0 = (int64)s.position.x - s.deform.x / 2 - (baseX << 16), v0 = (int64)s.position.y - s.deform.y / 2 - (baseY << 16);
            const int64 u1 = u0 + (int64)s.deform.x * lineSize, v1 = v0 + (int64)s.deform.y * lineSize;
            const int64 rnd = (int64)1 << (sh - 1);
            const uint32 uv0 = (uint16)(int16)((u0 + rnd) >> sh) | ((uint32)(uint16)(int16)((v0 + rnd) >> sh) << 16);
            const uint32 uv1 = (uint16)(int16)((u1 + rnd) >> sh) | ((uint32)(uint16)(int16)((v1 + rnd) >> sh) << 16);
            const uint32 X0 = (uint32)(x1 << 2), X1 = (uint32)(x2 << 2);
            const uint32 Y0 = (uint32)(y << 2) << 16, Y1 = (uint32)((y + 1) << 2) << 16;
            const uint32 rgba = 0x00FFFFFFu | ((uint32)(255 - fade) << 24); // the texture's share: 1 - f
            sFloorLog[y].uv0 = uv0, sFloorLog[y].uv1 = uv1, sFloorLog[y].level = (uint8)(1 + k);
            w32[0] = X0 | Y0, w32[1] = uv0, w32[2] = rgba;
            w32[3] = X1 | Y0, w32[4] = uv1, w32[5] = rgba;
            w32[6] = X1 | Y1, w32[7] = uv1, w32[8] = rgba;
            w32[9] = X0 | Y1, w32[10] = uv0, w32[11] = rgba;
            w32 += 12, ++q;
            ++last->count;
        }
    }
    sStats.tileQuads += q - sQuadCount;
    sQuadCount = q;
    return true;
}

// DrawDeformedSprite (UFO_Plasma's sky, OOZ's smog): every row of the clip samples the sheet along its scanline,
// wrapping, across the whole screen width (the software ignores the clip's x). One quad a line from the sheet in a
// texture of its own (RotoTex), in the command's ink.
bool EmitDeformed(const Cmd &c, const ScanlineInfo *sl)
{
    const unsigned sheetID = (unsigned)c.p[0];
    const int32 ink = c.p[1], alpha = c.p[2];
    if (sheetID >= SURFACE_COUNT || !gfxSurface[sheetID].pixels)
        return true;
    const GFXSurface &g = gfxSurface[sheetID];
    const unsigned w = (unsigned)g.width, h = (unsigned)g.height;
    if (w < 8 || h < 8 || (w & (w - 1)) || (h & (h - 1)) || w != (1u << g.lineSize))
        return Refuse(GpuRender::REASON_UNSUPPORTED, c.type);
    uint8 blend = BLEND_NORMAL;
    int32 a     = 0xFF;
    switch (ink) {
        case INK_NONE: break;
        case INK_BLEND: a = 0x80; break;
        case INK_ALPHA: a = alpha > 0xFF ? 0xFF : alpha; break;
        case INK_ADD: blend = BLEND_ADD, a = alpha; break;
        case INK_SUB: blend = BLEND_SUB, a = alpha; break;
        case INK_MASKED: blend = BLEND_MASKED; break;
        case INK_UNMASKED: blend = BLEND_UNMASKED; break;
        default: return Refuse(GpuRender::REASON_INK, (unsigned)ink);
    }
    const int y1 = c.clip[1] < 0 ? 0 : c.clip[1], y2 = c.clip[3] > SCREEN_YSIZE ? SCREEN_YSIZE : c.clip[3];
    if (y1 >= y2)
        return true;
    const RotoSrc src = { NULL, &g };
    bool handled;
    if (!EmitRotoTex(src, w, h, sl + (y1 - c.clip[1]), 0, screens[0].pitch, y1, y2, 0.0f, &handled, blend, 0x00FFFFFFu | ((uint32)a << 24)))
        return false;
    return handled ? true : Refuse(GpuRender::REASON_UNSUPPORTED, c.type);
}

// ---------------------------------------------------------------- big rotozoom layers as line strips

// A rotozoom layer too big for a texture (Pinball's table: two 2048x2048 maps whose tiles fill 29 x 112 of their
// 128 x 128) whose lines are horizontal (no y step): each screen line samples one map row, so a strip texture holds,
// in its row y, screen line y's map row over the box of the layer's non-empty columns (up to 512 pixels), in that
// line's palette bank (Pinball fogs the table by distance with banks 0-7, 105 colours in bank 7: a texture per bank
// would not fit). Each line is then one quad per turn of the map in which it crosses the box. A strip row is
// converted again only when its line's map row, bank colours or the layout change; just before C3D_FrameBegin (the
// GPU has finished the last frame, UploadStrips). As tile runs the table took ~9000 quads and ~18 ms a frame on the 2DS.
enum { STRIP_SLOTS = 2, STRIP_W = 512, STRIP_H = 256 };
struct Strip {
    const TileLayer *layer; // NULL: a free slot
    C3D_Tex tex;
    bool alloc;
    unsigned x0;               // map pixel column of the strip's column 0
    unsigned tiles;            // the box's width in tiles: the strip's columns beyond it are never sampled
    uint32 layoutHash;         // the layout (and tileset epoch) the box was taken for
    uint32 have[STRIP_H];      // what each row holds (StripKey; 0: nothing)
    uint32 want[STRIP_H];      // what this frame's lines need
    uint16 row[STRIP_H];       // their map rows
    uint8 bank[STRIP_H];       // and banks
    bool dirty;                // rows to convert after FrameBegin
    unsigned rowsUp;           // SITE REFUSED: rows converted at the last upload, and its time (us) converting / copying
    unsigned convUs, copyUs;
    uint32 lastUsed;           // sRotoFrame
};
Strip sStrips[STRIP_SLOTS];
const char *sBigWhy = "-"; // SITE REFUSED: the last big layer's state

void FreeStrip(Strip &st)
{
    if (st.alloc)
        C3D_TexDelete(&st.tex);
    memset(&st, 0, sizeof(st));
}

// The box of the layer's non-empty tile columns (inclusive); false when it has none.
bool LayoutColumns(const TileLayer *layer, int *tx0, int *tx1)
{
    int x0 = 0x7FFF, x1 = -1;
    for (int y = 0; y < layer->ysize; ++y) {
        const uint16 *row = &layer->layout[y << layer->widthShift];
        for (int x = 0; x < layer->xsize; ++x)
            if (row[x] != 0xFFFF)
                x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
    }
    *tx0 = x0, *tx1 = x1;
    return x1 >= 0;
}

// floor(a / b) for b > 0
inline int64 FloorDiv(int64 a, int64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The layer's lines [y1, y2) from its strip. *handled false: not this way (lines with a y step, a box too wide, no
// VRAM): the caller draws tile runs.
bool EmitStripRoto(TileLayer *layer, const ScanlineInfo *first, int x1, int x2, int y1, int y2, float feet, bool *handled)
{
    *handled = false;
    if (GpuRender::depthMode & 512) // debug: the tile runs as before
        return true;
    const unsigned mapW = (unsigned)TILE_SIZE << layer->widthShift, mapH = (unsigned)TILE_SIZE << layer->heightShift;
    if (mapW <= 1024 && mapH <= 1024) // RotoTex's
        return true;
    sBigWhy = "lines";
    for (int y = y1; y < y2; ++y)
        if (first[y - y1].deform.y)
            return true;
    // A layer found empty or too wide stays so until the scene changes: finding out hashes and walks its whole layout,
    // ~28 ms a frame for Special Stage 4's floor (512x512 tiles) while the camera looks along an axis (every line's y
    // step 0) at the start, until the first turn.
    static struct {
        const TileLayer *layer;
        uint32 content;
        const char *why;
    } sNot[4];
    static unsigned sNotNext;
    for (auto &n : sNot)
        if (n.layer == layer && n.content == contentVersion) {
            sBigWhy = n.why;
            return true;
        }
    auto refuse = [&](const char *why) {
        sNot[sNotNext++ % 4] = { layer, contentVersion, why };
        sBigWhy              = why;
        return true;
    };
    // The slot: the layer's, a free one, or one not drawn for a second
    Strip *st = NULL;
    for (Strip &t : sStrips)
        if (t.layer == layer)
            st = &t;
    for (unsigned i = 0; i < STRIP_SLOTS && !st; ++i)
        if (!sStrips[i].layer || sRotoFrame - sStrips[i].lastUsed > 60) {
            FreeStrip(sStrips[i]);
            st = &sStrips[i];
        }
    sBigWhy = "slots";
    if (!st)
        return true;
    // The layout (and the tileset's epoch): a change takes the box again and every row anew
    uint32 layoutHash = 2166136261u ^ sTileEpoch * 16777619u;
    {
        const uint32 *lw     = (const uint32 *)layer->layout;
        const unsigned words = ((1u << layer->widthShift) << layer->heightShift) / 2;
        for (unsigned i = 0; i < words; ++i)
            layoutHash = (layoutHash ^ lw[i]) * 16777619u;
    }
    if (st->layer != layer || st->layoutHash != layoutHash) {
        int tx0, tx1;
        if (!LayoutColumns(layer, &tx0, &tx1))
            return refuse("empty");
        if ((tx1 - tx0 + 1) * TILE_SIZE > STRIP_W)
            return refuse("too wide");
        if (!st->alloc) {
            sBigWhy = "no vram";
            if (vramSpaceFree() < STRIP_W * STRIP_H * 2 + VramReserve() || !C3D_TexInitVRAM(&st->tex, STRIP_W, STRIP_H, GPU_RGBA5551))
                return true;
            C3D_TexSetFilter(&st->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&st->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            st->alloc = true;
        }
        st->layer      = layer;
        st->x0         = (unsigned)tx0 * TILE_SIZE;
        st->tiles      = (unsigned)(tx1 - tx0 + 1);
        st->layoutHash = layoutHash;
        memset(st->have, 0, sizeof(st->have));
    }
    // Each bank's colours as a hash: a row's key is its map row, its bank's colours and the layout
    uint32 bankHash[8];
    for (unsigned b = 0; b < 8; ++b) {
        const uint32 *pw = (const uint32 *)sPalShadow[sBankMap[b]];
        uint32 h         = 2166136261u ^ b;
        for (unsigned i = 0; i < PALETTE_BANK_SIZE / 2; ++i)
            h = (h ^ pw[i]) * 16777619u;
        bankHash[b] = h;
    }

    const int lineSize = x2 - x1;
    sBigWhy            = "quads";
    if (sQuadCount + (unsigned)(y2 - y1) * 3 > MAX_QUADS)
        return true;
    *handled     = true;
    sBigWhy      = "-";
    st->lastUsed = sRotoFrame;
    const uint8 tex     = (uint8)(TEX_BIG0 + (st - sStrips));
    const int64 period  = (int64)mapW << 16;
    unsigned q          = sQuadCount;
    uint32 *w32         = (uint32 *)&sVbo[sCur][q * 4];
    Batch *own          = NULL; // the batch this layer is adding to (only its own: another's count is that emitter's)
    for (int y = y1; y < y2; ++y) {
        const ScanlineInfo &s = first[y - y1];
        const unsigned row = ((uint32)s.position.y >> 16) & (mapH - 1), bank = sLineBuf[y] & 7;
        const uint32 key   = ((row * 2654435761u) ^ bankHash[bank] ^ layoutHash) | 1u;
        st->want[y] = key, st->row[y] = (uint16)row, st->bank[y] = (uint8)bank;
        st->dirty |= st->have[y] != key;
        const int32 dx   = s.deform.x;
        const int64 p0   = (int64)(uint32)s.position.x;
        const int64 pEnd = p0 + (int64)dx * (lineSize - 1);
        const int64 lo = p0 < pEnd ? p0 : pEnd, hi = p0 < pEnd ? pEnd : p0;
        const uint32 v   = (uint32)(y * 16 + 8); // the strip row's middle, 1/16 texel
        const int8 depth = feet > 0 ? RotoLineDepth(feet, dx, 0) : sDepth;
        for (int64 turn = FloorDiv(lo, period) - 1; turn <= FloorDiv(hi, period) + 1; ++turn) {
            const int64 a = ((int64)st->x0 << 16) + turn * period, e = a + ((int64)(st->tiles * TILE_SIZE) << 16); // [a, e) in 16.16
            if (e <= lo || a > hi)
                continue;
            int64 c0, c1; // the pixels whose samples fall in [a, e)
            if (dx > 0)
                c0 = FloorDiv(a - p0 + dx - 1, dx), c1 = FloorDiv(e - p0 + dx - 1, dx);
            else if (dx < 0)
                c0 = FloorDiv(p0 - e, -dx) + 1, c1 = FloorDiv(p0 - a, -dx) + 1;
            else
                c0 = 0, c1 = lineSize;
            c0 = c0 < 0 ? 0 : c0, c1 = c1 > lineSize ? lineSize : c1;
            if (c0 >= c1 || q >= MAX_QUADS)
                continue;
            // Ends half a step outside the first and last samples (as RotoTex), in 1/16 texel of the strip
            const int64 u0 = (p0 + c0 * dx - a) - dx / 2, u1 = u0 + (int64)dx * (c1 - c0);
            if (!own || own->depth != depth || own->scissor != sScissor) {
                if (own)
                    own->count = q - own->first;
                if (sBatchCount >= MAX_BATCHES) {
                    sQuadCount = q;
                    return Refuse(GpuRender::REASON_QUAD_LIMIT);
                }
                sBatches[sBatchCount++] = { tex, BLEND_NORMAL, q, 0, 0, depth, sScissor };
                own                     = &sBatches[sBatchCount - 1];
            }
            const uint32 U0 = (uint32)(uint16)(int16)(u0 >> 12), U1 = (uint32)(uint16)(int16)(u1 >> 12);
            const uint32 X0 = (uint32)((x1 + (int)c0) << 2), X1 = (uint32)((x1 + (int)c1) << 2);
            const uint32 Y0 = (uint32)(y << 2) << 16, Y1 = (uint32)((y + 1) << 2) << 16;
            w32[0] = X0 | Y0, w32[1] = U0 | v << 16, w32[2] = 0xFFFFFFFFu;
            w32[3] = X1 | Y0, w32[4] = U1 | v << 16, w32[5] = 0xFFFFFFFFu;
            w32[6] = X1 | Y1, w32[7] = U1 | v << 16, w32[8] = 0xFFFFFFFFu;
            w32[9] = X0 | Y1, w32[10] = U0 | v << 16, w32[11] = 0xFFFFFFFFu;
            w32 += 12, ++q;
        }
    }
    if (own)
        own->count = q - own->first;
    sQuadCount = q;
    return true;
}

// Before FrameBegin: the strip rows this frame's lines need and do not hold, by blocks of 8 rows (a block's other rows
// are converted with it: the staging buffer holds no earlier content), each run of blocks one copy into VRAM.
void UploadStrips()
{
    static const uint8 my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    u16 *staging = (u16 *)C3DPresent::Staging();
    for (Strip &st : sStrips) {
        if (!st.layer || !st.dirty || !staging)
            continue;
        C3DPresent::StagingWait(); // the other strip's copies read the same rows of it
        st.dirty = false;
        st.rowsUp = 0;
        u64 tConv = 0, tCopy = 0;
        const TileLayer *layer = st.layer;
        const unsigned maskTX = layer->xsize - 1, maskTY = layer->ysize - 1, wShift = layer->widthShift;
        const unsigned blockBytes = STRIP_W * 8 * 2;
        int runStart = -1;
        for (unsigned br = 0; br <= STRIP_H / 8; ++br) {
            bool need = false;
            if (br < STRIP_H / 8)
                for (unsigned y = br * 8; y < br * 8 + 8; ++y)
                    need |= st.want[y] && st.want[y] != st.have[y];
            if (need) {
                if (runStart < 0)
                    runStart = (int)br;
                const u64 t0 = svcGetSystemTick();
                for (unsigned y = br * 8; y < br * 8 + 8; ++y) {
                    if (!st.want[y]) { // no line there this frame: the copy overwrites it with whatever staging holds
                        st.have[y] = 0;
                        continue;
                    }
                    const unsigned row = st.row[y], ty = (row >> 4) & maskTY;
                    const u16 *tex     = sTexPal[sBankMap[st.bank[y]]];
                    u16 *blkRow        = &staging[(br * (STRIP_W >> 3)) << 6];
                    const uint16 *lay  = &layer->layout[ty << wShift];
                    // Only the box's tiles (the rest of the row is never sampled); texel pairs side by side in the
                    // Morton order as one word each. (~20 ns a texel on the 2DS: ~10 ms for Pinball's 480 rows when
                    // the camera moves fast. Prefetching the tiles' pixels or writing whole 8x8 blocks did not help.)
                    for (unsigned tx = 0; tx < st.tiles; ++tx) {
                        const uint16 lt = lay[((st.x0 >> 4) + tx) & maskTX];
                        uint32 *b       = (uint32 *)(blkRow + ((tx * 2) << 6) + my[y & 7]);
                        if (lt == 0xFFFF) { // an empty tile (tile 0xFFF, transparent as the software has it)
                            b[0] = b[2] = b[8] = b[10] = 0;
                            b[32] = b[34] = b[40] = b[42] = 0;
                            continue;
                        }
                        const uint8 *sp = &tilesetPixels[TILE_DATASIZE * (lt & 0xFFF) + (row & 15) * 16];
                        b[0]  = tex[sp[0]] | (uint32)tex[sp[1]] << 16, b[2] = tex[sp[2]] | (uint32)tex[sp[3]] << 16;
                        b[8]  = tex[sp[4]] | (uint32)tex[sp[5]] << 16, b[10] = tex[sp[6]] | (uint32)tex[sp[7]] << 16;
                        b[32] = tex[sp[8]] | (uint32)tex[sp[9]] << 16, b[34] = tex[sp[10]] | (uint32)tex[sp[11]] << 16;
                        b[40] = tex[sp[12]] | (uint32)tex[sp[13]] << 16, b[42] = tex[sp[14]] | (uint32)tex[sp[15]] << 16;
                    }
                    st.have[y] = st.want[y];
                    ++st.rowsUp;
                }
                tConv += svcGetSystemTick() - t0;
            }
            else if (runStart >= 0) {
                const u32 off = (u32)runStart * blockBytes, bytes = (br - (u32)runStart) * blockBytes;
                const u64 t0 = svcGetSystemTick();
                CopyToVram((u8 *)staging + off, (u8 *)st.tex.data + off, bytes);
                tCopy += svcGetSystemTick() - t0;
                runStart = -1;
            }
        }
        memset(st.want, 0, sizeof(st.want));
        st.convUs = (unsigned)(tConv / (SYSCLOCK_ARM11 / 1000000)), st.copyUs = (unsigned)(tCopy / (SYSCLOCK_ARM11 / 1000000));
    }
}

bool EmitLayerRotozoom(const Cmd &c, TileLayer *layer, const ScanlineInfo *sl)
{
    const int x1 = c.clip[0] < 0 ? 0 : c.clip[0], x2 = c.clip[2] > screens[0].pitch ? screens[0].pitch : c.clip[2];
    const int y1 = c.clip[1] < 0 ? 0 : c.clip[1], y2 = c.clip[3] > SCREEN_YSIZE ? SCREEN_YSIZE : c.clip[3];
    if (x1 >= x2 || y1 >= y2)
        return true;
    const int lineSize = x2 - x1;
    // c.clip[1] is where the recorded scanlines start (the software reads scanlines[clipBound_Y1]).
    const ScanlineInfo *first = sl + (y1 - c.clip[1]);
    const float feet          = RotoFeet(first, y1, y2);
    auto us = [](u64 t0) { return (unsigned)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000)); };
    {
        bool handled;
        const RotoSrc src = { layer, NULL };
        const u64 t0 = svcGetSystemTick();
        const bool ok = EmitRotoTex(src, (unsigned)TILE_SIZE << layer->widthShift, (unsigned)TILE_SIZE << layer->heightShift, first, x1, x2, y1, y2, feet, &handled);
        sStats.rotoTexUs += us(t0);
        if (!ok)
            return false;
        if (handled)
            return true;
    }
    {
        bool handled;
        const u64 t0 = svcGetSystemTick();
        const bool ok = EmitStripRoto(layer, first, x1, x2, y1, y2, feet, &handled);
        sStats.rotoStripUs += us(t0);
        if (!ok)
            return false;
        if (handled)
            return true;
    }
    {
        const u64 t0 = svcGetSystemTick();
        const bool ok = EmitFloorWindow(layer, first, x1, x2, y1, y2, feet);
        sStats.rotoFloorUs += us(t0);
        if (!ok)
            return false;
    }

    // Estimate the runs: tile crossings along x and y per line, plus one.
    unsigned estimate = 0;
    for (int y = y1; y < y2; ++y) {
        if (sLineDone[y - y1])
            continue;
        const ScanlineInfo &s = first[y - y1];
        const uint32 ax = (uint32)(s.deform.x < 0 ? -s.deform.x : s.deform.x), ay = (uint32)(s.deform.y < 0 ? -s.deform.y : s.deform.y);
        estimate += (uint32)(((uint64)(ax + ay) * (uint32)lineSize) >> 20) + 2;
    }
    if (estimate > ROTO_MAX_RUNS) {
        ++sStats.rotoSoftRuns;
        return EmitSoftLayer(c, layer, sl, LAYER_ROTOZOOM);
    }
    // The layer's quads go into one batch; counts are kept in locals (stores through the vertex pointer would make the
    // compiler reload every global after each one).
    if (!sBatchCount || sBatches[sBatchCount - 1].tex != TEX_TILE_FINE || sBatches[sBatchCount - 1].blend != BLEND_NORMAL ||
        sBatches[sBatchCount - 1].depth != sDepth || sBatches[sBatchCount - 1].scissor != sScissor) {
        if (sBatchCount >= MAX_BATCHES)
            return Refuse(GpuRender::REASON_QUAD_LIMIT);
        sBatches[sBatchCount++] = { TEX_TILE_FINE, BLEND_NORMAL, sQuadCount, 0, 0, sDepth, sScissor };
    }
    Batch *batch          = &sBatches[sBatchCount - 1];
    unsigned batchFirst   = sQuadCount; // quads of the open batch start here
    const Batch keep0     = *batch;     // the batch as it was, and how many there were (for the fallback below)
    const unsigned keepBatches = sBatchCount;
    const unsigned first0 = sQuadCount;
    // Stereo: each line's depth from its distance (the walk's step grows with it), the lines at the player's feet
    // (line 200, the Special Stage camera's) on the screen plane, the horizon the farthest.
    const unsigned limit  = first0 + ROTO_MAX_QUADS < MAX_QUADS ? first0 + ROTO_MAX_QUADS : MAX_QUADS;
    unsigned q            = first0, empty = 0;
    uint32 *w             = (uint32 *)&sVbo[sCur][q * 4];

    // (bank, tile) -> the cell's texel origin in 1/32 texel (u | v << 16) for this layer: EnsureTile's tables (~200 KB)
    // would miss the cache on every run (the Old 3DS has no L2). Entries stay right for the whole layer: a cell used
    // this frame is never evicted.
    static uint32 memoKey[256], memoOrg[256];
    memset(memoKey, 0xFF, sizeof(memoKey));

    const uint32 maskX = (uint32)((TILE_SIZE << layer->widthShift) - 1) >> 4, maskY = (uint32)((TILE_SIZE << layer->heightShift) - 1) >> 4;
    const unsigned wShift = layer->widthShift;
    const uint16 *layout  = layer->layout;
    // A layer that is scaled but not turned and whose lines all share one x and the same size of step (Metallic Madness
    // 1's level layer drawn at half size: 3100 quads, 6 ms): the runs of a line are the same on every line of a tile row,
    // so one quad takes all of them (u as before, v from the first line's sample to the last's).
    bool merge = !(GpuRender::depthMode & 64) && y2 - y1 >= 2 && !(sDepths && feet > 0);
    int32 sv   = merge ? first[1].position.y - first[0].position.y : 0;
    uint8 mbank;
    merge = merge && sv > 0 && UniformBank(y1, y2, &mbank);
    for (int y = y1; merge && y < y2; ++y) {
        const ScanlineInfo &a = first[y - y1];
        merge = !sLineDone[y - y1] && a.deform.y == 0 && a.deform.x == first[0].deform.x && a.position.x == first[0].position.x &&
                (y == y1 || a.position.y - first[y - y1 - 1].position.y == sv);
    }
    for (int y = y1, step = 1; y < y2; y += step) {
        step = 1;
        if (sLineDone[y - y1]) // drawn from the floor window
            continue;
        const ScanlineInfo &s = first[y - y1];
        const unsigned bank   = sLineBuf[y];
        const int32 dx = s.deform.x, dy = s.deform.y;
        const int hx = dx >> 12, hy = dy >> 12; // half a pixel step in 1/32 texel
        if (sDepths && feet > 0) {
            const int8 depth = RotoLineDepth(feet, dx, dy);
            if (depth != batch->depth) {
                // A batch per depth band: close this one, open the next
                batch->count += q - batchFirst;
                if (batch->count == 0)
                    batch->depth = depth;
                else {
                    if (sBatchCount >= MAX_BATCHES) {
                        sQuadCount = q;
                        return Refuse(GpuRender::REASON_QUAD_LIMIT);
                    }
                    sBatches[sBatchCount++] = { TEX_TILE_FINE, BLEND_NORMAL, q, 0, 0, depth, 0 };
                    batch                   = &sBatches[sBatchCount - 1];
                }
                batchFirst = q;
            }
        }
        uint32 px = (uint32)s.position.x, py = (uint32)s.position.y; // wraps as the software's int32 does
        int m = 1;
        if (merge) {
            const uint32 row = py >> 20;
            while (y + m < y2 && ((py + (uint32)(m * sv)) >> 20) == row)
                ++m;
        }
        step            = m;
        const int hvy   = merge ? sv >> 12 : hy;
        const uint32 Y0 = (uint32)(y << 2) << 16, Y1 = (uint32)((y + m) << 2) << 16;
        RotoAxis ax, ay;
        ax.Init(px, dx), ay.Init(py, dy);
        int cx = 0;
        while (cx < lineSize) {
            // The run ends where x or y next enters another tile.
            while (ax.next <= cx)
                ax.Step();
            while (ay.next <= cx)
                ay.Step();
            int end = ax.next < ay.next ? ax.next : ay.next;
            end     = end < lineSize ? end : lineSize;
            const int n = end - cx;
            const uint16 t = layout[((px >> 20) & maskX) + (((py >> 20) & maskY) << wShift)];
            if (t != 0xFFFF) {
                const uint32 key    = (bank << 12) | (t & 0xFFF);
                const unsigned slot = (key ^ (key >> 8)) & 255;
                if (memoKey[slot] != key) {
                    const int cell = EnsureTile(t & 0xFFF, bank);
                    if (cell < 0) {
                        batch->count += q - batchFirst, sQuadCount = q;
                        return Refuse(cell == -2 ? GpuRender::REASON_ATLAS_FULL : GpuRender::REASON_TILE_RANGE);
                    }
                    memoKey[slot] = key;
                    memoOrg[slot] = (uint32)((cell % (TILE_ATLAS_W / 16)) * 512) | ((uint32)((cell / (TILE_ATLAS_W / 16)) * 512) << 16);
                }
                if (q >= limit) {
                    // Too many: drop this layer's quads and let the software draw it.
                    ++sStats.rotoSoftQuads;
                    sQuadCount                  = first0;
                    sBatchCount                 = keepBatches;
                    sBatches[sBatchCount - 1]   = keep0;
                    if (!keep0.count)
                        --sBatchCount;
                    return EmitSoftLayer(c, layer, sl, LAYER_ROTOZOOM);
                }
                // First and last samples in 1/32 texel inside the tile, kept 2/32 off its edges; the quad's ends
                // are half a pixel step further out.
                const uint32 org = memoOrg[slot];
                const int cu = (int)(org & 0xFFFF), cv = (int)(org >> 16);
                const uint32 fx = px & 0xFFFFF, fy = py & 0xFFFFF;
                const uint32 lx = (px + (uint32)((n - 1) * dx)) & 0xFFFFF, ly = (py + (uint32)((n - 1) * dy) + (uint32)((m - 1) * (merge ? sv : 0))) & 0xFFFFF;
                auto in = [](uint32 f) { const int v = (int)(f >> 11); return v < 2 ? 2 : v > 509 ? 509 : v; };
                int v0 = in(fy), v1 = in(ly);
                if (merge) {
                    // The lines' samples keep their exact spacing (clamping the ends alone skews the ones between them, one
                    // texel off in some columns), a 2/32 texel off the edges as the single lines are.
                    const int first = (int)(fy >> 11), last = first + (m - 1) * (sv >> 11);
                    if (!(sv & 0xFFFF)) {
                        // Whole texels a line: sample the middle of each line's texel, far from the edge the GPU's
                        // interpolation over a tall quad could tip over (a column a texel off, see the 2/32 below).
                        v0 = ((first >> 5) << 5) + 16;
                        v1 = v0 + (m - 1) * (sv >> 11);
                    }
                    else {
                        const int bias = last + 2 <= 511 ? 2 : 511 - last;
                        v0 = first + bias, v1 = last + bias;
                    }
                }
                const int uA = cu + in(fx) - hx, uB = cu + in(lx) + hx;
                // A single line's quad runs along its line (u and v both change from end to end); a merged quad has v
                // changing down the lines and u along the run.
                const uint32 uv0 = (uint16)uA | ((uint32)(uint16)(cv + (merge ? v0 - hvy : v0 - hvy)) << 16);
                const uint32 uv1 = (uint16)uB | ((uint32)(uint16)(cv + (merge ? v0 - hvy : v1 + hvy)) << 16);
                const uint32 uv2 = (uint16)uB | ((uint32)(uint16)(cv + v1 + hvy) << 16);
                const uint32 uv3 = (uint16)uA | ((uint32)(uint16)(cv + (merge ? v1 + hvy : v0 - hvy)) << 16);
                const uint32 X0 = (uint32)((x1 + cx) << 2), X1 = (uint32)((x1 + end) << 2);
                // Whole words (x y | u v | rgba): the vertex buffer is written through to memory, and the byte and
                // halfword stores of Vtx initialisers (32 a quad) cost most of this loop's time.
                w[0] = X0 | Y0, w[1] = uv0, w[2] = 0xFFFFFFFFu;
                w[3] = X1 | Y0, w[4] = uv1, w[5] = 0xFFFFFFFFu;
                w[6] = X1 | Y1, w[7] = merge ? uv2 : uv1, w[8] = 0xFFFFFFFFu;
                w[9] = X0 | Y1, w[10] = merge ? uv3 : uv0, w[11] = 0xFFFFFFFFu;
                w += 12, ++q;
            }
            else
                ++empty;
            px += (uint32)(n * dx);
            py += (uint32)(n * dy);
            cx = end;
        }
    }
    batch->count += q - batchFirst;
    sStats.tileQuads += q - first0;
    sStats.emptyTiles += empty;
    sQuadCount = q;
    if (!batch->count && batch == &sBatches[sBatchCount - 1])
        --sBatchCount;
    return true;
}

bool EmitLayerHScroll(const Cmd &c, TileLayer *layer, const ScanlineInfo *sl)
{
    const int y1 = c.clip[1], y2 = c.clip[3];
    uint8 bank;
    if (!UniformBank(y1, y2, &bank)) // lines in different palette banks (the water line): once per run of one bank
        return PerBankRun(c, y1, y2, [&](const Cmd &cc) { return EmitLayerHScroll(cc, layer, sl + (cc.clip[1] - y1)); });
    // screens[0], not currentScreen: the draw loop has moved that on to screen 1 (never set up on the 3DS, pitch 0,
    // which left every tile run empty).
    const int pitch = screens[0].pitch;
    // Stereo: each band of lines takes the depth of its own scroll info's horizontal parallax (a background's clouds,
    // mountains and water at their own distances), not one depth for the whole layer.
    const int pixelHeight = TILE_SIZE * layer->ysize;
    const bool bands      = sDepths && !sUfoStage && !sTitleScene && layer->lineScroll && layer->scrollInfoCount;
    auto bandDepth        = [&](int yPix) -> int8 {
        if ((unsigned)yPix >= (unsigned)pixelHeight || layer->lineScroll[yPix] >= layer->scrollInfoCount)
            return sDepth;
        return ParallaxDepth(layer->scrollInfo[layer->lineScroll[yPix]].parallaxFactor);
    };
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
            if (bands && bandDepth(yPix + (end - line)) != bandDepth(yPix))
                break;
            ++end;
        }
        if (bands) {
            sDepth = bandDepth(yPix);
            sLayerNear = sDepth < sLayerNear ? sDepth : sLayerNear;
            sLayerFar  = sDepth > sLayerFar ? sDepth : sLayerFar;
        }
        // Split the band at tile-row boundaries.
        int ly = yPix, dy = line;
        while (dy < end) {
            const int rem = 16 - (ly & 15);
            const int n   = rem < (end - dy) ? rem : (end - dy);
            const int margin = EdgeMargin();
            if (!EmitTileRun(layer, bank, ly >> 4, ly & 15, dy, n, -margin, pitch + margin, FROM_FIXED(x)))
                return false;
            dy += n;
            ly += n;
        }
        line = end;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- the mask frame (INK_MASKED / INK_UNMASKED)
//
// The software draws a masked pixel only where the screen already holds maskColor. Here the stencil keeps that: in a
// frame with masked inks every draw clears the stencil under what it covers, and a second pass of the same batch,
// writing no colour, sets it back to 1 where the fragment's colour equals the mask colour, tested by the texture
// combiner: d = colour - mask + 0.5 (ADD_SIGNED), 16 * sum((d - 0.5)^2) (DOT3_RGBA, x4), plus 1 - alpha; the alpha
// test keeps zeros. Masked batches then draw where the stencil is 1, unmasked ones where it is not.

// The mask colour as the GPU sees it after the atlas conversion (5 bits per channel, widened by replication).
u32 MaskConstant()
{
    const u16 t       = ToTexel((uint16)maskColor);
    const unsigned r5 = t >> 11, g5 = (t >> 6) & 31, b5 = (t >> 1) & 31;
    const unsigned r = (r5 << 3) | (r5 >> 2), g = (g5 << 3) | (g5 >> 2), b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// `texOnly`: the texture alone (the floor windows' lines carry their fade in the vertex alpha, not a transparency;
// UFO_Plasma's mask colour is the same in every fog bank, so the unfaded texel is what the software compares).
void SetCompareTexEnv(u32 mask, bool texOnly = false)
{
    C3D_TexEnv *e0 = C3D_GetTexEnv(0); // the fragment colour, as the colour pass computes it
    C3D_TexEnvInit(e0);
    C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(e0, C3D_Both, texOnly ? GPU_REPLACE : GPU_MODULATE);
    // Stage 1 passes the fragment on and the combiner buffer keeps its alpha for stage 4. Whether the buffer's bit 0
    // stands for stage 0 or stage 1 (the documents differ), both hold the fragment.
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvBufUpdate(C3D_Alpha, BIT(0));
    C3D_TexEnv *e2 = C3D_GetTexEnv(2); // d = colour + (1 - mask) - 0.5
    C3D_TexEnvInit(e2);
    C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, (GPU_TEVSRC)0);
    C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR);
    C3D_TexEnvFunc(e2, C3D_RGB, GPU_ADD_SIGNED);
    C3D_TexEnvColor(e2, mask);
    C3D_TexEnv *e3 = C3D_GetTexEnv(3); // 16 * |colour - mask|^2 in RGB (on the console it does not reach the alpha)
    C3D_TexEnvInit(e3);
    C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(e3, C3D_RGB, GPU_DOT3_RGBA);
    C3D_TexEnvScale(e3, C3D_Both, GPU_TEVSCALE_4);
    C3D_TexEnv *e4 = C3D_GetTexEnv(4); // alpha = the result (from red) + (1 - the fragment's alpha): transparent never counts
    C3D_TexEnvInit(e4);
    C3D_TexEnvSrc(e4, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS_BUFFER, (GPU_TEVSRC)0);
    C3D_TexEnvOpAlpha(e4, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA);
    C3D_TexEnvFunc(e4, C3D_Alpha, GPU_ADD);
}

void ResetTexEnv()
{
    for (int i = 1; i < 6; ++i)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

unsigned GpuRender::depthMode = 0;
int GpuRender::stereoFar       = 6;
bool GpuRender::depthTint      = false;
bool GpuRender::floorWindows   = true;
bool GpuRender::floorFogAverage = false;
int GpuRender::floorFogPercent  = 60;
unsigned GpuRender::gpuSkip    = 0;
const char *GpuRender::RotoTexWhy()
{
    static char buf[240];
    int n = snprintf(buf, sizeof(buf), "%s, floor %s, big %s", sRotoTexWhy, sFloorWhy, sBigWhy);
    for (const Strip &st : sStrips)
        if (st.layer && n < (int)sizeof(buf))
            n += snprintf(buf + n, sizeof(buf) - n, " | strip L%d rows %u conv %u us copy %u us", (int)(st.layer - tileLayers), st.rowsUp, st.convUs, st.copyUs);
    for (unsigned i = 0; i < ROTO_TEX_SLOTS && n < (int)sizeof(buf); ++i)
        if (sRotoTex[i].key && sRotoRuns[i][0])
            n += snprintf(buf + n, sizeof(buf) - n, " | roto %s", sRotoRuns[i]);
    return buf;
}
const char *GpuRender::DepthSummary() { return sDepthLast[0] ? sDepthLast : "- (no 3D frame yet: slider up or SET force3d 1)"; }

const GpuRender::Stats &GpuRender::GetStats() { return sStats; }

unsigned GpuRender::TakeC3DTimes(float avg[5])
{
    const unsigned n = sC3DFrames;
    for (int i = 0; i < 5; ++i) {
        avg[i]     = n ? sC3DSum[i] / n : 0.0f;
        sC3DSum[i] = 0.0f;
    }
    sC3DFrames = 0;
    return n;
}
const char *GpuRender::LastRefusal() { return sRefusal[0] ? sRefusal : "-"; }

void GpuRender::DumpFloor(void *file)
{
    FILE *f = (FILE *)file;
    if (!sFloorLogLayer) {
        fprintf(f, "no floor drawn from the windows yet\n");
        return;
    }
    const TileLayer *layer = sFloorLogLayer;
    fprintf(f, "floor lines %d..%d, x %d..%d, map %ux%u px\n", sFloorLogY1, sFloorLogY2 - 1, sFloorLogX1, sFloorLogX2,
            (unsigned)TILE_SIZE << layer->widthShift, (unsigned)TILE_SIZE << layer->heightShift);
    for (int k = 0; k < FLOOR_LEVELS; ++k) {
        const FloorLevel &l = sFloor.level[k];
        fprintf(f, "level %d: %s, %u texels, 1/%u, cell %d px, origin cells %d %d = px %d %d .. %d %d%s\n", k,
                sFloorLogValid[k] ? "valid" : "INVALID", l.texW, 1u << l.shift, l.CellPx(), sFloorLogOx[k], sFloorLogOy[k],
                sFloorLogOx[k] * l.CellPx(), sFloorLogOy[k] * l.CellPx(), (sFloorLogOx[k] + (int)l.Cells()) * l.CellPx() - 1,
                (sFloorLogOy[k] + (int)l.Cells()) * l.CellPx() - 1, l.alloc && l.Whole(layer) ? " (whole map)" : "");
    }
    // Per line: the level, the map pixels of its ends (as the windows test them) and the quad's texel coordinates; the
    // map's colour index at five points along the line (0: transparent in the map itself).
    fprintf(f, "y lvl step | map x0 y0 -> x1 y1 | uv0 -> uv1 (texels) | map index at 0 1/4 1/2 3/4 1\n");
    const int lineSize = sFloorLogX2 - sFloorLogX1;
    for (int y = sFloorLogY1; y < sFloorLogY2; ++y) {
        const FloorLineLog &g = sFloorLog[y];
        const float step = sqrtf((float)g.dx * g.dx + (float)g.dy * g.dy) / 65536.0f;
        const float ax = (g.px - g.dx * 0.5f) / 65536.0f, ay = (g.py - g.dy * 0.5f) / 65536.0f;
        const float bx = ax + g.dx / 65536.0f * lineSize, by = ay + g.dy / 65536.0f * lineSize;
        fprintf(f, "%3d %d %6.3f | %9.1f %9.1f -> %9.1f %9.1f | %8.2f %8.2f -> %8.2f %8.2f |", y, g.level ? g.level - 1 : -1, step, ax, ay, bx,
                by, (int16)(g.uv0 & 0xFFFF) / 16.0f, (int16)(g.uv0 >> 16) / 16.0f, (int16)(g.uv1 & 0xFFFF) / 16.0f,
                (int16)(g.uv1 >> 16) / 16.0f);
        for (int i = 0; i <= 4; ++i) {
            const int64 mx = ((int64)g.px + (int64)g.dx * lineSize * i / 4) >> 16, my = ((int64)g.py + (int64)g.dy * lineSize * i / 4) >> 16;
            const unsigned t = FloorTileAt(layer, (int)(mx >> 4), (int)(my >> 4));
            fprintf(f, " %3u", tilesetPixels[TILE_DATASIZE * t + (my & 15) * 16 + (mx & 15)]);
        }
        fprintf(f, "\n");
    }
}

// SITE MODELCHECK: every model in VRAM compared with what was uploaded; the VRAM blocks the renderer holds, to see what
// overlaps or ends just before a broken one.
void GpuRender::CheckModels(void *file)
{
    FILE *f = (FILE *)file;
    struct Block { const char *what; int id; const u8 *p; u32 n; };
    static Block blocks[MODEL_COUNT * 3 + 64];
    unsigned nb = 0;
    auto add = [&](const char *what, int id, const void *p, u32 n) {
        if (p && n && nb < sizeof(blocks) / sizeof(blocks[0]))
            blocks[nb++] = { what, id, (const u8 *)p, n };
    };
    for (unsigned m = 0; m < MODEL_COUNT; ++m) {
        const GModel &g = sModels[m];
        add("model vtx", (int)m, g.vtx, g.vbytes), add("model idx", (int)m, g.idx, g.ibytes), add("model col", (int)m, g.col, g.cbytes);
    }
    add("tile atlas", 0, sTileTex.data, sTileTex.size), add("sprite atlas", 0, sSprTex.data, sSprTex.size);
    add("soft layer", 0, sSoftTex.data, sSoftTex.size);
    for (int k = 0; k < FLOOR_LEVELS; ++k)
        if (sFloor.level[k].alloc)
            add("floor level", k, sFloor.level[k].tex.data, sFloor.level[k].tex.size);
    for (unsigned i = 0; i < ROTO_TEX_SLOTS; ++i) {
        for (int b = 0; b < 8; ++b)
            if (sRotoTex[i].bank[b].alloc)
                add("roto bank", (int)(i * 8 + b), sRotoTex[i].bank[b].tex.data, sRotoTex[i].bank[b].tex.size);
        if (sRotoTex[i].maskAlloc)
            add("roto mask", (int)i, sRotoTex[i].mask.data, sRotoTex[i].mask.size);
    }
    for (unsigned i = 0; i < STRIP_SLOTS; ++i)
        if (sStrips[i].alloc)
            add("strip", (int)i, sStrips[i].tex.data, sStrips[i].tex.size);
    if (sGrabReady)
        add("grab", 0, sGrabTex.data, sGrabTex.size);
    fprintf(f, "vram free %u KB, models' share %u KB\n", (unsigned)(vramSpaceFree() >> 10), (unsigned)(sModelVram >> 10));
    for (unsigned a = 0; a < nb; ++a)
        for (unsigned b = a + 1; b < nb; ++b)
            if (blocks[a].p < blocks[b].p + blocks[b].n && blocks[b].p < blocks[a].p + blocks[a].n)
                fprintf(f, "OVERLAP: %s %d %p+%lu and %s %d %p+%lu\n", blocks[a].what, blocks[a].id, blocks[a].p, (unsigned long)blocks[a].n,
                        blocks[b].what, blocks[b].id, blocks[b].p, (unsigned long)blocks[b].n);
    static u8 want[4096];
    for (unsigned m = 0; m < MODEL_COUNT; ++m) {
        const GModel &g = sModels[m];
        if (!g.ok)
            continue;
        const Model &md = modelList[m];
        const u8 *buf[3] = { (const u8 *)g.vtx, (const u8 *)g.idx, g.col };
        const u32 size[3] = { g.vbytes, g.ibytes, g.cbytes };
        static const char *const kName[3] = { "vtx", "idx", "col" };
        for (int w = 0; w < 3; ++w) {
            if (!buf[w] || !size[w])
                continue;
            u32 bad = 0, first = 0, last = 0;
            for (u32 off = 0; off < size[w]; off += sizeof(want)) {
                const u32 n = size[w] - off < sizeof(want) ? size[w] - off : sizeof(want);
                FillModel(md, g, w, off, n, want);
                for (u32 i = 0; i < n; ++i)
                    if (buf[w][off + i] != want[i]) {
                        first = bad++ ? first : off + i;
                        last  = off + i;
                    }
            }
            if (!bad) {
                fprintf(f, "model %u %s ok (%lu bytes at %p)\n", m, kName[w], (unsigned long)size[w], buf[w]);
                continue;
            }
            fprintf(f, "MODEL %u %s: %lu of %lu bytes differ, %lu..%lu (at %p)\n", m, kName[w], (unsigned long)bad, (unsigned long)size[w],
                    (unsigned long)first, (unsigned long)last, buf[w]);
            // The bad bytes as they are and as they should be (16-bit words), from the first one: what wrote them
            {
                const u32 a = first & ~1u, n = (last - a + 2) < 256 ? (last - a + 2) & ~1u : 256;
                FillModel(md, g, w, a, n, want);
                fprintf(f, "  at %lu, VRAM / uploaded (u16):\n", (unsigned long)a);
                for (u32 i = 0; i < n; i += 16) {
                    fprintf(f, "   ");
                    for (u32 j = i; j < i + 16 && j < n; j += 2)
                        fprintf(f, " %04x", *(const u16 *)(buf[w] + a + j));
                    fprintf(f, "  /");
                    for (u32 j = i; j < i + 16 && j < n; j += 2)
                        fprintf(f, " %04x", *(const u16 *)(want + j));
                    fprintf(f, "\n");
                }
            }
            // The block that ends nearest before the bad bytes (an overrun's suspect)
            const u8 *badAt = buf[w] + first;
            int near = -1;
            for (unsigned b = 0; b < nb; ++b)
                if (blocks[b].p < badAt && (near < 0 || blocks[b].p > blocks[near].p))
                    near = (int)b;
            if (near >= 0)
                fprintf(f, "  nearest block before: %s %d %p+%lu (ends %p)\n", blocks[near].what, blocks[near].id, blocks[near].p,
                        (unsigned long)blocks[near].n, blocks[near].p + blocks[near].n);
        }
    }
    fprintf(f, "blocks (%u):\n", nb);
    for (unsigned b = 0; b < nb; ++b)
        fprintf(f, "  %p+%-7lu %s %d\n", blocks[b].p, (unsigned long)blocks[b].n, blocks[b].what, blocks[b].id);
}

unsigned GpuRender::ModelDamage()
{
    static u8 want[4096];
    unsigned bad = 0;
    for (unsigned m = 0; m < MODEL_COUNT; ++m) {
        const GModel &g = sModels[m];
        if (!g.ok)
            continue;
        const u8 *buf[3] = { (const u8 *)g.vtx, (const u8 *)g.idx, g.col };
        const u32 size[3] = { g.vbytes, g.ibytes, g.cbytes };
        for (int w = 0; w < 3; ++w)
            for (u32 off = 0; buf[w] && off < size[w]; off += sizeof(want)) {
                const u32 n = size[w] - off < sizeof(want) ? size[w] - off : sizeof(want);
                FillModel(modelList[m], g, w, off, n, want);
                if (memcmp(buf[w] + off, want, n))
                    for (u32 i = 0; i < n; ++i)
                        bad += buf[w][off + i] != want[i];
            }
    }
    return bad;
}

void GpuRender::DumpBatches(void *file)
{
    FILE *f = (FILE *)file;
    fprintf(f, "batches %u, quads %u (tex blend first count depth scissor):\n", sBatchCount, sQuadCount);
    for (unsigned b = 0; b < sBatchCount; ++b) {
        const Batch &bt = sBatches[b];
        fprintf(f, "  %3u tex %2u blend %2u first %5u count %5u depth %3d scissor %u\n", b, bt.tex, bt.blend, bt.first, bt.count, bt.depth, bt.scissor);
    }
}

#if MANIA_DEV_TOOLS
const char *GpuRender::ConvReport() { return sConvReport; }
#else
const char *GpuRender::ConvReport() { return "-"; }
#endif

const char *GpuRender::ReasonName()
{
    static const char *const kCmd[]   = { "FILL", "LINE", "RECT", "CIRCLE", "CIRCLE OUTLINE", "FACE", "BLENDED FACE", "SPRITE", "ROTO SPRITE", "LAYER", "LINEBUF", "FACES", "BLENDED FACES", "3D SCENE", "PALETTE", "DEFORMED SPRITE" };
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

// Stereoscopic depth of a command, in pixels of disparity at the full slider: tile layers by their horizontal
// parallax (the playfield, factor 1.0, on the screen plane; a far background up to STEREO_FAR pixels behind it; a
// foreground faster than the camera in front), the HUD's draw groups (12 and up) 2 pixels in front, sprites drawn
// among the background layers (before the playfield) at the depth of the layer under them, everything else on the
// screen plane. An HSCROLL layer's bands get their own depths when it is emitted (EmitLayerHScroll).
static int8 UfoSpriteDepth(const Cmd &c, float k);

// The title screen: the camera never moves, so parallax says nothing. By what the title draws: the clouds (layer 2)
// at the horizon, the island (layer 3, a perspective plane) and its 3D trees (draw group 2) halfway, the mountains
// and the water's reflection (group 1) far, the logo with Sonic and the menu text (group 4) out of the screen.
static int8 TitleDepth(const Cmd &c)
{
    if (c.type == CMD_LAYER) {
        const TileLayer *layer = (const TileLayer *)(uintptr_t)c.p[0];
        const int index        = (int)(layer - tileLayers);
        return index == 2 ? STEREO_FAR : index == 3 ? STEREO_FAR / 2 : 0;
    }
    switch (c.group) {
        case 1: return STEREO_FAR;
        case 2: {
            // The island's 3D sprites (Title3DSprite): standing on the island at Z, ground at screen line
            // 152 + i with i = 0x90 * 0x2800 / Z, scaled 0x18000 * 0x90 / Z, so i = scale / 9.6. The island's line i
            // is at depth 1 - i / 87 (EmitLayerRotozoom). Unscaled ones (the scale clamps at 0x200) by their foot.
            float i = c.type == CMD_ROTO && c.p[8] > 0 ? (float)c.p[8] / 9.6f
                      : c.type == CMD_SPRITE           ? (float)(c.p[1] + c.p[3] - 152)
                                                       : 43.0f;
            i       = i < 0 ? 0 : i > 87 ? 87 : i;
            return (int8)roundf((1.0f - i / 87.0f) * STEREO_FAR);
        }
        case 4: return -(STEREO_FAR * 2 / 3);
        default: return 0; // the fade (group 12) covers the whole screen: shifted, it would leave a column bare
    }
}

static int8 CmdDepth(const Cmd &c, unsigned index)
{
    if (sTitleScene)
        return TitleDepth(c);
    if (c.type == CMD_LAYER) {
        const TileLayer *layer = (const TileLayer *)(uintptr_t)c.p[0];
        // The Special Stage: the sky layers scroll by deformation (parallax 1.0) but are the farthest thing; the floor
        // (rotozoom) gets a depth per line (EmitLayerRotozoom).
        if (sUfoStage && c.kind != LAYER_ROTOZOOM)
            return STEREO_FAR;
        // The horizontal factor: the layer's own for VSCROLL and rotozoom layers, the scroll infos' (per band of
        // lines) for HSCROLL and BASIC ones, here the nearest of them (a layer drawn in one piece).
        if ((c.kind == LAYER_HSCROLL || c.kind == LAYER_BASIC) && layer->scrollInfoCount) {
            int8 d = 127;
            for (unsigned s = 0; s < layer->scrollInfoCount; ++s) {
                const int8 sd = ParallaxDepth(layer->scrollInfo[s].parallaxFactor);
                d             = sd < d ? sd : d;
            }
            return d;
        }
        return ParallaxDepth(layer->parallaxFactor);
    }
    // What covers the whole screen (the fade to black between menus and scenes, a flash) is not shifted: it would leave a
    // column bare at one side, showing what is behind.
    if (c.type == CMD_FILL)
        return 0;
    if (c.type == CMD_RECT) {
        const bool sr = c.p[7] != 0;
        const int w = sr ? c.p[2] : FROM_FIXED(c.p[2]), h = sr ? c.p[3] : FROM_FIXED(c.p[3]);
        const int x = sr ? c.p[0] : FROM_FIXED(c.p[0]) - screens[0].position.x;
        if (w >= 400 && h >= SCREEN_YSIZE && x <= 0)
            return 0;
    }
    // The HUD and what is above it (groups 13 and up). Group 12 is the players' high group (Zone->playerDrawGroup[1]: the
    // cutscenes put them there): they stay on the playfield.
    if (c.group >= 13)
        return -2;
    if (!sUfoStage && (c.type == CMD_SPRITE || c.type == CMD_ROTO) && index < sPlayfieldEnd && sBehind > 0) {
        ++sBehindSprites;
        return sBehind;
    }
    // The Special Stage's track objects (rings, spheres, UFOs) are sprites the game scales by distance: scale =
    // 0x1000000 / Z, with the same projection as the 3D models (projectionX 8), so they get the models' depth.
    if (sUfoStage && c.type == CMD_ROTO && c.p[8] > 0)
        return UfoSpriteDepth(c, 16777216.0f);
    return 0;
}

// Z from a Special Stage sprite's scale (scale = k / Z), as a depth.
static int8 UfoSpriteDepth(const Cmd &c, float k)
{
    {
        const float z = k / (float)c.p[8];
        float f       = 1.0f - STEREO_ZC / z;
        f             = f < -0.5f ? -0.5f : f > 1.0f ? 1.0f : f;
        return (int8)roundf(f * STEREO_FAR);
    }
}

// Depths of a Special Stage frame's commands, worked out before it is emitted: an item box draws its cage as a
// wireframe 3D scene (lines, no depth of their own) and then its contents, a sprite scaled 0x2000000 / Z: the lines
// take the depth of the sprite that follows them.
static int8 sCmdDepth[8192];

static void UfoDepths(const Stream &stream)
{
    const unsigned n = stream.count < 8192 ? stream.count : 8192;
    int8 next        = 0;
    for (unsigned i = n; i-- > 0;) {
        const Cmd &c = stream.cmds[i];
        if (c.type == CMD_ROTO && c.group < 12 && c.p[8] > 0 && i > 0 && stream.cmds[i - 1].type == CMD_LINE)
            sCmdDepth[i] = UfoSpriteDepth(c, 33554432.0f);
        else if (c.type == CMD_LINE && c.group < 12)
            sCmdDepth[i] = next;
        else
            sCmdDepth[i] = CmdDepth(c, i);
        if (c.type == CMD_ROTO)
            next = sCmdDepth[i];
    }
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
        // A new scene: the last one's rotozoom textures and 3D models go too (the models waited for the next 3D
        // scene, holding VRAM: the title's island texture found none after a Special Stage).
        sSeenContent = contentVersion;
        for (unsigned i = 0; i < ROTO_TEX_SLOTS; ++i)
            FreeRotoTex(sRotoTex[i]);
        for (Strip &st : sStrips)
            FreeStrip(st);
        sRotoTexFailed = NULL;
        FreeFloor();
        FreeModels();
        FreeGrab();
        ResetCaches();
    }
    TrackPalette(false, stream.basePalette);
    if (sTileChangedAny) {
        memset(sTileChanged, 0, sizeof(sTileChanged));
        sTileChangedAny = false;
    }
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
                ++sTileEpoch; // the rotozoom textures made from the tileset are stale
                sTileChanged[t >> 5] |= 1u << (t & 31);
                sTileChangedAny = true;
                for (unsigned v = 0; v < 4; ++v) {
                    sTileGroupsOk[t + v * TILE_COUNT] = 0;
                    for (unsigned b = 0; b < BANKS; ++b)
                        sTileStamp[b][t + v * TILE_COUNT] = 0;
                }
            }
        }
    }

    sCur = (sCur + 1) & 1;
    ++sRotoFrame;
    // Both eyes when the 3D slider is up or DEBUG FORCE 3D is on (as if the slider were at the top: on a 2DS it
    // measures what 3D would cost; the right eye is drawn and never seen).
    sSlider = osGet3DSliderState();
    if (DEV_SW_FORCE3D)
        sSlider = 1.0f;
    sStereo = sSlider > 0.0f && C3DPresent::TargetRight();
    {
        // OPTIONS > 3D DEPTH scales the slider's effect
        static const float kLevel[] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
        sSlider *= kLevel[Port::depth3d >= 0 && Port::depth3d < 5 ? Port::depth3d : 2];
    }
    sDepths   = sStereo || GpuRender::depthTint;
    sUfoScene = sceneInfo.listData && !strncmp(sceneInfo.listData[sceneInfo.listPos].folder, "UFO", 3);
    sUfoStage = sDepths && sceneInfo.listData && !strncmp(sceneInfo.listData[sceneInfo.listPos].folder, "UFO", 3);
    sTitleScene = sDepths && sceneInfo.listData && !strcmp(sceneInfo.listData[sceneInfo.listPos].folder, "Title");
    if (sUfoStage)
        UfoDepths(stream);
    sPlayfieldEnd = 0;
    if (sDepths && !sUfoStage && !sTitleScene)
        for (unsigned k = 0; k < stream.count; ++k)
            if (stream.cmds[k].type == CMD_LAYER && CmdDepth(stream.cmds[k], k) <= 0)
                sPlayfieldEnd = k;
    bool ok;
    unsigned i = 0;
    u64 emitTicks[CMD_TYPES_MAX], tEmit0 = 0, tCmd = 0; // SITE EMIT: geometry building per command type
    for (int attempt = 0;; ++attempt) {
    sQuadCount    = 0;
    sBatchCount   = 0;
    sPendingCount = 0;
    sFail.reason  = REASON_OK;
    sSprFull      = false;
    sSoftJobCount = sSoftRows = 0;
    sDraw3DCount  = 0;
    sStats.softLayers = 0;
    sStats.faces      = 0;
    sStats.rotoUs = sStats.rotoQuads = 0;
    sStats.rotoBuildUs = sStats.floorMoveUs = sStats.floorWrites = 0;
    sStats.rotoTexUs = sStats.rotoStripUs = sStats.rotoFloorUs = 0;
    ok            = true;
    memset(sLineBuf, 0, sizeof(sLineBuf));
    for (unsigned b = 0; b < REAL_BANKS; ++b)
        sBankMap[b] = (uint8)b;
    sVirtUsed     = 0;
    sScissor      = 0;
    sScissorCount = 1;
    sBehind       = 0;
    sBehindSprites = 0;
    sDepthLen     = 0;
    sDepthCur[0]  = 0;

    memset(emitTicks, 0, sizeof(emitTicks));
    tEmit0 = tCmd = svcGetSystemTick();
    for (i = 0; i < stream.count && ok; ++i) {
        const Cmd &c = stream.cmds[i];
        if (i) { // the previous command's emission time, by type (SITE TIMES)
            const u64 now = svcGetSystemTick();
            emitTicks[stream.cmds[i - 1].type < CMD_TYPES_MAX ? stream.cmds[i - 1].type : 0] += now - tCmd;
            tCmd = now;
        }
        sDepth = !sDepths ? 0 : (sUfoStage && i < 8192) ? sCmdDepth[i] : CmdDepth(c, i);
        if (GpuRender::depthMode & 1024) { // debug: everything scissored to the top-left quarter (checks the mapping)
            static const int16 kQuarter[4] = { 0, 0, 200, 120 };
            ScissorFor(kQuarter);
        }
        switch (c.type) {
            case CMD_LINEBUF: memcpy(sLineBuf, stream.arena + c.arena, sizeof(sLineBuf)); break;
            case CMD_PALETTE:
                if (!ApplySnapshot((const uint16(*)[256])(stream.arena + c.arena)))
                    ok = Refuse(REASON_UNSUPPORTED, CMD_PALETTE);
                break;
            case CMD_SPRITE: ok = EmitSprite(c); break;
            case CMD_ROTO: ok = EmitRoto(c); break;
            case CMD_FACE: ok = EmitFace(c, stream.arena + c.arena); break;
            case CMD_BLENDED_FACE: ok = EmitBlendedFace(c, stream.arena + c.arena); break;
            case CMD_SCENE3D: ok = EmitScene3D(c, stream.arena + c.arena); break;
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
            case CMD_LINE: ok = EmitLine(c); break;
            case CMD_CIRCLE: ok = EmitRing(c, c.p[0], c.p[1], 0, c.p[2], (uint32)c.p[3], c.p[4], c.p[5], c.p[6] != 0); break;
            case CMD_CIRCLE_OUTLINE: ok = EmitRing(c, c.p[0], c.p[1], c.p[2], c.p[3], (uint32)c.p[4], c.p[5], c.p[6], c.p[7] != 0); break;
            case CMD_FILL: ok = EmitFill(c); break;
            case CMD_DEFORMED: ok = EmitDeformed(c, (const ScanlineInfo *)(stream.arena + c.arena)); break;
            case CMD_LAYER: {
                TileLayer *layer = (TileLayer *)(uintptr_t)c.p[0];
                const ScanlineInfo *sl = (const ScanlineInfo *)(stream.arena + c.arena);
                ++sStats.layerCmds;
                if (!layer->xsize || !layer->ysize)
                    break;
                sLayerNear = c.kind == LAYER_HSCROLL ? 127 : sDepth;
                sLayerFar  = c.kind == LAYER_HSCROLL ? -128 : sDepth;
                if (c.kind == LAYER_BASIC)
                    ok = EmitLayerBasic(c, layer, sl);
                else if (c.kind == LAYER_HSCROLL)
                    ok = EmitLayerHScroll(c, layer, sl);
                else if (c.kind == LAYER_VSCROLL)
                    ok = EmitLayerVScroll(c, layer, sl);
                else if (c.kind == LAYER_ROTOZOOM && !(GpuRender::depthMode & 256) && !sRotoSoft) {
                    const u64 t0 = svcGetSystemTick();
                    const unsigned q0 = sQuadCount;
                    ok = EmitLayerRotozoom(c, layer, sl);
                    sStats.rotoUs += (unsigned)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000));
                    sStats.rotoQuads += sQuadCount - q0;
                }
                else
                    ok = EmitSoftLayer(c, layer, sl, c.kind);
                if (sDepths && !sUfoStage) {
                    if (sLayerNear > sLayerFar) // no band emitted
                        sLayerNear = sLayerFar = sDepth;
                    sBehind = sLayerNear > 0 ? sLayerNear : 0;
                    if (sDepthLen + 24 < sizeof(sDepthCur))
                        sDepthLen += (unsigned)snprintf(sDepthCur + sDepthLen, sizeof(sDepthCur) - sDepthLen, "L%d g%d %d..%d | ",
                                                        (int)(layer - tileLayers), c.group, sLayerNear, sLayerFar);
                }
                break;
            }
            default: ok = Refuse(REASON_UNSUPPORTED, c.type); break;
        }
    }
    // depthmode bit 5 (debug): a red masked rectangle over the whole screen shows where the stencil ends at 1
    if (ok && (GpuRender::depthMode & 32))
        AddQuad(TEX_SPRITE, BLEND_MASKED, 0, 0, 400, 240, 4, 4, 4, 4, SPR_ATLAS_W, SPR_ATLAS_H, 255, 0, 0, 255);
    // The sprite atlas filled up mid-frame: empty it and build the frame once more from scratch (the tiles queued so
    // far go back to unconverted, the sprites are all new).
    // Out of quads with rotozoom layers drawn as tile runs (thousands of quads): once more with those layers in
    // software (one quad each) rather than the whole frame on the CPU.
    const bool quadRetry = !ok && sFail.reason == GpuRender::REASON_QUAD_LIMIT && sStats.rotoQuads && !sRotoSoft;
    if (ok || attempt > 0 || (!sSprFull && !quadRetry))
        break;
    for (unsigned k = 0; k < sPendingCount; ++k)
        if (sPending[k].kind == 0)
            sTileStamp[sPending[k].bank][sPending[k].id] = 0;
    if (quadRetry) {
        sRotoSoft = true;
        ++sStats.quadRetries;
    }
    else {
        ResetSprites();
        ++sStats.atlasRetries;
    }
    }
    sRotoSoft = false;
    if (ok && sDepths && !sUfoStage) {
        snprintf(sDepthCur + sDepthLen, sizeof(sDepthCur) - sDepthLen, "far %d playfield #%u sprites behind %u", STEREO_FAR,
                 sPlayfieldEnd, sBehindSprites);
        memcpy(sDepthLast, sDepthCur, sizeof(sDepthLast));
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
    if (i)
        emitTicks[stream.cmds[i - 1].type < CMD_TYPES_MAX ? stream.cmds[i - 1].type : 0] += svcGetSystemTick() - tCmd;
    {
        const float k = 1000.0f / SYSCLOCK_ARM11;
        for (unsigned t2 = 0; t2 < CMD_TYPES_MAX; ++t2)
            sStats.emitMs[t2] = emitTicks[t2] * k;
        sStats.preMs = (tEmit0 - tStart) * k;
    }
    GPU_TIME(STAGE_PCOPY, tStart);
    u64 t = svcGetSystemTick();
    C3D_RenderTarget *target = (C3D_RenderTarget *)C3DPresent::Target();
    // The line strips before the frame: inside one, citro3d only queues C3D_SyncTextureCopy for the frame's end, and
    // every strip would get what the shared staging buffer held last (Pinball's two table layers both showed the top
    // one). Outside it the copy waits for the GPU's queue: the wait FrameBegin makes anyway.
    UploadStrips();
    // Without SYNCDRAW, which waits for the next vblank (up to 16 ms): FrameBegin already waits until the GPU has
    // finished the previous frame, so the atlas cells rewritten below are no longer being read.
    C3D_FrameBegin(0);
    GPU_TIME(STAGE_PFRAME, t);
    const float tickMs = 1000.0f / SYSCLOCK_ARM11;
    sStats.beginMs     = (svcGetSystemTick() - t) * tickMs;
    sStats.eyeMs[0] = sStats.eyeMs[1] = sStats.overlayMs = 0.0f;
    t            = svcGetSystemTick();
    sTileDirtyLo = sSprDirtyLo = ~0u;
    sTileDirtyHi = sSprDirtyHi = 0;
#if MANIA_DEV_TOOLS
    sConvTopCount = sConvTiles = sConvTilesSame = sConvSprites = sConvSpritesSame = sConvSpritePixels = sConvSpriteChanged = 0;
    const u64 tConv = svcGetSystemTick();
#endif
    for (unsigned i = 0; i < sPendingCount; ++i) {
        if (sPending[i].kind == 0)
            ConvertTile(sPending[i]);
        else
            ConvertSprite(sPending[i]);
    }
#if MANIA_DEV_TOOLS
    {
        int n = snprintf(sConvReport, sizeof(sConvReport), "%.2f ms | tiles %u (%u unchanged) | sprites %u (%u with nothing to rewrite), %u px, %u written | groups changed bank 0:",
                         (svcGetSystemTick() - tConv) * (1000.0f / SYSCLOCK_ARM11), sConvTiles, sConvTilesSame, sConvSprites, sConvSpritesSame, sConvSpritePixels, sConvSpriteChanged);
        for (unsigned g = 0; g < 32 && n < (int)sizeof(sConvReport); ++g)
            if (sGroupGen[0][g] == sGen)
                n += snprintf(sConvReport + n, sizeof(sConvReport) - n, " %u-%u", g * 8, g * 8 + 7);
        for (unsigned k = 0; k < sConvTopCount && n < (int)sizeof(sConvReport); ++k) {
            const ConvEntry &e = sConvTop[k];
            n += snprintf(sConvReport + n, sizeof(sConvReport) - n, " | sheet %u %ux%u bank %u%s groups %08lx written %lu", e.sheet, e.w, e.h, e.bank,
                          e.fresh ? " whole" : "", (unsigned long)e.groups, (unsigned long)e.changed);
        }
    }
#endif
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

    bool maskFrame = false;
    for (unsigned b = 0; b < sBatchCount && !maskFrame; ++b)
        maskFrame = sBatches[b].blend == BLEND_MASKED || sBatches[b].blend == BLEND_UNMASKED;
    if (GpuRender::depthMode & 16) // debug: masked inks drawn as plain sprites
        maskFrame = false;
    const u32 maskConst = maskFrame ? MaskConstant() : 0;
    // Depth matters to the 3D scenes, the stencil to the mask frame. Clear: depth to its far end for the depth mode in
    // use (24 low bits), stencil 0 (8 high bits).
    u32 cmd3dWords = 0, cmdEye0Start = 0; // SITE TIMES: the left eye's command words, and those of its 3D scenes
    unsigned instances3d = 0, scenes3d = 0;
    {
        u32 *cb, csz;
        GPUCMD_GetBuffer(&cb, &csz, &cmdEye0Start);
    }
    C3D_RenderTarget *const eyeTarget[2] = { target, sStereo ? (C3D_RenderTarget *)C3DPresent::TargetRight() : NULL };
    C3DPresent::SetStereo(sStereo);
    for (int eye = 0; eye < (sStereo ? 2 : 1); ++eye) {
    const u64 tEye = svcGetSystemTick();
    target    = eyeTarget[eye];
    sEyeShift = sStereo ? (eye ? 0.5f : -0.5f) * sSlider : 0.0f;
    if (eye) {
        // Never overflow the command buffer (a panic, a frozen console): the right eye only when it fits again.
        u32 *cbuf, csize, coffset;
        GPUCMD_GetBuffer(&cbuf, &csize, &coffset);
        sStats.cmdEyeKB = (coffset - cmdEye0Start) * 4 / 1024;
        sStats.cmd3dKB  = cmd3dWords * 4 / 1024;
        sStats.instances3d = instances3d, sStats.scenes3d = scenes3d;
        if (coffset * 2 > csize - csize / 16) {
            ++sStats.eyesDropped;
            break;
        }
    }
    C3D_RenderTargetClear(target, (sDraw3DCount || maskFrame) ? C3D_CLEAR_ALL : C3D_CLEAR_COLOR, 0x000000FF,
                          (GpuRender::depthMode & 4) ? 0 : 0x00FFFFFF);
    C3D_FrameDrawOn(target);
    C3D_CullFace(GPU_CULL_NONE);
    auto bind2D = [&]() {
        C3D_BindProgram(&sProgram);
        C3D_SetAttrInfo(&sAttr);
        BufInfo_Init(&sBufInfo);
        BufInfo_Add(&sBufInfo, sVbo[sCur], sizeof(Vtx), 2, 0x10);
        C3D_SetBufInfo(&sBufInfo);
        ResetTexEnv();
        // In the mask frame the depth test is on (GPU_ALWAYS, no depth writes): without it the stencil was not applied.
        C3D_DepthTest(maskFrame, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
        // In the mask frame transparent texels must not touch the stencil: they are discarded.
        C3D_AlphaTest(maskFrame, GPU_GREATER, 0);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, (C3D_Mtx *)C3DPresent::Projection());
    };
    bind2D();

    int curBlend = -1, curTex = -1, curDepth = 0; // bind2D sets the projection of depth 0
    uint8 curProj = 0;                            // the rotozoom planes' program bound (bind2D binds the 2D one)
    int curTint = -1000;                          // DEPTH TINT: the depth the texture combiner paints now
    int curScissor = 0;
    // The batch before was a 3D scene: its program and state stay for the next scene (a run of them is common: 57 in a
    // row in Special Stage 2), the 2D ones come back before a 2D batch.
    bool in3D = false;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    for (unsigned b = 0; b < sBatchCount; ++b) {
        const Batch &bt = sBatches[b];
        if (in3D && bt.tex != TEX_3D) {
            bind2D(); // the 2D state again, textures and blending set anew
            curBlend = curTex = -1;
            curDepth = 0;
            curProj  = 0;
            curTint  = -1000;
            in3D     = false;
        }
        // SITE SET gpuskip (debug): batches not drawn, to weigh their GPU time: 1 rotozoom tile runs, 2 3D scenes,
        // 4 tile layers, 8 sprites / rectangles, 16 rotozoom textures and floor windows.
        if (GpuRender::gpuSkip) {
            const unsigned kind = bt.tex == TEX_TILE_FINE ? 1 : bt.tex == TEX_3D ? 2 : bt.tex == TEX_TILE ? 4 : bt.tex == TEX_SPRITE ? 8
                                : bt.tex >= TEX_FLOOR0 ? 16 : 0;
            (void)kind;
            if (GpuRender::gpuSkip & kind)
                continue;
        }
        if (bt.scissor != curScissor) {
            if (!bt.scissor)
                C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            else {
                // Screen rectangle -> the target's own pixels: it is 240 wide and 400 high, the top screen turned
                // (both axes run backwards; checked with depthmode 1024 in Azahar).
                const int16 *r = sScissors[bt.scissor];
                C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - r[3]), (u32)(400 - r[2]), (u32)(240 - r[1]), (u32)(400 - r[0]));
            }
            curScissor = bt.scissor;
        }
        if (bt.tex == TEX_3D) {
            u32 *cb0, csz0, coff0;
            GPUCMD_GetBuffer(&cb0, &csz0, &coff0);
            Draw3DSceneGpu(bt.first, in3D);
            in3D = true;
            u32 *cb1, csz1, coff1;
            GPUCMD_GetBuffer(&cb1, &csz1, &coff1);
            if (!eye) {
                cmd3dWords += coff1 - coff0;
                instances3d += (unsigned)sDraw3D[bt.first].h->count;
                ++scenes3d;
            }
            continue;
        }
        if (bt.proj != curProj) {
            // To the rotozoom planes' program or back. Its projection and uvscale are the 2D program's registers, but
            // binding a program loads its constants, which may sit where the other keeps a uniform: both set again.
            C3D_BindProgram(bt.proj ? &sRotoProg : &sProgram);
            if (bt.proj)
                C3D_FVUnifSet(GPU_VERTEX_SHADER, sRotoEyeLoc, (eye ? 0.5f : -0.5f) * sSlider, 0.0f, 0.0f, 0.0f);
            curProj  = bt.proj;
            curDepth = -1000;
            curTex   = -1;
        }
        if (bt.depth != curDepth) {
            // This eye's view of the batch: shifted by half its disparity, left for the left eye when behind.
            C3D_Mtx m;
            Mtx_Copy(&m, (const C3D_Mtx *)C3DPresent::Projection());
            // Whole pixels: a fraction moved the 1-pixel floor runs' samples off their tiles (stray texels in 3D)
            const int depth = bt.depth;
            Mtx_Translate(&m, roundf((eye ? 0.5f : -0.5f) * depth * sSlider), 0.0f, 0.0f, true);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, &m);
            curDepth = bt.depth;
        }
        if (bt.blend == BLEND_TINT) {
            // INK_TINT: what this eye's target holds so far goes to the GPU (split), the transfer engine copies it into
            // the grab texture (queued after that part), and the quads redraw it grey from there, opaque:
            // k * r + k * g + k * b + c over four combiner stages (each clamps at 1, as the table caps at 255).
            C3D_FrameSplit(0);
            GX_TextureCopy((u32 *)target->frameBuf.colorBuf, GX_BUFFER_DIM(240 * 8 * 2 >> 4, 0), (u32 *)sGrabTex.data,
                           GX_BUFFER_DIM(240 * 8 * 2 >> 4, (256 - 240) * 8 * 2 >> 4), 240 * 400 * 2, 8);
            C3D_TexBind(0, &sGrabTex);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvLoc, 1.0f / 256.0f, -1.0f / 512.0f, 0.0f, 0.0f);
            const u32 k = bt.constant & 0xFF, cc = (bt.constant >> 8) & 0xFF;
            static const GPU_TEVOP_RGB kOp[3] = { GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B };
            for (int st = 0; st < 3; ++st) {
                C3D_TexEnv *env = C3D_GetTexEnv(st);
                C3D_TexEnvInit(env);
                C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, st ? GPU_PREVIOUS : GPU_CONSTANT);
                C3D_TexEnvOpRgb(env, kOp[st], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
                C3D_TexEnvFunc(env, C3D_RGB, st ? GPU_MULTIPLY_ADD : GPU_MODULATE);
                C3D_TexEnvSrc(env, C3D_Alpha, st ? GPU_PREVIOUS : GPU_PRIMARY_COLOR, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
                C3D_TexEnvColor(env, 0xFF000000u | k << 16 | k << 8 | k);
            }
            C3D_TexEnv *env = C3D_GetTexEnv(3);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, (GPU_TEVSRC)0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvColor(env, 0xFF000000u | cc << 16 | cc << 8 | cc);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            if (maskFrame) { // what it covers is no longer the mask colour
                C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0xFF);
                C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_ZERO);
            }
            C3D_DrawElements(GPU_TRIANGLES, (int)(bt.count * 6), C3D_UNSIGNED_SHORT, sIdx + bt.first * 6);
            ResetTexEnv();
            curBlend = curTex = -1;
            curTint  = -1000;
            continue;
        }
        const bool floorTex = bt.tex >= TEX_FLOOR0 && bt.tex <= TEX_FLOOR4;
        const bool fadeEnv  = floorTex || bt.fade; // the floor's fade in the combiner
        if (curTex != bt.tex && floorTex) {
            const FloorLevel &l = sFloor.level[bt.tex - TEX_FLOOR0];
            C3D_TexBind(0, (C3D_Tex *)&l.tex);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvLoc, 1.0f / (l.texW * 16.0f), -1.0f / (l.texW * 16.0f), 0.0f, 0.0f);
            curTex = bt.tex;
        }
        if (bt.tex >= TEX_BIG0 && curTex != bt.tex) {
            C3D_TexBind(0, (C3D_Tex *)&sStrips[bt.tex - TEX_BIG0].tex);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvLoc, 1.0f / (STRIP_W * 16.0f), -1.0f / (STRIP_H * 16.0f), 0.0f, 0.0f);
            curTex = bt.tex;
        }
        const int rotoKey = (int)(bt.tex | (bt.blend == BLEND_HOT ? 0 : bt.fog << 8) | bt.uvk << 16);
        if (bt.tex >= TEX_ROTO0 && bt.tex < TEX_BIG0 && curTex != rotoKey) {
            // A rotozoom layer's texture: the copy for the batch's bank (Batch::fog), or its hot-colour mask; texel
            // units of 1/16 << uvk.
            const bool mask  = bt.tex >= TEX_ROTOMASK0;
            const RotoTex &r = sRotoTex[bt.tex - (mask ? TEX_ROTOMASK0 : TEX_ROTO0)];
            C3D_TexBind(0, (C3D_Tex *)(mask ? &r.mask : &r.bank[bt.fog & 7].tex));
            const float unit = 16.0f / (float)(1u << bt.uvk);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvLoc, 1.0f / (r.w * unit), -1.0f / (r.h * unit), 0.0f, 0.0f);
            curTex = rotoKey;
        }
        if (curTex != bt.tex && bt.tex < TEX_FLOOR0) { // the atlases and the software-layer texture
            const bool tile = bt.tex == TEX_TILE || bt.tex == TEX_TILE_FINE;
            C3D_TexBind(0, tile ? &sTileTex : bt.tex == TEX_SOFT ? &sSoftTex : &sSprTex);
            const float fine = bt.tex == TEX_TILE_FINE ? 32.0f : 1.0f;
            const float w    = (tile ? TILE_ATLAS_W : bt.tex == TEX_SOFT ? SOFT_W : SPR_ATLAS_W) * fine;
            const float h    = (tile ? TILE_ATLAS_H : bt.tex == TEX_SOFT ? SOFT_H : SPR_ATLAS_H) * fine;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUvLoc, 1.0f / w, -1.0f / h, 0.0f, 0.0f);
            curTex = bt.tex;
        }
        if (bt.blend == BLEND_HOT) {
            // A rotozoom texture's hot colour: where the mask equals the reference, its colour now, opaque. DEPTH TINT
            // shows the planes, not the colours: skipped there.
            if (GpuRender::depthTint)
                continue;
            C3D_TexEnv *env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_TexEnvColor(env, 0xFF000000u | (bt.constant & 0xFFFFFFu));
            C3D_AlphaTest(true, GPU_EQUAL, (int)(bt.constant >> 24));
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            if (maskFrame) { // what it covers is no longer the mask colour
                C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0xFF);
                C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_ZERO);
            }
            C3D_DrawElements(GPU_TRIANGLES, (int)(bt.count * 6), C3D_UNSIGNED_SHORT, sIdx + bt.first * 6);
            ResetTexEnv();
            C3D_AlphaTest(maskFrame, GPU_GREATER, 0);
            curBlend = -1;
            curTint  = -1000;
            continue;
        }
        // DEPTH TINT (debug): every batch one flat colour by its depth, keeping its silhouette; inks drawn as plain
        // alpha so a dim or additive picture does not read as a far plane. As in ../zelda-tmc-3ds: blue, dark far
        // behind the screen, bright in front of it. Rectangles and fills (fades) keep their colours.
        const bool tinted = GpuRender::depthTint && bt.blend != BLEND_FILL;
        auto applyTint = [&]() {
            float t = (float)(STEREO_FAR > 0 ? STEREO_FAR : 1) - bt.depth; // 0 farthest .. STEREO_FAR the screen plane
            t       = t < 0 ? 0 : t;
            const float k = 12.0f / (float)(STEREO_FAR > 0 ? STEREO_FAR * 1.5f : 1.5f); // the front (-far/2) at 12
            float v       = t * k;
            v             = v > 12.0f ? 12.0f : v;
            const unsigned r = (unsigned)(v * 5.0f), g = (unsigned)(v * 10.0f), bl = 40u + (unsigned)(v * 17.0f);
            C3D_TexEnv *env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_TexEnvColor(env, 0xFF000000u | (bl << 16) | (g << 8) | r);
            curTint = bt.depth;
        };
        if (tinted && curTint != bt.depth)
            applyTint();
        else if (!tinted && fadeEnv) {
            // A floor window line: the texture faded by the vertex alpha's complement towards the fog colour
            // (INTERPOLATE: texture * a + fog * (1 - a)), opaque where the map is.
            if (curTint != -2000) {
                C3D_TexEnv *env = C3D_GetTexEnv(0);
                C3D_TexEnvInit(env);
                C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
                C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
                C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
                C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
                C3D_TexEnvColor(env, sFloor.fogColor);
                curTint = -2000;
            }
        }
        else if (!tinted && bt.blend == BLEND_SUB) {
            // INK_SUB takes away the colour's complement, alpha * (31 - c) a channel (subtractLookupTable), not the
            // colour: Green Hill's water pool (0xA0E8F0) leaves the blue, where subtracting the colour went black.
            if (curTint != -3000) {
                ResetTexEnv();
                C3D_TexEnv *env = C3D_GetTexEnv(1);
                C3D_TexEnvInit(env);
                C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
                C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
                C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
                curTint = -3000;
            }
        }
        else if (!tinted && curTint != -1000) {
            ResetTexEnv();
            curTint = -1000;
        }
        const int blend = tinted && bt.blend != BLEND_MASKED && bt.blend != BLEND_UNMASKED ? BLEND_NORMAL : bt.blend;
        if (curBlend != blend || blend == BLEND_FILL) {
            switch (blend) {
                case BLEND_FILL:
                    C3D_BlendingColor(bt.constant);
                    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_ONE_MINUS_CONSTANT_COLOR, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
                    break;
                case BLEND_ADD: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE); break;
                case BLEND_SUB: C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE); break;
                default: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); break;
            }
            curBlend = blend;
        }
        if (maskFrame) {
            switch (bt.blend) {
                case BLEND_MASKED: // only on the mask colour, which it covers: 1 -> 2 (drawn; the second pass decides)
                    C3D_StencilTest(true, GPU_EQUAL, 1, 0xFF, 0xFF);
                    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_INCR);
                    break;
                case BLEND_UNMASKED: // only off the mask colour
                    C3D_StencilTest(true, GPU_NOTEQUAL, 1, 0xFF, 0xFF);
                    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_ZERO);
                    break;
                case BLEND_FILL: C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF); break;
                default: // what it covers is no longer the mask colour (the second pass may say it is)
                    C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0xFF);
                    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_ZERO);
                    break;
            }
        }
        // The second pass: stencil 1 where the drawn colour is the mask colour (blended colours are not judged). After
        // a masked draw only where it drew (2 -> 1); a 2 left behind means "drawn over, not the mask colour".
        auto secondPass = [&](unsigned first, unsigned count) {
            SetCompareTexEnv(maskConst, (bt.tex >= TEX_FLOOR0 && bt.tex <= TEX_FLOOR4) || bt.fade);
            if (GpuRender::depthMode & 8) {
                // debug: paint the result unblended; with bit 7 the final alpha as grey
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                if (GpuRender::depthMode & 128) {
                    C3D_TexEnv *e5 = C3D_GetTexEnv(5);
                    C3D_TexEnvInit(e5);
                    C3D_TexEnvSrc(e5, C3D_RGB, GPU_PREVIOUS, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
                    C3D_TexEnvOpRgb(e5, GPU_TEVOP_RGB_SRC_ALPHA);
                    C3D_TexEnvFunc(e5, C3D_RGB, GPU_REPLACE);
                }
            }
            C3D_AlphaTest(!(GpuRender::depthMode & 8), GPU_LESS, 3);
            C3D_DepthTest(true, GPU_ALWAYS, (GpuRender::depthMode & 8) ? GPU_WRITE_COLOR : (GPU_WRITEMASK)0);
            if (bt.blend == BLEND_MASKED) {
                C3D_StencilTest(true, GPU_EQUAL, 2, 0xFF, 0xFF);
                C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_DECR);
            }
            else {
                C3D_StencilTest(true, GPU_ALWAYS, 1, 0xFF, 0xFF);
                C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
            }
            C3D_DrawElements(GPU_TRIANGLES, (int)(count * 6), C3D_UNSIGNED_SHORT, sIdx + first * 6);
            ResetTexEnv();
            curBlend = -1; // the debug view may have changed the blending
            curTint  = -1000;

            C3D_AlphaTest(true, GPU_GREATER, 0);
            C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
        };
        if (maskFrame && bt.blend == BLEND_MASKED && !bt.disjoint) {
            // Quad by quad: inside one draw call a later quad would see the stencil an earlier one changed (1 -> 2)
            // before the second pass settled it, and draw nothing. (Disjoint quads, one a line, go in one draw: 240
            // lines of UFO_Plasma one by one took ~10 ms of submit and 95 % of the command buffer.)
            for (unsigned q = 0; q < bt.count; ++q) {
                if (tinted && curTint != bt.depth) // the second pass reset the combiner
                    applyTint();
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
                C3D_StencilTest(true, GPU_EQUAL, 1, 0xFF, 0xFF);
                C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_INCR);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, sIdx + (bt.first + q) * 6);
                secondPass(bt.first + q, 1);
            }
            continue;
        }
        C3D_DrawElements(GPU_TRIANGLES, (int)(bt.count * 6), C3D_UNSIGNED_SHORT, sIdx + bt.first * 6);
        if (maskFrame && (bt.blend == BLEND_NORMAL || bt.blend == BLEND_UNMASKED || bt.blend == BLEND_MASKED))
            secondPass(bt.first, bt.count);
    }
    if (in3D || curProj)
        bind2D();
    // citro3d's state is sticky and citro2d (the bottom screen) draws next: leave no stencil, alpha test or scissor behind.
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    ResetTexEnv();
    const u64 tOverlay = svcGetSystemTick();
    BottomUi::DrawTopOverlayGpu(target); // OPTIONS > SHOW FPS
    sStats.overlayMs += (svcGetSystemTick() - tOverlay) * tickMs;
    sStats.eyeMs[eye] = (tOverlay - tEye) * tickMs;
    } // eye
    {
        // How full the GPU command buffer got (it overflowing panics libctru, a hang on the console)
        u32 *cbuf, csize, coffset;
        GPUCMD_GetBuffer(&cbuf, &csize, &coffset);
        const unsigned pct = csize ? (unsigned)((u64)coffset * 100 / csize) : 0;
        sStats.cmdBufPct = pct > sStats.cmdBufPct ? pct : sStats.cmdBufPct;
    }
    BottomUi::DrawInTopFrame(); // the GAME tab's time, at the game's rate
    const u64 tEnd = svcGetSystemTick();
    C3D_FrameEnd(0);
    sStats.endMs = (svcGetSystemTick() - tEnd) * tickMs;
    sC3DSum[0] += sStats.beginMs, sC3DSum[1] += sStats.eyeMs[0], sC3DSum[2] += sStats.eyeMs[1];
    sC3DSum[3] += sStats.overlayMs, sC3DSum[4] += sStats.endMs;
    ++sC3DFrames;
    // citro3d's timings of the last finished GPU frame: drawing (the PICA's work) and processing (the CPU's submit).
    sStats.gpuDrawMs = C3D_GetDrawingTime();
    sStats.gpuProcMs = C3D_GetProcessingTime();
    sStats.batches   = sBatchCount;
    GPU_TIME(STAGE_PFRAME, t);
    return true;
}

#endif
