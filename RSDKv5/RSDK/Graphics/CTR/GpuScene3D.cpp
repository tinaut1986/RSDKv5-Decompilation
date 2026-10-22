#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/GpuScene3D.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <string.h>

#include "RSDK/Dev/CTR/DevTools.hpp"
#include "RSDK/Graphics/CTR/GpuFrame.hpp"
#include "RSDK/Graphics/CTR/GpuFrameCmd.hpp"

using namespace RSDK;

namespace
{

enum { POOL = 1024 };
GpuScene3D::Inst sPool[POOL];
uint16 sPoolScene[POOL];
unsigned sPoolCount;

// Per model: whether its coordinates fit the 16-bit vertex format (positions, normals / 4); checked once per content.
uint8 sFits[MODEL_COUNT]; // 0 unknown, 1 yes, 2 no
unsigned sFitsVersion = ~0u;

bool ModelFits(uint16 m)
{
    if (sFitsVersion != GpuFrame::contentVersion) {
        memset(sFits, 0, sizeof(sFits));
        sFitsVersion = GpuFrame::contentVersion;
    }
    if (!sFits[m]) {
        const Model &md = modelList[m];
        bool ok         = md.vertices && md.indices && md.vertCount && md.indexCount && (md.faceVertCount == 3 || md.faceVertCount == 4);
        const unsigned n = (unsigned)md.vertCount * (md.frameCount ? md.frameCount : 1);
        for (unsigned v = 0; ok && v < n; ++v) {
            const ModelVertex &mv = md.vertices[v];
            ok = mv.x >= -32768 && mv.x < 32768 && mv.y >= -32768 && mv.y < 32768 && mv.z >= -32768 && mv.z < 32768 && mv.nx >= -131072
                 && mv.nx < 131072 && mv.ny >= -131072 && mv.ny < 131072 && mv.nz >= -131072 && mv.nz < 131072;
        }
        sFits[m] = ok ? 1 : 2;
    }
    return sFits[m] == 1;
}

} // namespace

void GpuScene3D::BeginFrame() { sPoolCount = 0; }

void GpuScene3D::OnPrepare(uint16 scene)
{
    unsigned w = 0;
    for (unsigned r = 0; r < sPoolCount; ++r) {
        if (sPoolScene[r] == scene)
            continue;
        if (w != r) {
            sPool[w]      = sPool[r];
            sPoolScene[w] = sPoolScene[r];
        }
        ++w;
    }
    sPoolCount = w;
}

bool GpuScene3D::AddModel(uint16 model, uint16 scene, uint8 drawMode, Matrix *matWorld, Matrix *matNormals, uint32 color, bool animated, int32 frameA,
                          int32 frameB, int32 timer)
{
    if (!GpuFrame::active || !DEV_SW_GPURENDER || !DEV_SW_GPU3D || model >= MODEL_COUNT || scene >= SCENE3D_COUNT || !matWorld || sPoolCount >= POOL)
        return false;
    if (drawMode != S3D_SOLIDCOLOR_SCREEN && drawMode != S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN)
        return false;
    const Model &md = modelList[model];
    const bool shaded = drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN;
    if (shaded && (!(md.flags & 1) || !matNormals)) // 1: MODEL_USENORMALS (Scene3D.cpp)
        return false; // the software lights with normals left from an earlier model: not reproduced
    if (scene3DList[scene].vertexCount || !ModelFits(model))
        return false; // the scene already holds CPU faces: keep it all on the CPU (one sort)

    GpuScene3D::Inst &in = sPool[sPoolCount];
    sPoolScene[sPoolCount++] = scene;
    in.model    = (int16)model;
    in.frameA   = (int16)frameA;
    in.frameB   = (int16)frameB;
    in.timer    = (int16)timer;
    in.color    = color;
    in.animated = animated;
    in.hasNormals = matNormals != NULL;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            in.world[r * 4 + c] = matWorld->values[r][c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            in.normal[r * 3 + c] = matNormals ? matNormals->values[r][c] : 0;
    scene3DList[scene].drawMode = drawMode;
    return true;
}

void GpuScene3D::OnDraw(uint16 scene)
{
    if (!GpuFrame::active || scene >= SCENE3D_COUNT)
        return;
    unsigned n = 0;
    for (unsigned i = 0; i < sPoolCount; ++i)
        n += sPoolScene[i] == scene;
    if (!n)
        return;
    uint8 *at = GpuFrame::RecScene3D(sizeof(Head) + n * sizeof(Inst));
    if (!at)
        return; // the buffers are full: the frame went to the software (hazard), which has no instances to draw
    const Scene3D &scn = scene3DList[scene];
    Head *h            = (Head *)at;
    h->count           = (int32)n;
    h->scene           = scene;
    h->drawMode        = scn.drawMode;
    h->projX = scn.projectionX, h->projY = scn.projectionY;
    h->diffuse[0] = scn.diffuseX, h->diffuse[1] = scn.diffuseY, h->diffuse[2] = scn.diffuseZ;
    h->diffuseIntensity[0] = scn.diffuseIntensityX, h->diffuseIntensity[1] = scn.diffuseIntensityY, h->diffuseIntensity[2] = scn.diffuseIntensityZ;
    h->specularIntensity[0] = scn.specularIntensityX, h->specularIntensity[1] = scn.specularIntensityY, h->specularIntensity[2] = scn.specularIntensityZ;
    h->centerX = currentScreen->center.x, h->centerY = currentScreen->center.y;
    h->alpha = sceneInfo.entity ? sceneInfo.entity->alpha : 0xFF;
    h->ink   = sceneInfo.entity ? sceneInfo.entity->inkEffect : INK_NONE;
    Inst *out = (Inst *)(h + 1);
    for (unsigned i = 0; i < sPoolCount; ++i)
        if (sPoolScene[i] == scene)
            *out++ = sPool[i];
}

void GpuScene3D::Replay(const uint8 *arena)
{
    const Head *h   = (const Head *)arena;
    const Inst *ins = (const Inst *)(h + 1);
    if (h->scene < 0 || h->scene >= SCENE3D_COUNT)
        return;
    Scene3D *scn        = &scene3DList[h->scene];
    const uint16 keepV  = scn->vertexCount, keepF = scn->faceCount;
    scn->vertexCount    = 0;
    scn->faceCount      = 0;
    for (int32 i = 0; i < h->count; ++i) {
        const Inst &in = ins[i];
        Matrix w, nm;
        memset(&w, 0, sizeof(w));
        memset(&nm, 0, sizeof(nm));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                w.values[r][c] = in.world[r * 4 + c];
        w.values[3][3] = 0x100;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                nm.values[r][c] = in.normal[r * 3 + c];
        nm.values[3][3] = 0x100;
        if (in.animated) {
            // AddMeshFrameToScene takes the next frame from frameID + 1, or loopIndex past frameCount.
            Animator a;
            memset(&a, 0, sizeof(a));
            a.frameID    = in.frameA;
            a.frameCount = (int16)(in.frameA + 1);
            a.loopIndex  = (uint8)in.frameB;
            a.timer      = in.timer;
            AddMeshFrameToScene(in.model, h->scene, &a, h->drawMode, &w, in.hasNormals ? &nm : NULL, in.color);
        }
        else
            AddModelToScene(in.model, h->scene, h->drawMode, &w, in.hasNormals ? &nm : NULL, in.color);
    }
    // Draw3DScene reads the drawing entity's alpha and ink.
    static Entity stand;
    Entity *const keepEntity = sceneInfo.entity;
    stand.alpha              = h->alpha;
    stand.inkEffect          = (uint8)h->ink;
    sceneInfo.entity         = &stand;
    Draw3DScene((uint16)h->scene);
    sceneInfo.entity = keepEntity;
    scn->vertexCount = keepV;
    scn->faceCount   = keepF;
}

#endif
