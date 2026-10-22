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

enum Stage { STAGE_UPDATE, STAGE_DRAW, STAGE_FLIP, STAGE_COUNT };

// After RenderDevice::Init(): starts the FTP server and opens the perf CSV.
void Init();
// Once per main-loop iteration that runs a frame. Returns true when the engine must quit
// (a new build was installed and the countdown ended; the app relaunches itself).
bool FrameEnd();
void Shutdown();

void StageAdd(Stage stage, uint64 ticks);

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

#endif
