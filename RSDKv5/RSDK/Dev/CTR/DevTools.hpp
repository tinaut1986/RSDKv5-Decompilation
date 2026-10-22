#ifndef DEV_TOOLS_CTR_HPP
#define DEV_TOOLS_CTR_HPP

// Developer tools for the 3DS port (compiled in unless MANIA_DEV_TOOLS=0):
//  - an FTP server on port 5000 over the whole SD card, with SITE INSTALL <cia> to replace the
//    running build and relaunch into it (same protocol as the owner's other ports),
//  - per-stage frame timing (update / draw / flip) written once a second to
//    sdmc:/3ds/Sonic Mania 3DS/debug/perf-NNNN.csv and shown on the bottom screen.

#ifndef MANIA_DEV_TOOLS
#define MANIA_DEV_TOOLS 1
#endif

#if RETRO_PLATFORM == RETRO_3DS && MANIA_DEV_TOOLS

namespace RSDK
{
namespace DevTools
{

// UPDATE, DRAW and FLIP split a frame; the rest split DRAW by what is drawn (they do not nest).
enum Stage {
    STAGE_UPDATE,
    STAGE_DRAW,
    STAGE_FLIP,
    STAGE_LAYERS,  // tile layers
    STAGE_SPRITES, // DrawSpriteFlipped (all ordinary sprites)
    STAGE_ROTO,    // rotozoom + deformed sprites
    STAGE_FACES,   // DrawFace / DrawBlendedFace
    STAGE_3D,      // Draw3DScene
    STAGE_SHAPES,  // lines, rectangles, circles, single tiles
    STAGE_COUNT
};

// After RenderDevice::Init(): starts the FTP server and opens the perf CSV.
void Init();
// Once per main-loop iteration that runs a frame. Returns true when the engine must quit
// (a new build was installed and the countdown ended; the app relaunches itself).
bool FrameEnd();
void Shutdown();

void StageAdd(Stage stage, uint64 ticks);

// Switches the bottom-screen menu flips at run time (in memory, on at every boot).
extern bool swFrameSkip; // skip drawing + flip when the logic is more than a tick late
extern int swMaxSkip;    // most passes skipped in a row
extern bool swDraw;      // run the draw lists (off isolates the cost of the logic)
extern bool swFlipCopy;  // copy the frame into the screen (off isolates the cost of the flip)
extern uint32 injectKeys; // 3DS key mask held on top of the real buttons (SITE INPUT)
extern bool swGpuPresent;  // present through citro3d instead of the CPU rotate-copy
extern unsigned skipCount; // passes whose drawing was skipped since the last second

// Appends one line to sdmc:/3ds/Sonic Mania 3DS/debug/log.txt (flushed at once, so it survives a
// crash or an exit to the HOME menu); also valid before Init().
void Log(const char *fmt, ...);

struct StageScope {
    Stage stage;
    uint64 start;
    explicit StageScope(Stage s);
    ~StageScope();
};

} // namespace DevTools
} // namespace RSDK

#define DEV_STAGE(stage) RSDK::DevTools::StageScope devStageScope(RSDK::DevTools::stage)
#define DEV_LOG(...)     RSDK::DevTools::Log(__VA_ARGS__)

#else

#define DEV_STAGE(stage)
#define DEV_LOG(...)

#endif

#if RETRO_PLATFORM == RETRO_3DS && MANIA_DEV_TOOLS
#define DEV_SW_FRAMESKIP RSDK::DevTools::swFrameSkip
#define DEV_SW_MAXSKIP   RSDK::DevTools::swMaxSkip
#define DEV_SW_DRAW      RSDK::DevTools::swDraw
#define DEV_SW_FLIPCOPY  RSDK::DevTools::swFlipCopy
#define DEV_SW_GPUPRESENT RSDK::DevTools::swGpuPresent
#define DEV_COUNT_SKIP() (++RSDK::DevTools::skipCount)
#else
#define DEV_SW_FRAMESKIP true
#define DEV_SW_MAXSKIP   2
#define DEV_SW_DRAW      true
#define DEV_SW_FLIPCOPY  true
#define DEV_SW_GPUPRESENT false
#define DEV_COUNT_SKIP() ((void)0)
#endif

#endif
