#ifndef PORT_CTR_HPP
#define PORT_CTR_HPP

// The player's side of the 3DS port, in every build (the dev tools add their switches on top): the options of the
// bottom screen (OPTIONS, UPDATES), saved in sdmc:/3ds/Sonic Mania 3DS/port-settings.ini (the game keeps its own
// Settings.ini there and the SD card ignores case), a frame counter, the TOP HUD option and the restart into a newly
// installed build.

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace Port
{

// The options. Read at boot (Load), written on every change (Save).
extern bool frameSkip;  // FRAMES AUTO: skip drawing a pass that is more than a tick late
extern bool lock30;     // FRAMES LOCK 30: draw every other pass (the logic stays at 60)
extern bool gpu;        // RENDERER GPU (production builds; the dev tools keep their own switches, see GpuMode)
extern int fps;         // SHOW FPS: 0 off, 1 top left, 2 top right, 3 bottom left, 4 bottom right
extern int hud;         // TOP HUD: 0 shown, 1 auto (hidden while the GAME tab is open), 2 hidden
extern int tab;         // the bottom screen's open tab (BottomUi)
extern bool autoUpdate; // UPDATES > AUTO UPDATE: look for a newer build at boot and offer it
extern bool betas;      // UPDATES > BETAS: the beta channel too (pre-releases)
extern int theme;       // OPTIONS > THEME: the bottom screen's colours (BottomUi's kThemes)
extern bool plus;       // OPTIONS > PLUS: Sonic Mania Plus content (Mighty, Ray, Encore); off by default, read at boot
extern bool plusAsked;  // the one-time question (OfferPlus) was answered

void Load(); // before the engine starts
void Save();

// Sonic Mania Plus. The decompilation's license wants every build given to anyone to start with it off; an option
// off by default is allowed. OfferPlus asks once, at boot, when the data has the Plus content; ApplyPlus hands the
// option to the engine (the DLC check) before the game starts, so a change counts from the next start.
void OfferPlus();
void ApplyPlus();
bool PlusApplied();
bool DataHasPlus();

// The renderer: GPU draws what it can, CPU is the original software renderer.
bool GpuMode();
void SetGpuMode(bool on);

// Frames shown in the last second.
float Fps();
// True when entities of this (stage) class must not be drawn: the game's HUD under TOP HUD hidden / auto.
bool HideHud(uint16 classID);

// After an update is installed: the game starts again into the new build 10 s later.
void RequestRelaunch();
// 0 when no restart is coming, else the seconds left; *progress 0..1 along the wait.
unsigned RelaunchSecondsLeft(float *progress);

// The network (sockets), shared by the updater and the dev tools' FTP server: the first user starts it (a 1 MB
// buffer); it then stays up until the process ends. False when it cannot start.
bool NetAcquire();
void NetRelease();
bool WifiConnected();

// Once per main-loop iteration: the frame counter, the bottom screen (production builds) and the restart. True when
// the engine must quit (the app starts again by itself).
bool FrameEnd();

} // namespace Port
} // namespace RSDK

#endif

#endif
