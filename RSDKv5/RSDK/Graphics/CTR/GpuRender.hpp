#ifndef GPU_RENDER_CTR_HPP
#define GPU_RENDER_CTR_HPP

// Turns a recorded command stream (GpuFrame) into PICA200 work: tile layers and sprites become textured quads
// sampled from two atlases made on demand from the 8-bit tileset and sprite sheets with the current palette,
// rectangles become coloured quads. A frame holding anything it cannot do (yet) is refused and replayed by the
// software renderer instead.

#include "RSDK/Graphics/CTR/GpuFrameCmd.hpp"

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace GpuRender
{

enum Reason {
    REASON_OK,
    REASON_NO_GPU,       // citro3d or the renderer could not start
    REASON_UNSUPPORTED,  // a command the GPU path does not do (the type is in LastUnsupported)
    REASON_INK,          // an ink effect not done yet
    REASON_PALETTE_LINES, // screen lines using different palette banks (the water line)
    REASON_TILE_RANGE,   // tile index outside the atlas
    REASON_ATLAS_FULL,
    REASON_QUAD_LIMIT,
    REASON_SPRITE_SIZE,
    REASON_BAD_DATA,
};

// True when the frame was drawn on the GPU and presented; false leaves everything untouched (nothing drawn).
bool Render(const GpuFrame::Stream &stream);

struct Stats {
    unsigned quads, tileConversions, spriteConversions, atlasFlushes;
    unsigned layerCmds, tileQuads, emptyTiles, rowsOut; // diagnostics
    unsigned reason, unsupportedCmd;
    unsigned atlasRetries; // frames rebuilt after emptying a full sprite atlas
    unsigned quadRetries;  // frames rebuilt with rotozoom layers in software after running out of quads
    unsigned softLayers;   // layers drawn by the software into a texture (last frame)
    unsigned faces;        // polygons emitted (last frame)
    unsigned rotoSoftRuns, rotoSoftQuads; // rotozoom layers sent to the software: too many runs / quads (total)
    unsigned rotoUs, rotoQuads; // rotozoom layers as tile runs: build time (us) and quads (last frame)
    unsigned eyesDropped;   // right eyes not drawn because the command buffer would not hold them (total)
    unsigned cmdBufPct;     // most of the GPU command buffer a frame has used (%), since boot
    unsigned tileEvictions; // times the full tile atlas gave back the cells not used in the frame (total)
};
const Stats &GetStats();
// The last refusal as a short name for the dev tools ("FACE", "INK TINT", "WATER LINE"); NULL when the last
// frame was drawn. Always a static string: callers may keep and compare the pointer.
const char *ReasonName();
// The last refused command with its parameters ("#12 type 7 kind 0 clip ... | p0 p1 ..."), for SITE STATE.
const char *LastRefusal();
// The depth set-up of the 3D scenes, to find the PICA's convention on the console: bit 0 nearer = lower z/w, bit 1
// GPU_GREATER instead of GPU_LESS, bit 2 clear the depth buffer to 0 instead of the maximum. SITE SET depthmode N.
extern unsigned depthMode;
// Stereo depth scale: pixels of disparity at the full slider for the farthest layers (SITE SET stereofar N, saved).
extern int stereoFar;
// DEPTH TINT (debug, not saved): every 2D batch painted one flat blue by its stereo depth, slider down too.
extern bool depthTint;
// The last 3D frame's depths ("L<layer> g<group> near..far | ... far N playfield #i sprites behind n"), SITE DEPTHS.
const char *DepthSummary();

} // namespace GpuRender
} // namespace RSDK

#endif

#endif
