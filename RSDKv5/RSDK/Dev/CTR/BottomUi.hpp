#ifndef BOTTOM_UI_CTR_HPP
#define BOTTOM_UI_CTR_HPP

// Graphical bottom screen (dev tools): live frame timings, a stacked update/draw/flip bar and big touch
// buttons for the switches, drawn with citro2d. Replaces the text-console menu, which stays only as the
// fallback when citro3d cannot start.

#include "RSDK/Dev/CTR/DevTools.hpp"

#if RETRO_PLATFORM == RETRO_3DS && MANIA_DEV_TOOLS

namespace RSDK
{
namespace BottomUi
{

void Init();
bool Ready();
// Once per frame: touch, and a redraw when something changed or a quarter of a second has passed.
void Update();
// The FPS counter of OPTIONS > SHOW FPS on the top screen: drawn with citro2d into the top target at the end of a
// GPU frame, or into the software picture (RGB565) before it is presented.
void DrawTopOverlayGpu(void *target); // C3D_RenderTarget*
void DrawTopOverlaySoft(unsigned short *fb, int pitch);

// The loading box: while the game reads from the SD card (boot, a stage load) the main loop stands still, so the
// loaders report here and the bottom screen is redrawn from inside them (at most every 50 ms): what is being loaded,
// the step, the file and a bar. Begin/End bracket a load; a phase is a share [lo, hi) of the bar that Item fills
// (i of n); Detail only changes the file shown.
void LoadBegin(const char *title);
void LoadPhase(const char *step, float lo, float hi);
void LoadItem(int i, int n);
void LoadDetail(const char *file);
void LoadEnd();

} // namespace BottomUi
} // namespace RSDK

#else

namespace RSDK
{
namespace BottomUi
{
inline void Init() {}
inline bool Ready() { return false; }
inline void Update() {}
inline void DrawTopOverlayGpu(void *) {}
inline void DrawTopOverlaySoft(unsigned short *, int) {}
inline void LoadBegin(const char *) {}
inline void LoadPhase(const char *, float, float) {}
inline void LoadItem(int, int) {}
inline void LoadDetail(const char *) {}
inline void LoadEnd() {}
} // namespace BottomUi
} // namespace RSDK

#endif

#endif
