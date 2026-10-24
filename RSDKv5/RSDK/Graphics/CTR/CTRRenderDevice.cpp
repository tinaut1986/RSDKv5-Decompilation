#define SCREEN_XSIZE_3DS (400)
#define SCREEN_YSIZE_3DS (240)

#define SCREEN_REFRESH (59.94f)
#include "RSDK/Dev/CTR/BottomUi.hpp"

using namespace RSDK;



bool RenderDevice::Init()
{
  gfxInitDefault();
  consoleInit(GFX_BOTTOM, NULL);
  // The console draws into one fixed buffer: with the bottom screen double-buffered every swap shows the
  // other (empty) one, and in GPU presentation mode, where nothing swaps it, the text stayed half missing.
  gfxSetDoubleBuffering(GFX_BOTTOM, false);

  if (printToConsole) {
    // dummy out later maybe possibly
    printf("RSDKv5 3DS: RenderDevice init\n");
    printf("The programmer has a nap. Hold out!\nProgrammer!\n");
  }

  gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
  gfxSetDoubleBuffering(GFX_TOP, true);

  // One entry per line, and per column for vertical-scroll layers and their callbacks: as many as the screen is wide,
  // as the PC render devices allocate it (max of width and height). With 240 the tower of Stardust Speedway 2
  // (SSZ2Setup_Scanline_BGTower, columns past the centre) wrote over the next heap chunk and the game crashed.
  const int32 scanlineCount = SCREEN_XSIZE_3DS > SCREEN_YSIZE_3DS ? SCREEN_XSIZE_3DS : SCREEN_YSIZE_3DS;
  scanlines = (ScanlineInfo*) malloc(scanlineCount * sizeof(ScanlineInfo));
  if (!scanlines)
    return false;
  memset(scanlines, 0, scanlineCount * sizeof(ScanlineInfo));

  engine.inFocus = 1;
  videoSettings.windowState = WINDOWSTATE_ACTIVE;
  videoSettings.dimMax = 1.0;
  videoSettings.dimPercent = 1.0;

  RSDK::SetScreenSize(0, SCREEN_XSIZE_3DS, SCREEN_YSIZE_3DS);

  memset(screens[0].frameBuffer, 0, SCREEN_XSIZE_3DS * SCREEN_YSIZE_3DS * sizeof(uint16));

  InitInputDevices();
  if (!AudioDevice::Init())
    return false;

  return true;
}

void RenderDevice::CopyFrameBuffer()
{
  return;
}

// The top screen's framebuffer is column-major and upside down: dst[x * 240 + (239 - y)] = src[y * pitch + x].
// Done in 8x8 tiles so every source row is read in short runs from cache and every destination column
// is written as four 32-bit words instead of eight scattered halfwords (the plain double loop took
// ~9 ms on an Old 3DS, more than half of what a frame may cost).
static void TransposeToFramebuffer(const u16 *src, int pitch, u16 *dst)
{
  for (int yb = 0; yb < SCREEN_YSIZE_3DS; yb += 8) {
    for (int xb = 0; xb < SCREEN_XSIZE_3DS; xb += 8) {
      const u16 *s0 = src + yb * pitch + xb;
      for (int xx = 0; xx < 8; xx++) {
        u32 *d = (u32 *)(dst + (xb + xx) * SCREEN_YSIZE_3DS + (SCREEN_YSIZE_3DS - 8 - yb));
        const u16 *s = s0 + xx;
        d[0] = (u32)s[7 * pitch] | ((u32)s[6 * pitch] << 16);
        d[1] = (u32)s[5 * pitch] | ((u32)s[4 * pitch] << 16);
        d[2] = (u32)s[3 * pitch] | ((u32)s[2 * pitch] << 16);
        d[3] = (u32)s[1 * pitch] | ((u32)s[0] << 16);
      }
    }
  }
}

// https://github.com/JeffRuLz/Sonic-1-2-2013-Decompilation/blob/main/RSDKv4/3ds/3ds.cpp#L141
void RenderDevice::FlipScreen()
{
  const bool drawnByGpu = GpuFrame::Present(); // execute the frame's recorded draw commands
  const u16 *src = screens[0].frameBuffer;
  u16 *dst = (uint16*) gfxGetFramebuffer(GFX_TOP, GFX_LEFT, 0, 0);

  if (!CheckForFrameSkip()) {
    if (!drawnByGpu)
      BottomUi::DrawTopOverlaySoft(screens[0].frameBuffer, screens[0].pitch); // OPTIONS > SHOW FPS
    switch (videoSettings.screenCount) {
      default:
      case 0:
        // image/video buffer, break
        break;
      case 1:
        if (drawnByGpu) {
          // already on screen
        }
        else if (DEV_SW_GPUPRESENT && (C3DPresent::Ready() || C3DPresent::Init()))
          C3DPresent::Present(src, screens[0].pitch);
        else if (DEV_SW_FLIPCOPY)
          TransposeToFramebuffer(src, screens[0].pitch, dst);
        break;

      // the separate screens used for competition mode will likely 
      // never be supported by the 3DS port
      case 2:
      case 3:
      case 4:
        break;
    }

    if (!(drawnByGpu || (DEV_SW_GPUPRESENT && C3DPresent::Ready()))) {
      C3DPresent::SetStereo(false); // the CPU copy fills the left eye only
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
    DEV_NOTE_FRAME(drawnByGpu ? NULL : GpuFrame::LastWhy());
    if (DEV_SW_VBLANK)
      gspWaitForVBlank();
  }
}

void RenderDevice::Release(bool32 isRefresh)
{
  if (scanlines)
    free(scanlines);

  if (!isRefresh) {
    C3DPresent::ShutdownCore();
    DEV_LOG("exit: citro3d shut down");
    gfxExit();
    DEV_LOG("exit: gfx shut down");
  }
}

void RenderDevice::RefreshWindow()
{

}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8* imagePixels)
{
  // TODO: implement
  return;
}

// TODO: you may have to rewrite parts of the engine elsewhere to hack 
// 3ds-theoraplayer in, just leave like this for now
void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8* imagePixels)
{
  return;
}

void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8* imagePixels)
{
  return;
}

void RenderDevice::SetupVideoTexture_YUV424(int32 width, int32 height, uint8* imagePixels)
{
  return;
}

bool RenderDevice::ProcessEvents()
{
  return true;
}

// TODO: re-use the frame limiter @JeffRuLz implemented with the CD port
void RenderDevice::InitFPSCap()
{
  return;
}

bool RenderDevice::CheckFPSCap()
{
  return true;
}

// Frame pacing. The logic runs one tick per main-loop pass and a drawn frame ends in a vblank
// wait, so a pass that costs more than one refresh (16.7 ms) slows the whole game down. Instead the
// passes are scheduled against the real clock: when one starts more than a tick late, its drawing
// and screen flip are skipped (the logic is only a few ms), which lets the logic catch up and keeps
// the game at full speed while the picture drops to 30 or 20 fps. At most two passes in a row are
// skipped, and a long stall (loading, pause) resets the schedule instead of fast-forwarding.
static const u64 TICKS_PER_FRAME = 4481136ULL; // the 3DS refresh: 268111856 / 4481136 = 59.83 Hz
static u64 sNextTick;
static int sSkipRun;
static bool sSkipThisPass;

void RenderDevice::UpdateFPSCap()
{
  u64 now = svcGetSystemTick();
  if (sNextTick == 0 || now > sNextTick + 4 * TICKS_PER_FRAME) {
    sNextTick = now;
    sSkipRun = 0;
  }
  // Early: wait for this pass's moment. The clock, not a vblank wait after the flip, sets the rhythm: a
  // pass that skipped its drawing used to skip the wait too, which shifted every later pass against the
  // vblank and made the drawn ones wait two (the picture ran at half rate while the logic ran at 60).
  if (now + SYSCLOCK_ARM11 / 2000 < sNextTick) {
    const u64 start = now;
    // Sleep most of it, leave about half a millisecond to spin on.
    const u64 sleepTicks = sNextTick - now - SYSCLOCK_ARM11 / 2000;
    svcSleepThread((s64)(sleepTicks * 1000000000ULL / SYSCLOCK_ARM11));
    while (svcGetSystemTick() < sNextTick) {
    }
    now = svcGetSystemTick();
#if MANIA_DEV_TOOLS
    DevTools::StageAdd(DevTools::STAGE_WAIT, now - start);
#else
    (void)start;
#endif
  }
  const u64 late = now > sNextTick ? now - sNextTick : 0;
  static unsigned pass;
  ++pass;
  // LOCK 30: every other pass is not drawn (the logic keeps its 60 ticks); otherwise a late pass may be skipped.
  sSkipThisPass = DEV_SW_LOCK30 ? (pass & 1) != 0 : DEV_SW_FRAMESKIP && late > TICKS_PER_FRAME && sSkipRun < DEV_SW_MAXSKIP;
  sSkipRun = sSkipThisPass ? sSkipRun + 1 : 0;
  if (sSkipThisPass)
    DEV_COUNT_SKIP();
  sNextTick += TICKS_PER_FRAME;
}

bool RenderDevice::CheckForFrameSkip()
{
  return sSkipThisPass;
}

// NOTE: shaders likely won't ever be supported by the 3DS port; given
// that the Retro Engine's internal resolution matches that of the 3DS's 
// screen, they probably wouldn't look too great if implemented anyways
void RenderDevice::LoadShader(const char* fileName, bool32 linear)
{
  return;
}

bool RenderDevice::InitShaders()
{
  return true;
}

bool RenderDevice::SetupRendering()
{
  // is anything even really needed here?
  return true;
}

void RenderDevice::InitVertexBuffer()
{
  // TODO: is this needed, since the buffer is copied to the screen in SW?
  return;
}

bool RenderDevice::InitGraphicsAPI()
{
  // TODO: implement, if needed 
  return true;
}

void RenderDevice::GetDisplays()
{
  // TODO: implement, if needed
  return;
}

void RenderDevice::GetWindowSize(int32* width, int32* height) {
  if (width)
    *width = 400;

  if (height)
    *height = 240;
}
