#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/C3DPresent.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <string.h>

#include "present_shbin.h"
#include "RSDK/Dev/CTR/DevTools.hpp"

using namespace RSDK;

enum { SCREEN_W = 400, SCREEN_H = 240, TEX_W = 512, TEX_H = 256 };

struct Vertex {
    float x, y, z, u, v;
};

static bool sReady, sFailed, sCore, sCoreFailed;
static C3D_RenderTarget *sTarget;
static C3D_Tex sTex;
static DVLB_s *sDvlb;
static shaderProgram_s sProgram;
static int sProjLoc;
static C3D_Mtx sProj;
static void *sStaging; // linear memory: the CPU writes the picture here, the display-transfer engine reads it
                       // (VRAM is not writable by the CPU from an installed title: the first try crashed in memcpy)
static Vertex *sQuad;  // linear memory
static C3D_BufInfo sBuf;
static C3D_AttrInfo sAttr;

void C3DPresent::ShutdownCore()
{
    if (!sCore)
        return;
    C3D_FrameSync(); // the last frame's commands and transfers are done
    C2D_Fini();
    C3D_Fini();
    sCore = sReady = false;
}

void *C3DPresent::Staging() { return sStaging; }

bool C3DPresent::InitCore()
{
    if (sCore)
        return true;
    if (sCoreFailed)
        return false;
    sCoreFailed = true;
    // A small command buffer: the app's linear heap holds the screens' buffers too. Every glyph of the bottom
    // screen is a 2D object; past the limit citro2d reuses its vertex buffer mid-frame and only the last
    // objects show (256 left the bottom screen blank). 768 objects are ~150 KB of linear memory.
    if (!C3D_Init(0x8000))
        return false;
    if (!C2D_Init(768))
        return false;
    sCore       = true;
    sCoreFailed = false;
    return true;
}

bool C3DPresent::CoreReady() { return sCore; }

bool C3DPresent::Init()
{
    if (sReady)
        return true;
    if (sFailed)
        return false; // one try only: a failed Init must not run again every frame
    sFailed = true;
    if (!InitCore())
        return false;

    sTarget = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W, GPU_RB_RGB565, (GPU_DEPTHBUF)-1);
    if (!sTarget)
        return false;
    C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    if (!C3D_TexInitVRAM(&sTex, TEX_W, TEX_H, GPU_RGB565))
        return false;
    C3D_TexSetFilter(&sTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    sStaging = linearAlloc(TEX_W * TEX_H * 2);
    if (sStaging)
        memset(sStaging, 0, TEX_W * TEX_H * 2);
    sQuad    = (Vertex *)linearAlloc(4 * sizeof(Vertex));
    if (!sStaging || !sQuad)
        return false;

    const float u = (float)SCREEN_W / TEX_W, v = (float)SCREEN_H / TEX_H;
    // A triangle strip; positions are screen pixels (y down). The PICA samples memory row 0 at v = 1 and
    // the last row of the texture (255) at v = 0, so the picture (rows 0..239 of 256) runs from v = 1 at
    // the top of the screen down to v = 1 - 240/256 at the bottom.
    const float vBottom = 1.0f - v;
    sQuad[0] = { 0, 0, 0.5f, 0, 1.0f };
    sQuad[1] = { 0, (float)SCREEN_H, 0.5f, 0, vBottom };
    sQuad[2] = { (float)SCREEN_W, 0, 0.5f, u, 1.0f };
    sQuad[3] = { (float)SCREEN_W, (float)SCREEN_H, 0.5f, u, vBottom };

    sDvlb = DVLB_ParseFile((u32 *)present_shbin, sizeof(present_shbin));
    if (!sDvlb)
        return false;
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sProjLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");

    AttrInfo_Init(&sAttr);
    AttrInfo_AddLoader(&sAttr, 0, GPU_FLOAT, 3); // position
    AttrInfo_AddLoader(&sAttr, 1, GPU_FLOAT, 2); // texture coordinates
    C3D_SetAttrInfo(&sAttr);

    BufInfo_Init(&sBuf);
    BufInfo_Add(&sBuf, sQuad, sizeof(Vertex), 2, 0x10);
    C3D_SetBufInfo(&sBuf);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    Mtx_OrthoTilt(&sProj, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0.0f, 0.0f, 1.0f, true);

    sReady  = true;
    sFailed = false;
    return true;
}

bool C3DPresent::Ready() { return sReady; }
void *C3DPresent::Target() { return sTarget; }
void *C3DPresent::Projection() { return &sProj; }

void C3DPresent::Present(const unsigned short *src, int pitch)
{
    if (!sReady)
        return;
    // The staging buffer has the texture's own size (512x256), so the display transfer is a plain tiling
    // with equal input and output dimensions; the picture is copied into its top-left corner.
    {
        DEV_STAGE(STAGE_PCOPY);
        unsigned short *dst = (unsigned short *)sStaging;
        for (int y = 0; y < SCREEN_H; ++y)
            memcpy(dst + y * TEX_W, src + y * pitch, SCREEN_W * 2);
        GSPGPU_FlushDataCache(sStaging, TEX_W * TEX_H * 2);
    }

    {
    DEV_STAGE(STAGE_PXFER);
    C3D_SyncDisplayTransfer((u32 *)sStaging, GX_BUFFER_DIM(TEX_W, TEX_H), (u32 *)sTex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    }

    DEV_STAGE(STAGE_PFRAME);
    C3D_FrameBegin(DEV_SW_SYNCDRAW ? C3D_FRAME_SYNCDRAW : 0);
    C3D_FrameDrawOn(sTarget);
    // citro2d (the bottom screen) changes the pipeline state between our frames: set ours again.
    C3D_BindProgram(&sProgram);
    C3D_SetAttrInfo(&sAttr);
    C3D_SetBufInfo(&sBuf);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, &sProj);
    C3D_TexBind(0, &sTex);
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    C3D_FrameEnd(0);
}

#endif
