#ifndef VIDEO_FILES_CTR_HPP
#define VIDEO_FILES_CTR_HPP

// OPTIONS > VIDEOS: how many of the game's videos have a copy converted for the 3DS in sdmc:/3ds/Sonic Mania 3DS/Video/
// (made on a PC by tools/convert-videos.py; Graphics/Video.cpp plays those instead of the pack's 1024x512 ones, which
// an Old 3DS shows only a few frames a second of). Converting on the console was tried: devkitPro's libtheora for the
// 3DS has no encoder (libtheoraenc.a is a stub), see docs/videos.md.

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace VideoFiles
{

int Count();     // the videos the game plays (the PC tool's list)
int Converted(); // of those, the ones with a converted copy (looked up on the SD card at most every 5 s)

} // namespace VideoFiles
} // namespace RSDK

#endif // RETRO_PLATFORM == RETRO_3DS

#endif // VIDEO_FILES_CTR_HPP
