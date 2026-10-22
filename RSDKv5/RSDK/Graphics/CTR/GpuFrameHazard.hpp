#ifndef GPU_FRAME_HAZARD_CTR_HPP
#define GPU_FRAME_HAZARD_CTR_HPP

// The few declarations the palette code needs without pulling in the whole engine header (see GpuFrame.hpp).

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace GpuFrame
{
extern bool active;
extern bool pending;
extern unsigned paletteVersion;
void Hazard(const char *why);
} // namespace GpuFrame
} // namespace RSDK

#define GPUFRAME_PALETTE_HAZARD()                                                                                                                    \
    do {                                                                                                                                             \
        ++RSDK::GpuFrame::paletteVersion;                                                                                                            \
        if (RSDK::GpuFrame::active || RSDK::GpuFrame::pending)                                                                                       \
            RSDK::GpuFrame::Hazard("PALETTE");                                                                                                       \
    } while (0)

#else

#define GPUFRAME_PALETTE_HAZARD()

#endif

#endif
