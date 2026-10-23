#ifndef GPU_FRAME_CTR_HPP
#define GPU_FRAME_CTR_HPP

// Record-and-execute drawing for the 3DS port.
//
// While the engine runs the draw lists of a frame (ProcessObjectDrawLists) every function that writes the
// framebuffer (sprites, tile layers, rectangles, faces...) is recorded as a command instead of drawing. At
// Present (RenderDevice::FlipScreen) the list is executed: by the GPU when every command in it is
// supported, otherwise replayed through the very same software functions, in order, so a frame the GPU
// cannot do looks exactly as it always did. Anything that would make a replay differ from the immediate
// result (a palette change, the 3D scene, text drawn by the dev menu) is a *hazard*: the commands recorded
// so far are replayed at once and the rest of the frame is drawn immediately.

#include "RSDK/Core/RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace GpuFrame
{

extern bool active;  // inside the draw lists: calls are recorded
extern bool pending; // commands waiting for Present

void Begin();
void End();
void Flush();   // replay the pending commands through the software renderer now
void Hazard(const char *why); // Flush, and draw immediately for the rest of the frame; `why` is a static name ("PALETTE")
// FlipScreen: execute what is pending. True when the GPU drew the frame and presented it (the caller must not
// present the software framebuffer), false when it was replayed into the software framebuffer.
bool Present();
// Why the frame Present just handled was not drawn by the GPU ("CPU MODE", "PALETTE", "FACE"...); NULL when it
// was. A static string.
const char *LastWhy();
// SITE CMDS: the next frame's command list is written to `path` before it is executed (one line per command).
extern volatile bool dumpRequest;
extern const char *dumpPath;
void InvalidateContent(); // a scene (tileset, sprite sheets) was loaded: textures made from the old ones are stale
// DrawAniTile rewrote tiles [first, first + count) of the tileset (all four flip variants): the GPU's copies are stale.
void TilesChanged(unsigned first, unsigned count);
extern uint32 tileDirty[TILE_COUNT / 32]; // bit per base tile, cleared by GpuRender when it drops its copies
extern bool tileDirtyAny;

// Recording entry points: true when the call was taken (the caller returns).
bool RecFill(uint32 color, int32 alphaR, int32 alphaG, int32 alphaB);
bool RecLine(int32 x1, int32 y1, int32 x2, int32 y2, uint32 color, int32 alpha, int32 ink, bool32 screenRelative);
bool RecRect(int32 x, int32 y, int32 w, int32 h, uint32 color, int32 alpha, int32 ink, bool32 screenRelative);
bool RecCircle(int32 x, int32 y, int32 radius, uint32 color, int32 alpha, int32 ink, bool32 screenRelative);
bool RecCircleOutline(int32 x, int32 y, int32 innerRadius, int32 outerRadius, uint32 color, int32 alpha, int32 ink, bool32 screenRelative);
bool RecFace(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 ink);
bool RecBlendedFace(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 ink);
bool RecSprite(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 direction, int32 ink, int32 alpha, int32 sheetID);
bool RecRoto(int32 x, int32 y, int32 pivotX, int32 pivotY, int32 width, int32 height, int32 sprX, int32 sprY, int32 scaleX, int32 scaleY,
             int32 direction, int16 rotation, int32 ink, int32 alpha, int32 sheetID);
bool RecLayer(TileLayer *layer, int kind);
bool RecDeformed(uint16 sheetID, int32 ink, int32 alpha);
// A CMD_SCENE3D with `bytes` of arena (GpuScene3D fills it); NULL when the buffers are full.
uint8 *RecScene3D(uint32 bytes);

// Statistics for the dev tools (commands of the last frame, how it was executed).
struct Stats {
    unsigned commands;
    unsigned replayed; // frames executed through the software renderer
    unsigned gpuFrames; // frames drawn by the GPU
    unsigned lastReason; // why the last frame fell back (GpuRender::Reason)
};
const Stats &GetStats();

} // namespace GpuFrame
} // namespace RSDK

// At the top of a framebuffer-writing function: record the call, or flush pending commands first when the
// call is an immediate one that must land after them. `rec` is the Rec* call.
#define GPUFRAME_REC(rec)                                                                                                                            \
    do {                                                                                                                                             \
        if (RSDK::GpuFrame::active) {                                                                                                                \
            if (RSDK::GpuFrame::rec)                                                                                                                 \
                return;                                                                                                                              \
        }                                                                                                                                            \
        else if (RSDK::GpuFrame::pending)                                                                                                            \
            RSDK::GpuFrame::Flush();                                                                                                                 \
    } while (0)

// For functions that cannot be deferred (palette changes, the 3D scene, dev text). `why` names it in the dev tools.
#define GPUFRAME_HAZARD(why)                                                                                                                         \
    do {                                                                                                                                             \
        if (RSDK::GpuFrame::active || RSDK::GpuFrame::pending)                                                                                       \
            RSDK::GpuFrame::Hazard(why);                                                                                                             \
    } while (0)

#else

#define GPUFRAME_REC(rec)
#define GPUFRAME_HAZARD(why)

#endif

#endif
