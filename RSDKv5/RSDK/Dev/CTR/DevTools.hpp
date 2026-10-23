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

#include "RSDK/Dev/CTR/Port.hpp"

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
    STAGE_PCOPY,   // GPU presentation: copy of the picture into the staging buffer
    STAGE_PXFER,   // ... display transfer into the texture
    STAGE_PFRAME,  // ... citro3d frame (wait for the previous one, draw, queue)
    STAGE_WAIT,    // idle time the pacing waits for the next tick: the headroom of the frame
    STAGE_COUNT
};

// After RenderDevice::Init(): starts the FTP server and opens the perf CSV.
void Init();
// Once per main-loop iteration that runs a frame. Returns true when the engine must quit
// (a new build was installed and the countdown ended; the app relaunches itself).
bool FrameEnd();
void Shutdown();

void StageAdd(Stage stage, uint64 ticks);

struct Stats {
    float fps, frameMs;
    float stageMs[STAGE_COUNT];
    unsigned skipped; // passes without drawing in the last second
    int scene;
};
void GetStats(Stats *out);
const char *FtpStatus(); // "192.168.1.141:5000" or why the server is off
void Mark();             // writes a mark into log.txt and the perf CSV
// The switches survive between sessions, in the port's file with the player's options (Port::Save,
// port-settings.ini), written on every change and read at start. DRAW LISTS is not saved: off it leaves a blank
// picture, which is a poor thing to wake up to. SaveSwitches writes them into that file (a FILE*).
void SaveSettings();
void SaveSwitches(void *file);
// Draw order of the last frame, run-length coded ("L2 S37 L1 S5 R1"): L tile layer, S sprite, R rotozoom/deform,
// F face, 3 3D scene, P shape. Fed by the stage scopes; SITE ORDER prints it.
void OrderBegin();
void OrderNote(char kind);
// The renderer mode, the one switch the owner flips: GPU turns on GPU presentation, recording and the GPU
// executor together (every frame the GPU can do is drawn by it, the rest by the software renderer); CPU turns
// all three off (the software renderer and the CPU copy to the screen, as the port started). Switching resets
// the frame counters below. SITE SET gpu/record/gpurender still flip the parts one by one.
void SetGpuMode(bool on);
bool GpuMode();
// Once per frame shown: NULL when the GPU drew it, else why the CPU did (a static name, see GpuFrame::LastWhy).
void NoteFrame(const char *why);
struct RenderStats {
    bool gpuMode;
    bool lastFrameGpu;
    unsigned secGpu, secCpu;     // frames shown in the last full second
    unsigned totalGpu, totalCpu; // since the mode was last switched
    const char *lastWhy;         // why the last CPU frame in GPU mode was one (NULL: none yet)
    struct { const char *why; unsigned count; } top[3]; // most frequent reasons since the switch
};
void GetRenderStats(RenderStats *out);
// While a CIA arrives or installs: 0 when nothing is going on, else the phase (1 receiving, 2 installing,
// 3 restart countdown), the title to show, how far along it is (0..1, or -1 when unknown) and, in phase 3,
// the seconds left.
int InstallNotice(char *title, unsigned cap, float *progress, unsigned *secondsLeft);

// Switches the bottom-screen menu flips at run time (in memory, on at every boot).
extern int swMaxSkip;    // most passes skipped in a row
extern bool swDraw;      // run the draw lists (off isolates the cost of the logic)
extern bool swFlipCopy;  // copy the frame into the screen (off isolates the cost of the flip)
extern uint32 injectKeys; // 3DS key mask held on top of the real buttons (SITE INPUT)
// Recorded runs (SITE RUNREC / RUNSTOP / RUNPLAY): the 3DS keys of every logic pass from a scene's load, played back
// from the same load to compare builds on one route. Called by the input device with the real keys; during playback
// it replaces them.
void RunKeys(uint32 *held, uint32 *down);
// The CAPTURE button (DEBUG tab): freeze and save what is on screen (debug/capture-NNN-*, tools/capture.sh); tapped
// again, carry on. CaptureNumber: the last capture's number (-1: none yet).
void TapCapture();
int CaptureNumber();
extern bool freeze;       // skip the objects' update, keep drawing (SITE FREEZE): the same frame in both renderers
extern bool swGpuPresent;  // present through citro3d instead of the CPU rotate-copy (part of the GPU mode)
extern bool swGpuRender;   // execute recorded frames on the GPU (needs swRecord and swGpuPresent)
extern bool swRecord;      // record draw calls per frame and execute them at the flip (GpuFrame)
extern bool swGpu3D;       // 3D scenes as GPU model instances (GpuScene3D); off: their faces as before. On by default, saved.
extern bool swBottomUi;    // redraw the bottom screen (off to measure its cost)
extern bool swSyncDraw;    // C3D_FrameBegin waits for the previous GPU frame (experiment switch)
extern bool swVblankWait;  // gspWaitForVBlank after the present (experiment switch)
extern unsigned skipCount; // passes whose drawing was skipped since the last second

// The player's options of the bottom screen (frame skip, lock 30, fps, hud, tab) are in Port.hpp.
extern bool swForce3D;     // DEBUG FORCE 3D: draw both eyes as if the slider were up (cost on an Old 2DS); saved
// True when entities of this (stage) class must not be drawn: SITE HIDE, then Port::HideHud.
bool HideEntityClass(uint16 classID);

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
#define DEV_NOTE_FRAME(why) RSDK::DevTools::NoteFrame(why)

#else

#define DEV_STAGE(stage)
#define DEV_LOG(...)
#define DEV_NOTE_FRAME(why) ((void)0)

#endif

#if RETRO_PLATFORM == RETRO_3DS && MANIA_DEV_TOOLS
#define DEV_SW_FRAMESKIP RSDK::Port::frameSkip
#define DEV_SW_MAXSKIP   RSDK::DevTools::swMaxSkip
#define DEV_SW_DRAW      RSDK::DevTools::swDraw
#define DEV_SW_FLIPCOPY  RSDK::DevTools::swFlipCopy
#define DEV_SW_GPUPRESENT RSDK::DevTools::swGpuPresent
#define DEV_SW_RECORD    RSDK::DevTools::swRecord
#define DEV_SW_GPURENDER RSDK::DevTools::swGpuRender
#define DEV_SW_GPU3D     RSDK::DevTools::swGpu3D
#define DEV_SW_SYNCDRAW  RSDK::DevTools::swSyncDraw
#define DEV_SW_VBLANK    RSDK::DevTools::swVblankWait
#define DEV_SW_LOCK30    RSDK::Port::lock30
#define DEV_SW_FORCE3D   RSDK::DevTools::swForce3D
#define DEV_HIDE_CLASS(id) RSDK::DevTools::HideEntityClass(id)
#define DEV_COUNT_SKIP() (++RSDK::DevTools::skipCount)
#elif RETRO_PLATFORM == RETRO_3DS
// Production: the dev tools' defaults, the renderer and the frame options from the player's settings (Port).
#define DEV_SW_FRAMESKIP RSDK::Port::frameSkip
#define DEV_SW_MAXSKIP   2
#define DEV_SW_DRAW      true
#define DEV_SW_FLIPCOPY  true
#define DEV_SW_GPUPRESENT RSDK::Port::gpu
#define DEV_SW_RECORD    RSDK::Port::gpu
#define DEV_SW_GPURENDER RSDK::Port::gpu
#define DEV_SW_GPU3D     true
#define DEV_SW_SYNCDRAW  false
#define DEV_SW_VBLANK    false
#define DEV_SW_LOCK30    RSDK::Port::lock30
#define DEV_SW_FORCE3D   false
#define DEV_HIDE_CLASS(id) RSDK::Port::HideHud(id)
#define DEV_COUNT_SKIP() ((void)0)
#else
#define DEV_SW_FRAMESKIP true
#define DEV_SW_MAXSKIP   2
#define DEV_SW_DRAW      true
#define DEV_SW_FLIPCOPY  true
#define DEV_SW_GPUPRESENT false
#define DEV_SW_RECORD    false
#define DEV_SW_GPURENDER false
#define DEV_SW_GPU3D     false
#define DEV_SW_SYNCDRAW  true
#define DEV_SW_VBLANK    true
#define DEV_SW_LOCK30    false
#define DEV_SW_FORCE3D   false
#define DEV_HIDE_CLASS(id) false
#define DEV_COUNT_SKIP() ((void)0)
#endif

#endif
