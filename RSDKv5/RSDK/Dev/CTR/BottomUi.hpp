#ifndef BOTTOM_UI_CTR_HPP
#define BOTTOM_UI_CTR_HPP

// Graphical bottom screen, drawn with citro2d, in every 3DS build: tabs GAME (the run), OPTIONS, UPDATES and, with the
// dev tools, DEBUG (live frame timings and big touch buttons for the switches; the text-console menu stays only as
// the fallback when citro3d cannot start); the loading box; the update prompt at boot.

#include "RSDK/Dev/CTR/DevTools.hpp"

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace BottomUi
{

void Init();
bool Ready();

// The loading box: while the game reads from the SD card (boot, a stage load) the main loop stands still, so the
// loaders report here and the bottom screen is redrawn from inside them (at most every 50 ms): what is being loaded,
// the step, the file and a bar. Begin/End bracket a load; a phase is a share [lo, hi) of the bar that Item fills
// (i of n); Detail only changes the file shown.
void LoadBegin(const char *title);
void LoadPhase(const char *step, float lo, float hi);
void LoadItem(int i, int n);
void LoadDetail(const char *file);
void LoadEnd();

// Once per frame: touch, and a redraw when something changed or a quarter of a second has passed.
void Update();
// Full redraws since boot and the time they took (ms), for SITE TIMES.
void DrawStats(unsigned *count, float *ms);
// A question over the whole bottom screen, before the game runs (the save import): a title, lines of text and one or
// two buttons (A / B or a touch), in its own loop. 1: the first button, 0: the second, -1: the app is closing.
int Ask(const char *title, const char *const *lines, int lineCount, const char *yes, const char *no);
// After the engine's init: loads the game's HUD, ring and item box sprites for the whole game and copies them into
// the bottom screen's art, so the GAME tab draws the game's labels and icons from the start.
void LoadArtAtBoot();
// The dev tools' SITE TOUCH: one tap at (x, y) on the next Update, as if the screen had been touched there.
void Touch(int x, int y);
// The FPS counter of OPTIONS > SHOW FPS on the top screen: drawn with citro2d into the top target at the end of a
// GPU frame, or into the software picture (RGB565) before it is presented.
void DrawTopOverlayGpu(void *target); // C3D_RenderTarget*
void DrawTopOverlaySoft(unsigned short *fb, int pitch);
// At the end of the top screen's citro3d frame (GPU and CPU presentation): the GAME tab's TIME box, redrawn into the
// bottom target when the time changed, so the hundredths run at the game's rate between the full redraws.
void DrawInTopFrame();

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
inline void DrawInTopFrame() {}
inline void LoadBegin(const char *) {}
inline void LoadPhase(const char *, float, float) {}
inline void LoadItem(int, int) {}
inline void LoadDetail(const char *) {}
inline void LoadEnd() {}
inline void LoadArtAtBoot() {}
} // namespace BottomUi
} // namespace RSDK

#endif

#endif
