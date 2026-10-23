#ifndef GPU_FRAME_CMD_CTR_HPP
#define GPU_FRAME_CMD_CTR_HPP

// The recorded command stream of a frame (GpuFrame.cpp records it, GpuRender.cpp turns it into GPU work).

#include "RSDK/Core/RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace GpuFrame
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
    // Runs of faces with the same ink, alpha, clip and screen position (the 3D scenes draw thousands a frame): p[0]
    // faces, p[1] alpha, p[2] ink; the arena holds per face int32 n, int32 0xRRGGBB (CMD_FACES only), Vector2 v[n],
    // and for CMD_BLENDED_FACES uint32 colours[n].
    CMD_FACES,
    CMD_BLENDED_FACES,
    CMD_SCENE3D, // a 3D scene's model instances (GpuScene3D::Head + Inst in the arena)
    CMD_PALETTE, // fullPalette changed while recording (a boss flashing, Super Sonic): the new one in the arena
    CMD_TYPES_MAX
};

struct Cmd {
    uint8 type;
    uint8 kind;    // layer type
    uint8 group;   // sceneInfo.currentDrawGroup when recorded (stereoscopic depth)
    int16 clip[4]; // clipBound_X1, Y1, X2, Y2 when the call was made
    int32 pos[2];  // currentScreen->position
    int32 p[15];   // parameters, per type
    uint32 arena;  // offset of the variable-size data (vertices, colours, scanlines, line buffer)
    uint32 arenaSize;
};

struct Stream {
    const Cmd *cmds;
    unsigned count;
    const uint8 *arena;
    const uint16 (*basePalette)[256]; // the palette the frame started with when it changed while recording, else NULL
};

// Bumped by every palette change / tile change / scene load: textures made from older data are stale.
extern unsigned paletteVersion;
extern unsigned contentVersion;

} // namespace GpuFrame
} // namespace RSDK

#endif

#endif
