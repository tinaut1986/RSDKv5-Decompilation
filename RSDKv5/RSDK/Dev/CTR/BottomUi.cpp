#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/iosupport.h>

#include "RSDK/Graphics/CTR/C3DPresent.hpp"
#include "RSDK/Graphics/CTR/GpuRender.hpp"
#include "RSDK/Dev/CTR/GameInfo.h"
#include "RSDK/Dev/CTR/DevTools.hpp"
#include "RSDK/Dev/CTR/UiFont.hpp"
#include "RSDK/Dev/CTR/PortVersion.h"
#include "RSDK/Dev/CTR/Updater.hpp"
#include <time.h>

using namespace RSDK;

enum { W = 320, H = 240 };

static bool sReady;
static C3D_RenderTarget *sTarget;
static C2D_TextBuf sTextBuf;
static bool sDirty = true;

static inline u32 Rgb(int r, int g, int b) { return C2D_Color32(r, g, b, 255); }

// Draws `s` at x and returns the x after it (for a line in several colours).
static float TextRun(const char *s, float x, float y, float scale, u32 color)
{
    C2D_Text t;
    C2D_TextParse(&t, sTextBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return x + w;
}

// `wrapW`: the line width for C2D_WordWrap (without it citro2d wraps after every word).
static void Text(const char *s, float x, float y, float scale, u32 color, u32 flags = 0, float z = 0.5f, float wrapW = 0)
{
    C2D_Text t;
    C2D_TextParse(&t, sTextBuf, s);
    C2D_TextOptimize(&t);
    if (flags & C2D_WordWrap)
        C2D_DrawText(&t, C2D_WithColor | flags, x, y, z, scale, scale, color, wrapW);
    else
        C2D_DrawText(&t, C2D_WithColor | flags, x, y, z, scale, scale, color);
}

static u64 sLastDraw;
static int sPressed = -1;

// Saves the options the bottom screen changed (and the dev tools' switches with them).
static void SaveAll()
{
#if MANIA_DEV_TOOLS
    DevTools::SaveSettings();
#else
    Port::Save();
#endif
}

#if MANIA_DEV_TOOLS

// ---------------------------------------------------------------- dev tools: the DEBUG tab's buttons


struct Button {
    const char *label;
    void (*tap)();
    void (*value)(char *out, unsigned cap, bool *on);
};

static void ValFrameSkip(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", Port::frameSkip ? "ON" : "OFF"); *on = Port::frameSkip; }
static void TapFrameSkip() { Port::frameSkip = !Port::frameSkip; }
static void ValMaxSkip(char *o, unsigned c, bool *on) { snprintf(o, c, "%d", DevTools::swMaxSkip); *on = true; }
static void TapMaxSkip() { DevTools::swMaxSkip = DevTools::swMaxSkip % 4 + 1; }
static void ValDraw(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swDraw ? "ON" : "OFF"); *on = DevTools::swDraw; }
static void TapDraw() { DevTools::swDraw = !DevTools::swDraw; }
static void ValFlip(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swFlipCopy ? "ON" : "OFF"); *on = DevTools::swFlipCopy; }
static void TapFlip() { DevTools::swFlipCopy = !DevTools::swFlipCopy; }
static void ValRenderer(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::GpuMode() ? "GPU" : "CPU"); *on = DevTools::GpuMode(); }
static void TapRenderer() { DevTools::SetGpuMode(!DevTools::GpuMode()); }
static unsigned sMarks;
static void ValMark(char *o, unsigned c, bool *on) { snprintf(o, c, "%u", sMarks); *on = false; }
static void TapMark()
{
    ++sMarks;
    DevTools::Mark();
}

static void ValGpu3D(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swGpu3D ? "GPU" : "FACES"); *on = DevTools::swGpu3D; }
static void TapGpu3D() { DevTools::swGpu3D = !DevTools::swGpu3D; }
static void ValForce3D(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swForce3D ? "2 EYES" : "OFF"); *on = DevTools::swForce3D; }
static void TapForce3D()
{
    DevTools::swForce3D = !DevTools::swForce3D;
    DevTools::SaveSettings();
}
static void ValDepthTint(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", GpuRender::depthTint ? "ON" : "OFF"); *on = GpuRender::depthTint; }
static void TapDepthTint() { GpuRender::depthTint = !GpuRender::depthTint; }
static void ValCapture(char *o, unsigned c, bool *on)
{
    const int n = DevTools::CaptureNumber();
    if (DevTools::freeze && n >= 0)
        snprintf(o, c, "#%03d GO ON", n);
    else
        snprintf(o, c, "%s", "TAKE");
    *on = DevTools::freeze;
}
static void TapCapture() { DevTools::TapCapture(); }

static const Button kButtons[] = {
    { "RENDERER", TapRenderer, ValRenderer }, { "FRAME SKIP", TapFrameSkip, ValFrameSkip }, { "MAX SKIP", TapMaxSkip, ValMaxSkip },
    { "LOG MARK", TapMark, ValMark },         { "DRAW LISTS", TapDraw, ValDraw },            { "FLIP COPY", TapFlip, ValFlip },
    { "3D SCENES", TapGpu3D, ValGpu3D },      { "FORCE 3D", TapForce3D, ValForce3D },    { "DEPTH TINT", TapDepthTint, ValDepthTint },
    { "CAPTURE", TapCapture, ValCapture },
};

#endif // MANIA_DEV_TOOLS

// Tabs, in the bar at the top (icons as in the owner's other ports). Values are saved: append only. kTabs is the
// order they show in; DEBUG only with the dev tools.
enum Tab { TAB_GAME, TAB_OPTIONS, TAB_DEBUG, TAB_UPDATES };
#if MANIA_DEV_TOOLS
static const int kTabs[] = { TAB_GAME, TAB_OPTIONS, TAB_UPDATES, TAB_DEBUG };
#else
static const int kTabs[] = { TAB_GAME, TAB_OPTIONS, TAB_UPDATES };
#endif
enum { TAB_SHOWN = sizeof(kTabs) / sizeof(kTabs[0]) };

enum {
    HEADER_H  = 26,
    TAB_W     = 30,
    TAB_H     = 21,
    TAB_PITCH = 34,
};

#if MANIA_DEV_TOOLS

// DEBUG layout: the tab bar, the buttons fixed at the bottom (four to a row) and between them the information,
// which scrolls: drag it, or touch the bar on the right to jump.
enum {
    BTN_COUNT = sizeof(kButtons) / sizeof(kButtons[0]),
    BTN_COLS  = 4,
    BTN_GAP   = 4,
    BTN_W     = (W - (BTN_COLS + 1) * BTN_GAP) / BTN_COLS,
    BTN_H     = 28,
    BTN_ROWS  = (BTN_COUNT + BTN_COLS - 1) / BTN_COLS,
    BTN_TOP   = H - BTN_ROWS * (BTN_H + BTN_GAP),
    VIEW_TOP  = HEADER_H,
    VIEW_H    = BTN_TOP - VIEW_TOP,
    BAR_W     = 4,
    BAR_HIT_X = W - 20, // touches right of this in the view move the bar
};

static float sScroll;       // pixels of the information scrolled off the top
static float sContentH = 1; // height of the information, measured by the last draw
static int sDragY = -1;     // touch y of the drag in progress (-1: none)
static bool sDragBar;

static float MaxScroll() { return sContentH > VIEW_H ? sContentH - VIEW_H : 0; }
static void ClampScroll()
{
    if (sScroll > MaxScroll())
        sScroll = MaxScroll();
    if (sScroll < 0)
        sScroll = 0;
}

static void ButtonRect(int i, float *x, float *y)
{
    *x = BTN_GAP + (i % BTN_COLS) * (BTN_W + BTN_GAP);
    *y = BTN_TOP + (i / BTN_COLS) * (BTN_H + BTN_GAP);
}

// The scrolling information, drawn from y = `top` (the view's top minus the scroll); z stays below the header and
// the button panel, which are drawn after it and cover what overflows the view. Returns the content's height.
static float DrawInfo(float top)
{
    DevTools::Stats st;
    DevTools::GetStats(&st);
    char line[96];
    float y = top + 2;

    snprintf(line, sizeof(line), "FTP %s", DevTools::FtpStatus());
    Text(line, 8, y, 0.42f, Rgb(170, 220, 190));
    y += 13;

    // Frame rate: green at 59+, yellow from 50, red below
    const u32 fpsColor = st.fps >= 59 ? Rgb(90, 220, 110) : st.fps >= 50 ? Rgb(240, 210, 70) : Rgb(235, 90, 80);
    snprintf(line, sizeof(line), "%.0f", st.fps);
    Text(line, 10, y, 1.1f, fpsColor);
    Text("fps", 74, y + 18, 0.5f, Rgb(160, 170, 180));
    snprintf(line, sizeof(line), "frame %.1f ms   skipped %u/s", st.frameMs, st.skipped);
    Text(line, 118, y + 4, 0.45f, Rgb(210, 215, 220));
    snprintf(line, sizeof(line), "scene %d", st.scene);
    Text(line, 118, y + 18, 0.45f, Rgb(160, 170, 180));
    y += 38;

    // Stacked bar of one 16.7 ms frame: update | draw | flip, the rest idle; each figure below in its segment's colour
    const float budget = 16.7f, barX = 8, barW = W - 16 - BAR_W - 4, barH = 12;
    C2D_DrawRectSolid(barX, y, 0.2f, barW, barH, Rgb(34, 40, 48));
    const u32 colors[3] = { Rgb(80, 140, 235), Rgb(240, 150, 60), Rgb(150, 160, 170) };
    float x = barX;
    for (int i = 0; i < 3; ++i) {
        float w = st.stageMs[i] / budget * barW;
        if (x + w > barX + barW)
            w = barX + barW - x;
        if (w > 0)
            C2D_DrawRectSolid(x, y, 0.3f, w, barH, colors[i]);
        x += w;
    }
    y += 14;
    {
        static const char *kName[3] = { "update", "draw", "flip" };
        float tx = 8;
        for (int i = 0; i < 3; ++i) {
            snprintf(line, sizeof(line), "%s %.1f   ", kName[i], st.stageMs[i]);
            tx = TextRun(line, tx, y, 0.42f, colors[i]);
        }
        snprintf(line, sizeof(line), "idle %.1f", st.stageMs[DevTools::STAGE_WAIT]);
        TextRun(line, tx, y, 0.42f, Rgb(95, 105, 120)); // the bar's empty part
    }
    y += 11;
    // What draw is made of (the draw lists' calls; in GPU mode their recording) and flip's GPU parts.
    snprintf(line, sizeof(line), "layers %.1f  sprites %.1f  3d %.1f  faces %.1f", st.stageMs[DevTools::STAGE_LAYERS], st.stageMs[DevTools::STAGE_SPRITES],
             st.stageMs[DevTools::STAGE_3D], st.stageMs[DevTools::STAGE_FACES]);
    Text(line, 8, y, 0.38f, Rgb(200, 135, 70));
    y += 11;
    snprintf(line, sizeof(line), "flip: geometry %.1f  textures %.1f  GPU frame %.1f", st.stageMs[DevTools::STAGE_PCOPY], st.stageMs[DevTools::STAGE_PXFER],
             st.stageMs[DevTools::STAGE_PFRAME]);
    Text(line, 8, y, 0.38f, Rgb(125, 135, 145));
    y += 15;

    // Renderer: the mode, who drew the last frame, frames per renderer, and why the CPU drew them in GPU mode.
    DevTools::RenderStats rs;
    DevTools::GetRenderStats(&rs);
    const u32 gpuCol = Rgb(90, 220, 110), cpuCol = Rgb(240, 170, 70);
    C2D_DrawRectSolid(0, y - 1, 0.1f, W - BAR_W - 4, 41, Rgb(22, 28, 36));
    snprintf(line, sizeof(line), "RENDERER %s", rs.gpuMode ? "GPU" : "CPU");
    Text(line, 8, y, 0.5f, rs.gpuMode ? gpuCol : Rgb(200, 205, 210));
    if (rs.lastFrameGpu)
        snprintf(line, sizeof(line), "last frame GPU");
    else if (rs.gpuMode && rs.lastWhy)
        snprintf(line, sizeof(line), "last frame CPU: %s", rs.lastWhy);
    else
        snprintf(line, sizeof(line), "last frame CPU");
    Text(line, W - BAR_W - 10, y + 1, 0.45f, rs.lastFrameGpu ? gpuCol : cpuCol, C2D_AlignRight);
    y += 14;
    snprintf(line, sizeof(line), "1 s: GPU %u  CPU %u     total: GPU %u  CPU %u", rs.secGpu, rs.secCpu, rs.totalGpu, rs.totalCpu);
    Text(line, 8, y, 0.4f, Rgb(200, 205, 210));
    y += 12;
    if (rs.gpuMode) {
        if (rs.top[0].why) {
            int n = snprintf(line, sizeof(line), "CPU:");
            for (int i = 0; i < 3 && rs.top[i].why && n < (int)sizeof(line); ++i)
                n += snprintf(line + n, sizeof(line) - n, " %s %u", rs.top[i].why, rs.top[i].count);
            Text(line, 8, y, 0.38f, cpuCol);
        }
        else
            Text("every frame on the GPU", 8, y, 0.38f, gpuCol);
    }
    else
        Text("software renderer, CPU copy to the screen", 8, y, 0.38f, Rgb(140, 150, 160));
    y += 14;
    return y - top;
}

#endif // MANIA_DEV_TOOLS

// ---------------------------------------------------------------- tab bar

static void TabRect(int i, float *x, float *y)
{
    *x = 4 + i * TAB_PITCH;
    *y = 2;
}

// 14x14 icons made of rectangles: a ring (GAME), sliders (OPTIONS), a bug (DEBUG).
static void DrawTabIcon(int tab, float cx, float cy, u32 col, float z)
{
    switch (tab) {
        case TAB_GAME: { // a ring, 12x12, in runs (C2D_DrawCircleSolid drew nothing here)
            static const char *const kRing[12] = { "....XXXX....", "..XXXXXXXX..", ".XXX....XXX.", ".XX......XX.", "XX........XX", "XX........XX",
                                                   "XX........XX", "XX........XX", ".XX......XX.", ".XXX....XXX.", "..XXXXXXXX..", "....XXXX...." };
            for (int r = 0; r < 12; ++r)
                for (int c = 0; c < 12;) {
                    if (kRing[r][c] != 'X') {
                        ++c;
                        continue;
                    }
                    int e = c;
                    while (e < 12 && kRing[r][e] == 'X')
                        ++e;
                    C2D_DrawRectSolid(cx - 6 + c, cy - 6 + r, z, e - c, 1, col);
                    c = e;
                }
            break;
        }
        case TAB_OPTIONS:
            for (int i = 0; i < 3; ++i) {
                C2D_DrawRectSolid(cx - 7, cy - 5 + i * 5, z, 14, 1, col);
                C2D_DrawRectSolid(cx - 5 + ((i * 5) % 9), cy - 6 + i * 5, z, 3, 3, col);
            }
            break;
        case TAB_UPDATES:
            C2D_DrawRectSolid(cx - 1, cy - 7, z, 3, 7, col); // the arrow's shaft
            for (int i = 0; i < 4; ++i)                      // its head
                C2D_DrawRectSolid(cx - 4 + i, cy - 1 + i, z, 9 - 2 * i, 1, col);
            C2D_DrawRectSolid(cx - 7, cy + 2, z, 2, 5, col); // the tray
            C2D_DrawRectSolid(cx + 6, cy + 2, z, 2, 5, col);
            C2D_DrawRectSolid(cx - 7, cy + 6, z, 15, 2, col);
            break;
        default:
            C2D_DrawRectSolid(cx - 3, cy - 4, z, 6, 9, col);  // body
            C2D_DrawRectSolid(cx - 2, cy - 7, z, 4, 3, col);  // head
            for (int i = 0; i < 3; ++i) {                     // legs
                C2D_DrawRectSolid(cx - 7, cy - 3 + i * 3, z, 4, 1, col);
                C2D_DrawRectSolid(cx + 3, cy - 3 + i * 3, z, 4, 1, col);
            }
            break;
    }
}

static void DrawTabBar()
{
    C2D_DrawRectSolid(0, 0, 0.7f, W, HEADER_H, Rgb(14, 18, 24));
    C2D_DrawRectSolid(0, HEADER_H - 1, 0.71f, W, 1, Rgb(44, 52, 64));
    for (int i = 0; i < TAB_SHOWN; ++i) {
        float x, y;
        TabRect(i, &x, &y);
        const bool on = kTabs[i] == Port::tab;
        C2D_DrawRectSolid(x, y, 0.72f, TAB_W, TAB_H, on ? Rgb(45, 150, 240) : Rgb(50, 56, 75));
        C2D_DrawRectSolid(x + 1, y + 1, 0.73f, TAB_W - 2, TAB_H - 2, on ? Rgb(18, 70, 130) : Rgb(26, 30, 42));
        DrawTabIcon(kTabs[i], x + TAB_W / 2.0f, y + TAB_H / 2.0f, on ? Rgb(255, 255, 255) : Rgb(140, 150, 175), 0.74f);
    }

    // Battery, then the clock, at the right end
    u8 level = 5, charging = 0;
    PTMU_GetBatteryLevel(&level);
    PTMU_GetBatteryChargeState(&charging);
    const float bx = W - 22, by = 9;
    C2D_DrawRectSolid(bx, by, 0.72f, 15, 8, Rgb(150, 160, 180));
    C2D_DrawRectSolid(bx + 15, by + 2, 0.72f, 2, 4, Rgb(150, 160, 180));
    C2D_DrawRectSolid(bx + 1, by + 1, 0.73f, 13, 6, Rgb(14, 18, 24));
    const u32 fill = charging ? Rgb(100, 220, 255) : level <= 1 ? Rgb(240, 100, 100) : level == 2 ? Rgb(240, 200, 80) : Rgb(110, 220, 130);
    C2D_DrawRectSolid(bx + 2, by + 2, 0.74f, 11.0f * (level > 5 ? 5 : level) / 5, 4, fill);
    char clock[8];
    const time_t now = time(NULL);
    const struct tm *tm = gmtime(&now); // the 3DS clock is local time already
    snprintf(clock, sizeof(clock), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    Text(clock, bx - 6, 6, 0.5f, Rgb(210, 220, 235), C2D_AlignRight, 0.75f);
}

// ---------------------------------------------------------------- GAME tab

static const char *CharName(int id)
{
    switch (id) {
        case 1: return "SONIC";
        case 2: return "TAILS";
        case 4: return "KNUCKLES";
        case 8: return "MIGHTY";
        case 16: return "RAY";
        default: return NULL;
    }
}

static void StatBox(float x, float y, float w, float h, const char *label, const char *value, u32 valueCol)
{
    C2D_DrawRectSolid(x, y, 0.3f, w, h, Rgb(58, 66, 104));
    C2D_DrawRectSolid(x + 1, y + 1, 0.31f, w - 2, h - 2, Rgb(24, 28, 48));
    Text(label, x + 7, y + 4, 0.42f, Rgb(140, 148, 170), 0, 0.32f);
    Text(value, x + w - 8, y + 15, 0.85f, valueCol, C2D_AlignRight, 0.32f);
}

static void DrawGameTab()
{
    PortGameInfo gi;
    PortGame_GetInfo(&gi);
    char line[64];
    float y = HEADER_H + 6;

    // Where: the scene's name from the scene list, and the mode
    const char *scene = sceneInfo.listData ? sceneInfo.listData[sceneInfo.listPos].name : "";
    snprintf(line, sizeof(line), "%s", scene);
    for (char *c = line; *c; ++c)
        *c = (char)toupper((unsigned char)*c);
    Text(line, 10, y, 0.62f, Rgb(255, 215, 0));
    static const char *const kModes[] = { "MANIA MODE", "ENCORE MODE", "TIME ATTACK", "COMPETITION" };
    Text(gi.gameMode >= 0 && gi.gameMode < 4 ? kModes[gi.gameMode] : "", W - 10, y + 3, 0.45f, Rgb(140, 148, 170), C2D_AlignRight);
    y += 20;

    // Who
    const char *lead = CharName(gi.character), *side = CharName(gi.sidekick);
    if (lead && side)
        snprintf(line, sizeof(line), "%s & %s", lead, side);
    else
        snprintf(line, sizeof(line), "%s", lead ? lead : "-");
    Text(line, 10, y, 0.5f, Rgb(100, 220, 255));
    if (gi.saveSlot >= 0)
        snprintf(line, sizeof(line), "SAVE %d", gi.saveSlot + 1);
    else
        snprintf(line, sizeof(line), "NO SAVE");
    Text(line, W - 10, y + 1, 0.45f, Rgb(140, 148, 170), C2D_AlignRight);
    y += 20;

    // Time, rings, score, lives
    const float bw = (W - 24) / 2.0f, bh = 40;
    const bool inStage = gi.playerValid;
    snprintf(line, sizeof(line), "%d'%02d\"%02d", sceneInfo.minutes, sceneInfo.seconds, sceneInfo.milliseconds);
    StatBox(8, y, bw, bh, "TIME", inStage ? line : "-", Rgb(230, 232, 240));
    snprintf(line, sizeof(line), "%d", gi.rings);
    StatBox(16 + bw, y, bw, bh, "RINGS", inStage ? line : "-", inStage && !gi.rings ? Rgb(240, 100, 100) : Rgb(255, 215, 0));
    y += bh + 6;
    snprintf(line, sizeof(line), "%d", gi.score);
    StatBox(8, y, bw, bh, "SCORE", inStage ? line : "-", Rgb(230, 232, 240));
    snprintf(line, sizeof(line), "%d", gi.lives);
    StatBox(16 + bw, y, bw, bh, "LIVES", inStage ? line : "-", Rgb(110, 220, 130));
    y += bh + 8;

    // Chaos emeralds of the save, in the game's order
    Text("EMERALDS", 10, y + 3, 0.42f, Rgb(140, 148, 170));
    static const u32 kEmerald[7] = { Rgb(60, 200, 90),  Rgb(240, 210, 60), Rgb(70, 120, 240), Rgb(170, 90, 220),
                                     Rgb(190, 195, 205), Rgb(80, 220, 230), Rgb(235, 70, 70) };
    for (int i = 0; i < 7; ++i) {
        const float ex = 90 + i * 22, ey = y + 8;
        const bool got = gi.emeralds > 0 && (gi.emeralds & (1 << i));
        const u32 col = got ? kEmerald[i] : Rgb(50, 56, 75);
        C2D_DrawTriangle(ex, ey - 7, col, ex - 7, ey, col, ex + 7, ey, col, 0.3f);
        C2D_DrawTriangle(ex - 7, ey, col, ex + 7, ey, col, ex, ey + 7, col, 0.3f);
    }
    y += 22;

    static const char *const kShields[] = { "NONE", "SHIELD", "BUBBLE", "FIRE", "LIGHTNING" };
    if (inStage) {
        snprintf(line, sizeof(line), "SHIELD %s%s", gi.shield >= 0 && gi.shield < 5 ? kShields[gi.shield] : "?", gi.super ? "     SUPER" : "");
        Text(line, 10, y, 0.42f, Rgb(200, 205, 210));
    }

    // Footer: frame rate and renderer
    snprintf(line, sizeof(line), "%.0f FPS  %s", Port::Fps(), Port::GpuMode() ? "GPU" : "CPU");
    Text(line, W / 2.0f, H - 16, 0.42f, Rgb(90, 115, 145), C2D_AlignCenter);
}

// ---------------------------------------------------------------- OPTIONS tab

enum { OPT_FRAMES, OPT_FPS, OPT_HUD, OPT_RENDERER, OPT_COUNT };

static void OptRect(int i, float *x, float *y)
{
    *x = 8 + (i % 2) * 154;
    *y = HEADER_H + 8 + (i / 2) * 38;
}

static void OptValue(int i, char *o, unsigned cap, u32 *col)
{
    const u32 good = Rgb(110, 220, 130), dim = Rgb(140, 148, 170), warn = Rgb(240, 200, 80);
    switch (i) {
        case OPT_FRAMES:
            snprintf(o, cap, "%s", Port::lock30 ? "LOCK 30" : Port::frameSkip ? "AUTO" : "NO SKIP");
            *col = Port::lock30 ? good : Port::frameSkip ? good : warn;
            break;
        case OPT_FPS: {
            static const char *const k[] = { "OFF", "TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT" };
            snprintf(o, cap, "%s", k[Port::fps]);
            *col = Port::fps ? good : dim;
            break;
        }
        case OPT_HUD: {
            static const char *const k[] = { "SHOWN", "AUTO", "HIDDEN" };
            snprintf(o, cap, "%s", k[Port::hud]);
            *col = Port::hud == 2 ? warn : good;
            break;
        }
        default:
            snprintf(o, cap, "%s", Port::GpuMode() ? "GPU" : "CPU");
            *col = Port::GpuMode() ? good : dim;
            break;
    }
}

static const char *const kOptName[OPT_COUNT] = { "FRAMES", "SHOW FPS", "TOP HUD", "RENDERER" };
static const char *const kOptHelp[OPT_COUNT] = {
    "AUTO SKIPS A LATE PICTURE, LOCK 30 DRAWS EVERY OTHER ONE, NO SKIP SLOWS DOWN",
    "FRAME RATE ON THE TOP SCREEN: L LOGIC, R RENDER (MS)",
    "AUTO HIDES THE GAME'S HUD WHILE THE GAME TAB SHOWS IT",
    "GPU DRAWS WHAT IT CAN (FASTER), CPU IS THE ORIGINAL SOFTWARE RENDERER",
};
static int sLastOpt = -1;

static void TapOption(int i)
{
    switch (i) {
        case OPT_FRAMES: // AUTO -> LOCK 30 -> NO SKIP -> AUTO
            if (Port::lock30)
                Port::lock30 = false, Port::frameSkip = false;
            else if (Port::frameSkip)
                Port::lock30 = true;
            else
                Port::frameSkip = true;
            break;
        case OPT_FPS: Port::fps = (Port::fps + 1) % 5; break;
        case OPT_HUD: Port::hud = (Port::hud + 1) % 3; break;
        default: Port::SetGpuMode(!Port::GpuMode()); break;
    }
    sLastOpt = i;
}

static void DrawOptionsTab()
{
    char val[32];
    for (int i = 0; i < OPT_COUNT; ++i) {
        float x, y;
        OptRect(i, &x, &y);
        const bool pressed = i == sPressed;
        C2D_DrawRectSolid(x, y, 0.3f, 150, 32, pressed ? Rgb(210, 235, 255) : Rgb(50, 80, 130));
        C2D_DrawRectSolid(x + 1, y + 1, 0.31f, 148, 30, pressed ? Rgb(90, 150, 220) : Rgb(24, 32, 50));
        C2D_DrawRectSolid(x + 1, y + 1, 0.32f, 148, 1, Rgb(90, 130, 180));
        u32 col;
        OptValue(i, val, sizeof(val), &col);
        Text(kOptName[i], x + 7, y + 3, 0.42f, Rgb(230, 232, 240), 0, 0.33f);
        if (i == OPT_FPS && Port::fps) {
            // A little top screen with the counter's corner filled in, as sm-3ds shows it
            const float sx = x + 7, sy = y + 15, sw = 26, sh = 14;
            C2D_DrawRectSolid(sx, sy, 0.33f, sw, sh, Rgb(140, 148, 170));
            C2D_DrawRectSolid(sx + 1, sy + 1, 0.34f, sw - 2, sh - 2, Rgb(14, 16, 28));
            const int corner = Port::fps - 1;
            C2D_DrawRectSolid((corner & 1) ? sx + sw - 10 : sx + 2, (corner & 2) ? sy + sh - 6 : sy + 2, 0.35f, 8, 4, col);
            Text(val, sx + sw + 6, y + 15, 0.5f, col, 0, 0.33f);
        }
        else
            Text(val, x + 7, y + 15, 0.5f, col, 0, 0.33f);
    }
    // What the last option touched does
    if (sLastOpt >= 0)
        Text(kOptHelp[sLastOpt], 12, HEADER_H + 8 + 2 * 38 + 6, 0.4f, Rgb(240, 200, 80), C2D_WordWrap, 0.33f, W - 24);
    Text("SONIC MANIA 3DS", W / 2.0f, H - 30, 0.45f, Rgb(90, 115, 145), C2D_AlignCenter);
    Text("V" PORT_VERSION_LABEL, W / 2.0f, H - 17, 0.4f, Rgb(90, 115, 145), C2D_AlignCenter);
}

// ---------------------------------------------------------------- UPDATES tab, the update prompt
// As in ../zelda-tmc-3ds (update_ui_3ds.inc): CHANNEL, AUTO UPDATE and the RELEASE found (tapped: its notes), the
// updater's status, one action button (check, download, a confirmation, cancel); the prompt raised by the check at
// boot (Updater_AutoPrompt) covers every tab until it is answered.

enum { UHIT_CHANNEL, UHIT_AUTO, UHIT_RELEASE, UHIT_ACTION, UHIT_PREV, UHIT_CLOSE, UHIT_NEXT };
enum { PHIT_LATER, PHIT_INSTALL, PHIT_NEWS, PHIT_PREV, PHIT_CLOSE, PHIT_NEXT };

static bool sUpdConfirm, sUpdNotes, sPromptNotes;
static unsigned sNotePage, sDrawnRevision = ~0u;
static const char *sUpdHint; // what the last tap did, when it did nothing visible

enum { NOTE_COLS = 42, NOTE_MAX = 256, NOTE_ROWS = 13 };
static char sNotes[NOTE_MAX][NOTE_COLS + 1];
static unsigned sNoteCount, sNoteRevision = ~0u;

static void UpperCopy(char *dst, unsigned cap, const char *src)
{
    unsigned n = 0;
    for (; src && *src && n + 1 < cap; ++src)
        dst[n++] = (char)toupper((unsigned char)*src);
    dst[n] = 0;
}

// The release's notes as lines of NOTE_COLS characters (Update_FormatNotes), loaded when the updater's data changes.
static void LoadNotes(const UpdateStatus &st)
{
    if (sNoteRevision == st.revision)
        return;
    static char body[12289];
    Updater_GetNotes(body, sizeof(body));
    sNoteCount    = Update_FormatNotes(body, sNotes, NOTE_MAX);
    sNoteRevision = st.revision;
    for (unsigned i = 0; i < sNoteCount; ++i)
        UpperCopy(sNotes[i], sizeof(sNotes[i]), sNotes[i]);
}

static unsigned NotePages() { return sNoteCount ? (sNoteCount + NOTE_ROWS - 1) / NOTE_ROWS : 1; }

static void Button(float x, float y, float w, float h, const char *label, bool pressed, u32 edge, u32 textCol, float z)
{
    C2D_DrawRectSolid(x, y, z, w, h, pressed ? Rgb(210, 235, 255) : edge);
    C2D_DrawRectSolid(x + 1, y + 1, z + 0.01f, w - 2, h - 2, pressed ? Rgb(90, 150, 220) : Rgb(24, 32, 50));
    Text(label, x + w / 2, y + h / 2 - 7, 0.5f, textCol, C2D_AlignCenter, z + 0.02f);
}

static bool Inside(int px, int py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }

// The notes, paged, in the box (x, y, w) and its PREV / CLOSE / NEXT row at `by` (hit ids prev, prev + 1, prev + 2).
static void DrawNotes(const UpdateStatus &st, float x, float y, float w, float by, int prev, float z)
{
    LoadNotes(st);
    const unsigned pages = NotePages();
    if (sNotePage >= pages)
        sNotePage = pages - 1;
    char head[80], ver[48];
    UpperCopy(ver, sizeof(ver), st.version);
    snprintf(head, sizeof(head), "WHAT'S NEW %s   %u/%u", ver, sNotePage + 1, pages);
    Text(head, x + w / 2, y, 0.45f, Rgb(255, 215, 0), C2D_AlignCenter, z);
    C2D_DrawRectSolid(x, y + 16, z, w, by - y - 20, Rgb(40, 70, 120));
    C2D_DrawRectSolid(x + 1, y + 17, z + 0.01f, w - 2, by - y - 22, Rgb(10, 14, 24));
    for (unsigned i = sNotePage * NOTE_ROWS; i < sNoteCount && i < (sNotePage + 1) * NOTE_ROWS; ++i)
        Text(sNotes[i], x + 5, y + 19 + (i - sNotePage * NOTE_ROWS) * 11, 0.38f, Rgb(220, 224, 235), 0, z + 0.02f);
    const float bw = (w - 8) / 3;
    Button(x, by, bw, 24, "PREV", sPressed == prev, Rgb(50, 80, 130), sNotePage ? Rgb(230, 232, 240) : Rgb(90, 100, 115), z);
    Button(x + bw + 4, by, bw, 24, "CLOSE", sPressed == prev + 1, Rgb(50, 80, 130), Rgb(230, 232, 240), z);
    Button(x + 2 * (bw + 4), by, bw, 24, "NEXT", sPressed == prev + 2, Rgb(50, 80, 130),
           sNotePage + 1 < pages ? Rgb(230, 232, 240) : Rgb(90, 100, 115), z);
}

static int NotesHit(int px, int py, float x, float w, float by, int prev)
{
    const float bw = (w - 8) / 3;
    for (int i = 0; i < 3; ++i)
        if (Inside(px, py, x + i * (bw + 4), by, bw, 24))
            return prev + i;
    return -1;
}

static void NotesTap(int which) // 0 prev, 1 close, 2 next
{
    if (which == 0 && sNotePage)
        --sNotePage;
    else if (which == 2 && sNotePage + 1 < NotePages())
        ++sNotePage;
}

// The page's geometry
enum { UCELL_Y = HEADER_H + 6, UCELL_H = 32, USTATUS_Y = UCELL_Y + UCELL_H + 6, USTATUS_H = 64, UACTION_Y = H - 32 };
static void UCellRect(int i, float *x, float *w)
{
    *w = (W - 16 - 8) / 3.0f;
    *x = 8 + i * (*w + 4);
}

static const char *UpdateAction(const UpdateStatus &st)
{
    if (sUpdConfirm)
        return "INSTALL UPDATE";
    if (st.state == UPDATE_INSTALLING || st.state == UPDATE_VERIFYING)
        return "PLEASE WAIT";
    if (Updater_Busy())
        return "CANCEL";
    if (st.state == UPDATE_AVAILABLE)
        return "DOWNLOAD UPDATE";
    if (st.state == UPDATE_DONE)
        return "UPDATE INSTALLED";
    return "CHECK FOR UPDATE";
}

static void DrawUpdatesTab()
{
    UpdateStatus st;
    Updater_GetStatus(&st);
    sDrawnRevision = st.revision;
    if (sUpdNotes) {
        DrawNotes(st, 8, HEADER_H + 4, W - 16, UACTION_Y, UHIT_PREV, 0.3f);
        return;
    }
    char val[48];
    const u32 good = Rgb(110, 220, 130), dim = Rgb(140, 148, 170), warn = Rgb(240, 200, 80), bad = Rgb(240, 100, 100);
    static const char *const kName[3] = { "CHANNEL", "AUTO UPDATE", "RELEASE" };
    for (int i = 0; i < 3; ++i) {
        float x, w;
        UCellRect(i, &x, &w);
        const bool pressed = i == sPressed;
        C2D_DrawRectSolid(x, UCELL_Y, 0.3f, w, UCELL_H, pressed ? Rgb(210, 235, 255) : Rgb(50, 80, 130));
        C2D_DrawRectSolid(x + 1, UCELL_Y + 1, 0.31f, w - 2, UCELL_H - 2, pressed ? Rgb(90, 150, 220) : Rgb(24, 32, 50));
        u32 col = Rgb(230, 232, 240);
        if (i == 0)
            snprintf(val, sizeof(val), "%s", Port::betas ? "+ BETAS" : "STABLE");
        else if (i == 1)
            snprintf(val, sizeof(val), "%s", Port::autoUpdate ? "ON" : "OFF"), col = Port::autoUpdate ? good : dim;
        else if (st.version[0])
            UpperCopy(val, sizeof(val), st.version);
        else
            snprintf(val, sizeof(val), "NONE"), col = dim;
        Text(kName[i], x + 6, UCELL_Y + 3, 0.4f, Rgb(170, 178, 200), 0, 0.33f);
        Text(val, x + 6, UCELL_Y + 15, 0.45f, col, 0, 0.33f);
    }

    // Status: what the updater says (or the question before an install), with what is installed.
    C2D_DrawRectSolid(8, USTATUS_Y, 0.3f, W - 16, USTATUS_H, Rgb(58, 66, 104));
    C2D_DrawRectSolid(9, USTATUS_Y + 1, 0.31f, W - 18, USTATUS_H - 2, Rgb(24, 28, 48));
    Text("STATUS", 14, USTATUS_Y + 4, 0.4f, dim, 0, 0.32f);
    Text("INSTALLED V" PORT_VERSION_LABEL, W - 14, USTATUS_Y + 4, 0.38f, dim, C2D_AlignRight, 0.32f);
    char msg[96];
    UpperCopy(msg, sizeof(msg), sUpdConfirm ? "Download and install this version?" : st.message[0] ? st.message : "Check looks for a new version");
    const u32 col = st.state == UPDATE_ERROR ? bad : (st.state == UPDATE_AVAILABLE || sUpdConfirm) ? warn
                    : (st.state == UPDATE_DONE || st.state == UPDATE_CURRENT) ? good : Rgb(230, 232, 240);
    Text(msg, 14, USTATUS_Y + 20, 0.5f, col, C2D_WordWrap, 0.32f, W - 28);
    const bool working = st.state == UPDATE_DOWNLOADING || st.state == UPDATE_VERIFYING || st.state == UPDATE_INSTALLING;
    if (working) {
        const float bx = 14, by = USTATUS_Y + USTATUS_H - 14, bw = W - 28;
        C2D_DrawRectSolid(bx, by, 0.33f, bw, 8, Rgb(44, 54, 70));
        C2D_DrawRectSolid(bx, by, 0.34f, bw * (st.progress > 100 ? 100 : st.progress) / 100.0f, 8, Rgb(60, 160, 100));
    }
    const char *hint = sUpdConfirm ? "UNSAVED PROGRESS IS LOST" : sUpdHint ? sUpdHint : st.version[0] ? "TAP RELEASE FOR WHAT'S NEW" : "";
    Text(hint, W / 2.0f, USTATUS_Y + USTATUS_H + 8, 0.4f, sUpdConfirm ? warn : Rgb(90, 115, 145), C2D_AlignCenter, 0.32f);

    Button(8, UACTION_Y, W - 16, 26, UpdateAction(st), sPressed == UHIT_ACTION, sUpdConfirm ? warn : Rgb(50, 80, 130), Rgb(230, 232, 240), 0.3f);
}

static int UpdatesHit(int px, int py, bool down)
{
    (void)down;
    if (sUpdNotes)
        return NotesHit(px, py, 8, W - 16, UACTION_Y, UHIT_PREV);
    for (int i = 0; i < 3; ++i) {
        float x, w;
        UCellRect(i, &x, &w);
        if (Inside(px, py, x, UCELL_Y, w, UCELL_H))
            return i;
    }
    return Inside(px, py, 8, UACTION_Y, W - 16, 26) ? UHIT_ACTION : -1;
}

static void UpdatesTap(int i)
{
    UpdateStatus st;
    Updater_GetStatus(&st);
    sUpdHint = NULL;
    if (sUpdNotes) {
        if (i == UHIT_CLOSE)
            sUpdNotes = false;
        else
            NotesTap(i - UHIT_PREV);
        return;
    }
    if (st.state == UPDATE_INSTALLING || st.state == UPDATE_DONE)
        return;
    const bool busy = Updater_Busy();
    if (i == UHIT_ACTION) {
        const bool confirm = sUpdConfirm;
        sUpdConfirm = false;
        if (busy)
            Updater_Cancel();
        else if (confirm && st.state == UPDATE_AVAILABLE)
            Updater_Download();
        else if (st.state == UPDATE_AVAILABLE)
            sUpdConfirm = true;
        else
            Updater_Check();
        return;
    }
    sUpdConfirm = false;
    if (busy) {
        sUpdHint = "WAIT FOR THE UPDATER TO FINISH";
        return;
    }
    switch (i) {
        case UHIT_CHANNEL:
            Port::betas = !Port::betas;
            Updater_SetChannel(Port::betas);
            break;
        case UHIT_AUTO:
            Port::autoUpdate = !Port::autoUpdate;
            sUpdHint         = Port::autoUpdate ? "LOOKS FOR A NEW VERSION AT EVERY START" : "ONLY WHEN YOU CHECK HERE";
            break;
        case UHIT_RELEASE:
            if (st.version[0])
                sUpdNotes = true, sNotePage = 0;
            break;
        default: break;
    }
}

static bool UpdatesBusy()
{
    float p;
    return Updater_Busy() || Port::RelaunchSecondsLeft(&p) != 0;
}

static bool UpdatesChanged()
{
    UpdateStatus st;
    Updater_GetStatus(&st);
    return st.revision != sDrawnRevision;
}

static bool UpdatePromptShown() { return Updater_AutoPrompt() != UPDATE_PROMPT_NONE; }

// The prompt's box
enum { PBOX_X = 16, PBOX_Y = HEADER_H + 10, PBOX_W = W - 32, PBOX_H = H - HEADER_H - 20 };
static const float kPromptBy = PBOX_Y + PBOX_H - 32;

static void DrawUpdatePrompt()
{
    const UpdatePrompt prompt = Updater_AutoPrompt();
    if (prompt == UPDATE_PROMPT_NONE)
        return;
    UpdateStatus st;
    Updater_GetStatus(&st);
    sDrawnRevision = st.revision;
    const float z = 0.85f;
    C2D_DrawRectSolid(0, 0, 0.8f, W, H, C2D_Color32(0, 0, 0, 170));
    C2D_DrawRectSolid(PBOX_X - 2, PBOX_Y - 2, 0.82f, PBOX_W + 4, PBOX_H + 4, Rgb(90, 120, 170));
    C2D_DrawRectSolid(PBOX_X, PBOX_Y, 0.84f, PBOX_W, PBOX_H, Rgb(24, 32, 46));
    const float cx = W / 2.0f, x = PBOX_X + 8, w = PBOX_W - 16, by = kPromptBy;
    const u32 text = Rgb(230, 232, 240), dim = Rgb(140, 148, 170), warn = Rgb(240, 200, 80);
    char line[96], ver[48];
    UpperCopy(ver, sizeof(ver), st.version);
    if (prompt == UPDATE_PROMPT_ASK && sPromptNotes) {
        DrawNotes(st, x, PBOX_Y + 6, w, by, PHIT_PREV, z + 0.02f);
        return;
    }
    if (prompt == UPDATE_PROMPT_ASK) {
        Text("UPDATE AVAILABLE", cx, PBOX_Y + 8, 0.6f, Rgb(255, 215, 0), C2D_AlignCenter, 0.95f);
        snprintf(line, sizeof(line), "NEW VERSION: %s", ver);
        Text(line, cx, PBOX_Y + 34, 0.5f, text, C2D_AlignCenter, 0.95f);
        Text("INSTALL IT NOW?", cx, PBOX_Y + 52, 0.5f, text, C2D_AlignCenter, 0.95f);
        Text("UNSAVED PROGRESS IS LOST", cx, PBOX_Y + 70, 0.42f, warn, C2D_AlignCenter, 0.95f);
        Text("(UPDATES TAB: AUTO UPDATE)", cx, PBOX_Y + 86, 0.4f, dim, C2D_AlignCenter, 0.95f);
        Button(cx - 75, PBOX_Y + 108, 150, 24, "WHAT'S NEW", sPressed == PHIT_NEWS, Rgb(50, 80, 130), Rgb(100, 220, 255), 0.9f);
        const float bw = (w - 8) / 2;
        Button(x, by, bw, 24, "LATER", sPressed == PHIT_LATER, Rgb(50, 80, 130), text, 0.9f);
        Button(x + bw + 8, by, bw, 24, "INSTALL", sPressed == PHIT_INSTALL, warn, text, 0.9f);
        return;
    }
    if (prompt == UPDATE_PROMPT_ERROR) {
        Text("UPDATE FAILED", cx, PBOX_Y + 8, 0.6f, Rgb(240, 100, 100), C2D_AlignCenter, 0.95f);
        UpperCopy(line, sizeof(line), st.message);
        Text(line, cx, PBOX_Y + 44, 0.5f, text, C2D_AlignCenter, 0.95f);
        Text("THE GAME KEEPS RUNNING AS IT WAS.", cx, PBOX_Y + 66, 0.42f, dim, C2D_AlignCenter, 0.95f);
        Button(cx - 60, by, 120, 24, "OK", sPressed == PHIT_LATER, Rgb(50, 80, 130), text, 0.9f);
        return;
    }
    // WORKING / RESTART: one bar for the download and the install, then the 10 s before the game starts again.
    float fill;
    if (prompt == UPDATE_PROMPT_RESTART) {
        const unsigned secs = Port::RelaunchSecondsLeft(&fill);
        snprintf(line, sizeof(line), "%u S", secs);
        Text("RESTARTING THE GAME", cx, PBOX_Y + 40, 0.5f, text, C2D_AlignCenter, 0.95f);
    }
    else {
        fill = (st.progress > 100 ? 100 : st.progress) / 100.0f;
        snprintf(line, sizeof(line), "%u%%", st.progress > 100 ? 100 : st.progress);
        char msg[96];
        UpperCopy(msg, sizeof(msg), st.message);
        Text(msg, cx, PBOX_Y + 40, 0.5f, text, C2D_AlignCenter, 0.95f);
    }
    Text("UPDATING", cx, PBOX_Y + 8, 0.6f, Rgb(255, 215, 0), C2D_AlignCenter, 0.95f);
    const float px = x + 6, py = PBOX_Y + 66, pw = w - 12, ph = 18;
    C2D_DrawRectSolid(px, py, 0.86f, pw, ph, Rgb(44, 54, 70));
    C2D_DrawRectSolid(px, py, 0.87f, pw * fill, ph, prompt == UPDATE_PROMPT_RESTART ? Rgb(235, 140, 50) : Rgb(60, 160, 100));
    Text(line, cx, py + 2, 0.5f, Rgb(255, 255, 255), C2D_AlignCenter, 0.95f);
    Text("DO NOT TURN THE CONSOLE OFF.", cx, PBOX_Y + 96, 0.42f, warn, C2D_AlignCenter, 0.95f);
    const bool canCancel = prompt == UPDATE_PROMPT_WORKING && st.state == UPDATE_DOWNLOADING;
    Button(cx - 70, by, 140, 24, canCancel ? "CANCEL" : "PLEASE WAIT", canCancel && sPressed == PHIT_LATER, Rgb(50, 80, 130),
           canCancel ? text : dim, 0.9f);
}

static int PromptHit(int px, int py)
{
    const UpdatePrompt prompt = Updater_AutoPrompt();
    const float cx = W / 2.0f, x = PBOX_X + 8, w = PBOX_W - 16, by = kPromptBy;
    if (prompt == UPDATE_PROMPT_ASK && sPromptNotes)
        return NotesHit(px, py, x, w, by, PHIT_PREV);
    if (prompt == UPDATE_PROMPT_ASK) {
        const float bw = (w - 8) / 2;
        if (Inside(px, py, cx - 75, PBOX_Y + 108, 150, 24))
            return PHIT_NEWS;
        if (Inside(px, py, x, by, bw, 24))
            return PHIT_LATER;
        if (Inside(px, py, x + bw + 8, by, bw, 24))
            return PHIT_INSTALL;
        return -1;
    }
    if (prompt == UPDATE_PROMPT_ERROR)
        return Inside(px, py, cx - 60, by, 120, 24) ? PHIT_LATER : -1;
    return Inside(px, py, cx - 70, by, 140, 24) ? PHIT_LATER : -1;
}

static void PromptTap(int i)
{
    if (sPromptNotes) {
        if (i == PHIT_CLOSE)
            sPromptNotes = false;
        else
            NotesTap(i - PHIT_PREV);
        return;
    }
    if (i == PHIT_NEWS) {
        sPromptNotes = true;
        sNotePage    = 0;
        return;
    }
    Updater_AutoAnswer(i == PHIT_INSTALL);
}

// ---------------------------------------------------------------- DEBUG tab

#if MANIA_DEV_TOOLS

static void DrawDebugTab()
{
    char line[96], val[32];
    ClampScroll();
    sContentH = DrawInfo(VIEW_TOP - sScroll);
    ClampScroll();

    // Scroll bar, only when the information does not fit
    if (sContentH > VIEW_H) {
        const float bx = W - BAR_W - 2, thumbH = VIEW_H * VIEW_H / sContentH;
        const float thumbY = VIEW_TOP + (VIEW_H - thumbH) * (sScroll / MaxScroll());
        C2D_DrawRectSolid(bx, VIEW_TOP, 0.55f, BAR_W, VIEW_H, Rgb(34, 40, 48));
        C2D_DrawRectSolid(bx, thumbY, 0.56f, BAR_W, thumbH, sDragBar || sDragY >= 0 ? Rgb(150, 190, 240) : Rgb(100, 120, 150));
    }

    // Button panel, over what scrolled under it
    C2D_DrawRectSolid(0, BTN_TOP - BTN_GAP / 2, 0.7f, W, H - BTN_TOP + BTN_GAP / 2, Rgb(14, 18, 24));
    C2D_DrawRectSolid(0, BTN_TOP - BTN_GAP / 2, 0.71f, W, 1, Rgb(44, 52, 64));
    for (int i = 0; i < BTN_COUNT; ++i) {
        float bx, by;
        ButtonRect(i, &bx, &by);
        bool on = false;
        val[0] = 0;
        kButtons[i].value(val, sizeof(val), &on);
        u32 fill = on ? Rgb(28, 88, 52) : Rgb(40, 46, 56);
        if (i == sPressed)
            fill = on ? Rgb(50, 130, 80) : Rgb(70, 82, 100);
        C2D_DrawRectSolid(bx, by, 0.72f, BTN_W, BTN_H, fill);
        C2D_DrawRectSolid(bx, by, 0.73f, 3, BTN_H, on ? Rgb(90, 220, 110) : Rgb(90, 100, 115));
        Text(kButtons[i].label, bx + 7, by + 2, 0.36f, Rgb(190, 198, 206), 0, 0.74f);
        Text(val, bx + 7, by + 12, 0.5f, Rgb(255, 255, 255), 0, 0.74f);
    }
    (void)line;
}

#endif // MANIA_DEV_TOOLS

static void Draw()
{
    char val[32];

    C2D_TextBufClear(sTextBuf);
    C3D_FrameBegin(0); // SYNCDRAW would wait for a vblank: up to 16 ms of the game's frame, four times a second
    C2D_Prepare();
    // The top-screen presentation turns blending off and the state is sticky: citro2d's glyph alpha needs it.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_TargetClear(sTarget, Rgb(14, 18, 24));
    C2D_SceneBegin(sTarget);

    switch (Port::tab) {
        case TAB_OPTIONS: DrawOptionsTab(); break;
        case TAB_UPDATES: DrawUpdatesTab(); break;
#if MANIA_DEV_TOOLS
        case TAB_DEBUG: DrawDebugTab(); break;
#endif
        default: DrawGameTab(); break;
    }
    DrawTabBar();
    DrawUpdatePrompt();

#if MANIA_DEV_TOOLS

    // An install in progress is a modal message: the screen is dimmed and a box with the step, a bar and the
    // percentage (or the seconds left before the restart) in it takes the middle.
    float progress;
    unsigned seconds;
    char title[48];
    const int phase = DevTools::InstallNotice(title, sizeof(title), &progress, &seconds);
    if (phase) {
        C2D_DrawRectSolid(0, 0, 0.8f, W, H, C2D_Color32(0, 0, 0, 170));
        const float bx = 30, by = 80, bw = W - 60, bh = 84;
        C2D_DrawRectSolid(bx - 2, by - 2, 0.82f, bw + 4, bh + 4, Rgb(90, 120, 170));
        C2D_DrawRectSolid(bx, by, 0.84f, bw, bh, Rgb(24, 32, 46));
        Text(title, W / 2.0f, by + 8, 0.6f, Rgb(255, 255, 255), C2D_AlignCenter, 0.95f); // above the dimmer and the box
        const float px = bx + 14, py = by + 44, pw = bw - 28, ph = 24;
        C2D_DrawRectSolid(px, py, 0.86f, pw, ph, Rgb(44, 54, 70));
        const float fill = progress < 0 ? 0 : progress > 1 ? 1 : progress;
        C2D_DrawRectSolid(px, py, 0.88f, pw * fill, ph, phase == 3 ? Rgb(235, 140, 50) : Rgb(60, 160, 100));
        if (phase == 3)
            snprintf(val, sizeof(val), "%us", seconds);
        else if (progress >= 0)
            snprintf(val, sizeof(val), "%u%%", (unsigned)(fill * 100));
        else
            snprintf(val, sizeof(val), "...");
        Text(val, W / 2.0f, py + 3, 0.55f, Rgb(255, 255, 255), C2D_AlignCenter, 0.95f);
    }
#endif

    C3D_FrameEnd(0);
}

// ---------------------------------------------------------------- top-screen FPS counter

// The counter's lines and box (top-screen pixels), shared by the GPU and the software drawing.
static int OverlayLines(char lines[3][16], u32 colors[3], int *bx, int *by, int *bw, int *bh)
{
    if (!Port::fps)
        return 0;
    const float fps = Port::Fps();
    snprintf(lines[0], 16, "%.0f FPS", fps);
    colors[0] = fps >= 59 ? 0x6EDC82 : fps >= 50 ? 0xF0C850 : 0xF06464;
#if MANIA_DEV_TOOLS // logic and render times (ms)
    DevTools::Stats st;
    DevTools::GetStats(&st);
    snprintf(lines[1], 16, "L%.1f", st.stageMs[0]);
    colors[1] = 0xE6E8F0;
    snprintf(lines[2], 16, "R%.1f", st.stageMs[1] + st.stageMs[2]);
    colors[2] = 0x8C94AA;
    const int n = 3;
#else
    const int n = 1;
#endif
    *bw = 9 * UiFont::ADVANCE + 3;
    *bh = n * 10 + 2;
    const int corner = Port::fps - 1; // top left, top right, bottom left, bottom right
    *bx = (corner & 1) ? 400 - 2 - *bw : 2;
    *by = (corner & 2) ? 240 - 2 - *bh : 2;
    return n;
}

void BottomUi::DrawTopOverlayGpu(void *target)
{
    char lines[3][16];
    u32 colors[3];
    int bx, by, bw, bh;
    const int n = OverlayLines(lines, colors, &bx, &by, &bw, &bh);
    if (!n || !sReady)
        return;
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_SceneBegin((C3D_RenderTarget *)target);
    C2D_DrawRectSolid(bx, by, 0.5f, bw, bh, C2D_Color32(0, 0, 0, 150));
    for (int i = 0; i < n; ++i) {
        const u32 c = C2D_Color32((colors[i] >> 16) & 0xFF, (colors[i] >> 8) & 0xFF, colors[i] & 0xFF, 255);
        UiFont::Runs(lines[i], bx + 2, by + 2 + i * 10, [&](int x, int y, int w) { C2D_DrawRectSolid(x, y, 0.6f, w, 1, c); });
    }
    C2D_Flush();
}

void BottomUi::DrawTopOverlaySoft(unsigned short *fb, int pitch)
{
    char lines[3][16];
    u32 colors[3];
    int bx, by, bw, bh;
    const int n = OverlayLines(lines, colors, &bx, &by, &bw, &bh);
    if (!n)
        return;
    for (int y = by; y < by + bh; ++y) // darken the box (about the GPU's 150/255 black)
        for (int x = bx; x < bx + bw; ++x) {
            const unsigned c = fb[y * pitch + x];
            fb[y * pitch + x] = (unsigned short)(((c >> 2) & 0x39E7) + ((c >> 3) & 0x18E3));
        }
    for (int i = 0; i < n; ++i) {
        const unsigned c = colors[i], c565 = ((((c >> 16) & 0xFF) >> 3) << 11) | ((((c >> 8) & 0xFF) >> 2) << 5) | ((c & 0xFF) >> 3);
        UiFont::Runs(lines[i], bx + 2, by + 2 + i * 10, [&](int x, int y, int w) {
            for (int k = 0; k < w; ++k)
                fb[y * pitch + x + k] = (unsigned short)c565;
        });
    }
}

// ---------------------------------------------------------------- loading box

static struct {
    bool active;
    char title[64], step[48], file[64];
    float lo, hi, progress;
    u64 start, lastDraw;
} sLoad;

static void DrawLoading()
{
    if (!sReady)
        return;
    char line[64];
    C2D_TextBufClear(sTextBuf);
    C3D_FrameBegin(0);
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_TargetClear(sTarget, Rgb(14, 16, 28));
    C2D_SceneBegin(sTarget);

    const float bx = 20, by = 62, bw = W - 40, bh = 116;
    C2D_DrawRectSolid(bx - 1, by - 1, 0.3f, bw + 2, bh + 2, Rgb(40, 70, 120));
    C2D_DrawRectSolid(bx, by, 0.31f, bw, bh, Rgb(10, 14, 24));
    Text(sLoad.title, W / 2.0f, by + 10, 0.55f, Rgb(255, 215, 0), C2D_AlignCenter, 0.4f);
    Text(sLoad.step, W / 2.0f, by + 34, 0.5f, Rgb(230, 232, 240), C2D_AlignCenter, 0.4f);
    Text(sLoad.file, W / 2.0f, by + 52, 0.4f, Rgb(140, 148, 170), C2D_AlignCenter, 0.4f);
    const float px = bx + 14, py = by + 72, pw = bw - 28, ph = 16;
    C2D_DrawRectSolid(px, py, 0.35f, pw, ph, Rgb(44, 54, 70));
    const float f = sLoad.progress < 0 ? 0 : sLoad.progress > 1 ? 1 : sLoad.progress;
    C2D_DrawRectSolid(px, py, 0.36f, pw * f, ph, Rgb(60, 160, 100));
    snprintf(line, sizeof(line), "%u%%", (unsigned)(f * 100));
    Text(line, W / 2.0f, py + 1, 0.45f, Rgb(255, 255, 255), C2D_AlignCenter, 0.4f);
    const float secs = (float)(svcGetSystemTick() - sLoad.start) / SYSCLOCK_ARM11;
    snprintf(line, sizeof(line), "%.1f S", secs);
    Text(line, bx + bw - 8, by + bh - 14, 0.38f, Rgb(90, 115, 145), C2D_AlignRight, 0.4f);
    Text("SONIC MANIA 3DS", W / 2.0f, H - 18, 0.42f, Rgb(90, 115, 145), C2D_AlignCenter);
    C3D_FrameEnd(0);
    sLoad.lastDraw = svcGetSystemTick();
}

static void MaybeDrawLoading(bool now)
{
    if (sLoad.active && (now || svcGetSystemTick() - sLoad.lastDraw > SYSCLOCK_ARM11 / 20))
        DrawLoading();
}

static void Upper(char *dst, unsigned cap, const char *src)
{
    unsigned n = 0;
    for (; src && *src && n + 1 < cap; ++src)
        dst[n++] = (char)toupper((unsigned char)*src);
    dst[n] = 0;
}

void BottomUi::LoadBegin(const char *title)
{
    Init(); // at boot this runs before the dev tools start
    Upper(sLoad.title, sizeof(sLoad.title), title);
    sLoad.step[0] = sLoad.file[0] = 0;
    sLoad.lo = sLoad.progress = 0;
    sLoad.hi                  = 1;
    sLoad.start               = svcGetSystemTick();
    sLoad.active              = true;
    MaybeDrawLoading(true);
}

void BottomUi::LoadPhase(const char *step, float lo, float hi)
{
    if (!sLoad.active)
        return;
    Upper(sLoad.step, sizeof(sLoad.step), step);
    sLoad.file[0] = 0;
    sLoad.lo = sLoad.progress = lo;
    sLoad.hi                  = hi;
    MaybeDrawLoading(true);
}

void BottomUi::LoadItem(int i, int n)
{
    if (!sLoad.active || n <= 0)
        return;
    sLoad.progress = sLoad.lo + (sLoad.hi - sLoad.lo) * i / n;
    MaybeDrawLoading(false);
}

void BottomUi::LoadDetail(const char *file)
{
    if (!sLoad.active)
        return;
    const char *slash = file ? strrchr(file, '/') : NULL;
    Upper(sLoad.file, sizeof(sLoad.file), slash ? slash + 1 : file);
    MaybeDrawLoading(false);
}

void BottomUi::LoadEnd()
{
#if MANIA_DEV_TOOLS
    if (sLoad.active) {
        DEV_LOG("load: %s in %lu ms", sLoad.title, (unsigned long)((svcGetSystemTick() - sLoad.start) / (SYSCLOCK_ARM11 / 1000)));
        // The engine's storage pools after the load: used / limit KB, entries, defragmentations and refused
        // allocations so far (a refused one leaves a layout empty: the scene drawn with tile 0).
        static const char *const kSet[DATASET_MAX] = { "STG", "MUS", "SFX", "STR", "TMP" };
        char line[256];
        unsigned w = 0;
        for (int d = 0; d < DATASET_MAX && w < sizeof(line); ++d)
            w += (unsigned)snprintf(line + w, sizeof(line) - w, "%s%s %u/%u KB e%u d%u f%u", d ? ", " : "", kSet[d],
                                    (unsigned)(dataStorage[d].usedStorage * 4 >> 10), (unsigned)(dataStorage[d].storageLimit >> 10),
                                    (unsigned)dataStorage[d].entryCount, (unsigned)storageDefrags[d], (unsigned)storageFails[d]);
        DEV_LOG("storage: %s", line);
    }
#endif
    sLoad.active = false;
    sDirty       = true; // the tabs again
}

// While the graphical screen is up, stdout and stderr go nowhere: the engine's prints went to the libctru text
// console, which writes into the same framebuffer and flashed over the UI until its next redraw.
static ssize_t NullWrite(struct _reent *, void *, const char *, size_t len) { return (ssize_t)len; }
static devoptab_t sNullOut;

void BottomUi::Init()
{
    if (sReady)
        return;
    if (!C3DPresent::InitCore())
        return;
    // The bottom screen is the UI's now (the text console lives in the same framebuffer): RGB565 like the top.
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    // RGBA8 like the other ports: the clear colour and the alpha blending of citro2d's text expect it; the
    // transfer to the screen converts to RGB565.
    sTarget = C3D_RenderTargetCreate(H, W, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (!sTarget)
        return;
    C3D_RenderTargetSetOutput(sTarget, GFX_BOTTOM, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    ptmuInit(); // battery level for the tab bar
    sTextBuf = C2D_TextBufNew(3072);
    if (!sTextBuf)
        return;
    sNullOut.name    = "null";
    sNullOut.write_r = NullWrite;
    devoptab_list[STD_OUT] = &sNullOut;
    devoptab_list[STD_ERR] = &sNullOut;
    sReady = true;
    sDirty = true;
}

bool BottomUi::Ready() { return sReady; }

void BottomUi::Update()
{
    if (!sReady)
        return;
#if MANIA_DEV_TOOLS
    if (!DevTools::swBottomUi)
        return;
    const bool debugTab = Port::tab == TAB_DEBUG;
#else
    const bool debugTab = false;
#endif
    const u32 down = hidKeysDown(), held = hidKeysHeld();
    int hit = -1;
    if (held & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        if (UpdatePromptShown()) {
            // The update prompt takes every touch while it is up.
            hit = PromptHit(t.px, t.py);
        }
        else if ((down & KEY_TOUCH) && t.py < HEADER_H) {
            // The tab bar
            for (int i = 0; i < TAB_SHOWN; ++i) {
                float x, y;
                TabRect(i, &x, &y);
                if (t.px >= x && t.px < x + TAB_W && t.py >= y && t.py < y + TAB_H && Port::tab != kTabs[i]) {
                    Port::tab = kTabs[i];
                    sLastOpt  = -1;
                    SaveAll();
                    sDirty = true;
                }
            }
#if MANIA_DEV_TOOLS
            sDragY   = -1;
            sDragBar = false;
#endif
        }
#if MANIA_DEV_TOOLS
        else if (debugTab) {
            if (down & KEY_TOUCH) {
                // A touch that starts in the view scrolls it (on the bar: jumps to that place); on a button it presses.
                sDragY   = t.py >= VIEW_TOP && t.py < BTN_TOP ? t.py : -1;
                sDragBar = sDragY >= 0 && t.px >= BAR_HIT_X && sContentH > VIEW_H;
            }
            if (sDragBar) {
                sScroll = (t.py - VIEW_TOP) * MaxScroll() / VIEW_H;
                sDirty  = true;
            }
            else if (sDragY >= 0) {
                if (t.py != sDragY) {
                    sScroll += sDragY - t.py;
                    sDragY  = t.py;
                    sDirty  = true;
                }
            }
            else {
                for (int i = 0; i < BTN_COUNT; ++i) {
                    float bx, by;
                    ButtonRect(i, &bx, &by);
                    if (t.px >= bx && t.px < bx + BTN_W && t.py >= by && t.py < by + BTN_H)
                        hit = i;
                }
            }
            ClampScroll();
        }
#endif
        else if (Port::tab == TAB_OPTIONS) {
            for (int i = 0; i < OPT_COUNT; ++i) {
                float x, y;
                OptRect(i, &x, &y);
                if (t.px >= x && t.px < x + 150 && t.py >= y && t.py < y + 32)
                    hit = i;
            }
        }
        else if (Port::tab == TAB_UPDATES)
            hit = UpdatesHit(t.px, t.py, (down & KEY_TOUCH) != 0);
    }
#if MANIA_DEV_TOOLS
    else if (sDragY >= 0 || sDragBar) {
        sDragY   = -1;
        sDragBar = false;
        sDirty   = true;
    }
#endif
    if ((down & KEY_TOUCH) && hit >= 0) {
        if (UpdatePromptShown())
            PromptTap(hit);
#if MANIA_DEV_TOOLS
        else if (debugTab)
            kButtons[hit].tap();
#endif
        else if (Port::tab == TAB_UPDATES)
            UpdatesTap(hit);
        else
            TapOption(hit);
        SaveAll();
        sDirty = true;
    }
    if (hit != sPressed) {
        sPressed = hit;
        sDirty   = true;
    }
    (void)debugTab;
    const u64 now = svcGetSystemTick();
    // Quicker while something moves (an install, a download, the restart countdown), so the bar moves.
    bool busy = UpdatesBusy();
#if MANIA_DEV_TOOLS
    float p;
    unsigned secs;
    char t[8];
    busy = busy || DevTools::InstallNotice(t, sizeof(t), &p, &secs) != 0;
#endif
    if (sDirty || UpdatesChanged() || now - sLastDraw > (busy ? SYSCLOCK_ARM11 / 10 : SYSCLOCK_ARM11 / 4)) {
        Draw();
        sLastDraw = now;
        sDirty    = false;
    }
}

#endif
