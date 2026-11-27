#ifndef VIDEO_FRAME_CTR_HPP
#define VIDEO_FRAME_CTR_HPP

// The top screen's picture while the engine shows a video (Theora, YUV planes) or a full-screen image (1024x512
// ARGB, LoadImage): screenCount is 0 then and the renderer draws nothing. Pictures are 400x240 RGB565 in a few
// slots: the video's decoder thread (Graphics/Video.cpp) converts frames ahead into free slots and the main thread
// shows each at its time. A picture is scaled to fit (1024x512 -> 400x200, a 400x200 video 1:1), centred, black
// around it. A 4:2:0 frame at the screen's size is converted by the Y2R hardware (the camera's YUV -> RGB
// converter), anything else on the CPU. The slots are freed when the engine draws its own screens again.

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace VideoFrame
{

// 12 slots (2.3 MB): half a second ahead at 24 fps, through the intro's stretches of frames that cost more to decode
// than they last (with 4, the picture stopped and jumped 4 times in a row around 21-23 s and at 53 s). A slot the heap
// cannot give is left unused.
enum { W = 400, H = 240, SLOTS = 12 };

// The slot's picture, allocated on the first call (NULL without memory).
uint16 *Slot(int32 slot);
// At the start of a video, on the main thread: the slots, and Y2R opened (it stays open from then on). Returns the
// time it took (us).
uint32 Prepare();
bool HardwareReady();

// Where SetYUV converts (the engine's SetupVideoTexture_* calls reach it through the render device).
void SetTarget(int32 slot);
// One decoded frame into the target slot. chromaShiftX/Y: 1/1 for 4:2:0, 1/0 for 4:2:2, 0/0 for 4:4:4. On the CPU
// it is done on return; through Y2R the planes are copied and the conversion runs on: the slot is not ready (and
// must not be shown) until WaitConversion returns or ConversionDone says so.
void SetYUV(int32 width, int32 height, const uint8 *y, const uint8 *u, const uint8 *v, int32 strideY, int32 strideU, int32 strideV,
            int32 chromaShiftX, int32 chromaShiftY);
void WaitConversion();
bool ConversionDone();
// A still image, 32-bit ARGB (the PNG decoder's pixels), averaged down into slot 0 and shown.
void SetImage(int32 width, int32 height, const uint32 *argb);

// Puts a slot on the screen.
void Show(int32 slot);
int32 Shown(); // -1: none

// The picture (W x H RGB565, pitch W) at `dim` brightness (0..1, the image fades), or NULL before the first one.
const uint16 *Picture(float dim);
// Changes whenever the picture does (the screen is only presented again then).
uint32 Version();

// Frees the slots (called once the engine draws its own screens again); Y2R stays open.
void Release();

// Frames converted since the last Release, the time it took (ticks of svcGetSystemTick), how many by Y2R.
void ResetStats(); // at the start of a video
uint64 ConvertTicks();
uint32 Conversions();
uint32 HardwareConversions();

} // namespace VideoFrame
} // namespace RSDK

#endif // RETRO_PLATFORM == RETRO_3DS

#endif // VIDEO_FRAME_CTR_HPP
