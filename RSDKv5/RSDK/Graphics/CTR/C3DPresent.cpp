#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/C3DPresent.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <citro3d.h>
#include <string.h>

#include "present_shbin.h"

using namespace RSDK;

enum { SCREEN_W = 400, SCREEN_H = 240, TEX_W = 512, TEX_H = 256 };

struct Vertex {
    float x, y, z, u, v;
};

static bool sReady, sFailed;
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

bool C3DPresent::Init()
{
    if (sReady)
        return true;
    if (sFailed)
        return false; // one try only: a failed Init must not run again every frame
    sFailed = true;
    // A small command buffer: the app's linear heap is only a couple of MB and holds the screens.
    if (!C3D_Init(0x8000))
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
    // A triangle strip; positions are screen pixels (y down), the picture's row 0 is the top.
    sQuad[0] = { 0, 0, 0.5f, 0, 0 };
    sQuad[1] = { 0, (float)SCREEN_H, 0.5f, 0, v };
    sQuad[2] = { (float)SCREEN_W, 0, 0.5f, u, 0 };
    sQuad[3] = { (float)SCREEN_W, (float)SCREEN_H, 0.5f, u, v };

    sDvlb = DVLB_ParseFile((u32 *)present_shbin, sizeof(present_shbin));
    if (!sDvlb)
        return false;
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sProjLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); // position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); // texture coordinates

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

void C3DPresent::Present(const unsigned short *src, int pitch)
{
    if (!sReady)
        return;
    // The staging buffer has the texture's own size (512x256), so the display transfer is a plain tiling
    // with equal input and output dimensions; the picture is copied into its top-left corner.
    unsigned short *dst = (unsigned short *)sStaging;
    for (int y = 0; y < SCREEN_H; ++y)
        memcpy(dst + y * TEX_W, src + y * pitch, SCREEN_W * 2);
    GSPGPU_FlushDataCache(sStaging, TEX_W * TEX_H * 2);

    C3D_SyncDisplayTransfer((u32 *)sStaging, GX_BUFFER_DIM(TEX_W, TEX_H), (u32 *)sTex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameDrawOn(sTarget);
    C3D_BindProgram(&sProgram);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, &sProj);
    C3D_TexBind(0, &sTex);
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    C3D_FrameEnd(0);
}

#endif
