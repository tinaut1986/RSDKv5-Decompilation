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
} // namespace BottomUi
} // namespace RSDK

#endif

#endif
