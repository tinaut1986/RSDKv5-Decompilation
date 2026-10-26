#ifndef C3D_PRESENT_CTR_HPP
#define C3D_PRESENT_CTR_HPP

// GPU presentation of the top screen: the software renderer still draws the 400x240 RGB565 picture
// into screens[0].frameBuffer, and instead of rotating it into the screen's framebuffer on the CPU
// (~6 ms on an Old 3DS) the picture is copied to VRAM, tiled into a texture by the display-transfer
// engine and drawn as one quad by the PICA200. This also brings citro3d up, which the graphical
// bottom screen and the stereoscopic 3D need.

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace C3DPresent
{

// Brings citro3d, the top render target, the texture and the shader up. False leaves everything as it
// was (the CPU path keeps working).
// citro3d and citro2d, shared by the top-screen presentation and the bottom-screen UI.
bool InitCore();
bool CoreReady();
// At exit, before gfxExit: waits for the GPU's queued work and shuts citro2d and citro3d down. A process that
// ended with GPU work queued is a suspect for the loader faults on the relaunch after SITE INSTALL.
void ShutdownCore();

bool Init();
bool Ready();

// For the GPU renderer, which draws into the same top-screen target.
struct C3D_RenderTarget_tag;
void *Target(); // C3D_RenderTarget*
void *Projection(); // C3D_Mtx*
// The right eye's target (created on first use; NULL if it cannot be) and the top screen's 3D mode: on while both
// eyes are drawn (the 3D slider, or DEBUG FORCE 3D), off otherwise.
void *TargetRight();
void SetStereo(bool on);
bool Stereo();
// The 512x256 linear staging buffer (16-bit texels): free for the GPU renderer, which does not present through it.
void *Staging();
// Waits until the copies already sent from the staging buffer are done: call it before the CPU writes the buffer again,
// outside a citro3d frame. C3D_SyncTextureCopy / C3D_SyncDisplayTransfer end in gspWaitForEvent(PPF, false), which
// returns at once when an earlier PPF (the last frame's display transfer) was never waited for: the copy was still
// reading the buffer while the next chunk was written into it (a model's triangles got its colours, #10's floor lines).
void StagingWait();

// Draws `src` (400x240 RGB565 with `pitch` pixels per row) on the top screen and presents it.
void Present(const unsigned short *src, int pitch);

} // namespace C3DPresent
} // namespace RSDK

#endif

#endif
