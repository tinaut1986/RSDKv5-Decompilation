#ifndef GPU_SCENE3D_CTR_HPP
#define GPU_SCENE3D_CTR_HPP

// The engine's 3D scenes on the GPU. While a frame is recorded, AddModelToScene / AddMeshFrameToScene in the modes the
// GPU path does (S3D_SOLIDCOLOR_SCREEN, S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN) store an instance (model, frames, matrices,
// colour) instead of transforming every vertex on the CPU, and Draw3DScene records one CMD_SCENE3D with the scene's
// instances and lighting. GpuRender draws it with the models in VRAM and scene3d.v.pica doing the transform, projection
// and lighting, a depth buffer in place of the face sort. A frame replayed by the software runs the same engine calls
// with the recorded data (Replay).

#include "RSDK/Core/RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace GpuScene3D
{

// CMD_SCENE3D's arena: a Head, then Head::count Inst.
struct Head {
    int32 count, scene, drawMode;
    int32 projX, projY;
    int32 diffuse[3], diffuseIntensity[3], specularIntensity[3];
    int32 centerX, centerY;
    int32 posX, posY; // the screen's position (the orthographic modes draw relative to it)
    int32 alpha, ink;
};
struct Inst {
    int16 model, frameA, frameB, timer; // timer: the interpolation towards frame B, 0..255 (0 for a still model)
    uint32 color;
    int32 world[12];  // rows 0-2 of the world matrix
    int32 normal[9];  // rows 0-2, columns 0-2 of the normal matrix
    uint8 animated, hasNormals, pad[2];
};

void BeginFrame();
void OnPrepare(uint16 scene);
// True when the model was taken as an instance (the caller returns without adding faces).
bool AddModel(uint16 model, uint16 scene, uint8 drawMode, Matrix *matWorld, Matrix *matNormals, uint32 color, bool animated, int32 frameA, int32 frameB,
              int32 timer);
void OnDraw(uint16 scene);
// Software replay of a CMD_SCENE3D (GpuFrame::Flush).
void Replay(const uint8 *arena);

} // namespace GpuScene3D
} // namespace RSDK

#endif

#endif
