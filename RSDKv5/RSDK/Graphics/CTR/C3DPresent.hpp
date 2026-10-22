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
bool Init();
bool Ready();

// Draws `src` (400x240 RGB565 with `pitch` pixels per row) on the top screen and presents it.
void Present(const unsigned short *src, int pitch);

} // namespace C3DPresent
} // namespace RSDK

#endif

#endif
