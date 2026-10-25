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
#include "RSDK/Dev/CTR/PortText.hpp"
#include <time.h>
#include <math.h>

using namespace RSDK;
using Port::Tr;

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

// Bottom-screen colour themes (OPTIONS > THEME, Port::theme, saved by position: append new ones; on 2026-10-24, before
// any release had themes, the order changed once). MANIA (the default) mixes the four colours of the game's menus, a
// different one for each tab and cell in turn (`multi`, kMania), over the navy of the menus with their diagonal stripes,
// on filled panels with a cut corner (`cut`); MANIA OUTLINE is the same in framed boxes; CLASSIC is the first look; the
// others are one colour of the game each: Sonic's blue, Knuckles' red, Super Sonic's gold, Green Hill's green.
struct Theme {
    const char *name;
    bool multi, cut; // cut: filled panels with the top-right corner cut (Panel)
    u32 bg, barLine, tabOnEdge, tabOnFill, tabOffEdge, tabOffFill, tabIcon;
    u32 cellEdge, cellFill, cellLine, pressEdge, pressFill, boxEdge, boxFill, noteEdge, noteFill, popEdge, popFill;
    u32 text, label, dim, footer, accent, clock, battery, loadBg;
};
static inline u32 Hex(u32 rgb) { return C2D_Color32(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF, 255); }
#define THEME(n, mu, cu, a, b, c, d, e, f, g, h, i, j, k, l, m, o, p, q, r, s, t, u, v, w, x, y, z, aa)                                          \
    { n, mu, cu, Hex(a), Hex(b), Hex(c), Hex(d), Hex(e), Hex(f), Hex(g), Hex(h), Hex(i), Hex(j), Hex(k), Hex(l), Hex(m),                              \
      Hex(o), Hex(p), Hex(q), Hex(r), Hex(s), Hex(t), Hex(u), Hex(v), Hex(w), Hex(x), Hex(y), Hex(z), Hex(aa) }
static const Theme kThemes[] = {
    //     name          multi  cut    bg        barLine   tabOnE    tabOnF    tabOffE   tabOffF   tabIcon   cellE     cellF     cellLine  pressE
    //                    pressF    boxE      boxF      noteE     noteF     popE      popF      text      label     dim       footer
    //                    accent    clock     battery   loadBg
    THEME("MANIA",       true, true, 0x0A1438, 0x2A6CF0, 0x2A6CF0, 0x1A3C8C, 0x2A3C78, 0x101C48, 0xB4C0E6, 0x2A6CF0, 0x12245C, 0x7AA8FF, 0xFFFFFF,
                                0x3C78F0, 0xF8C820, 0x0E1A48, 0x2A6CF0, 0x070E28, 0xF8C820, 0x12245C, 0xFFFFFF, 0xE6ECFF, 0xC8D2F0, 0x8CA0D8,
                                0xFFE45C, 0xE0E8FF, 0x9AAAD8, 0x0A1438),
    THEME("MANIA OUTLINE", true, false, 0x0A1438, 0x2A6CF0, 0x2A6CF0, 0x1A3C8C, 0x2A3C78, 0x101C48, 0x9AAAD8, 0x2A6CF0, 0x12245C, 0x7AA8FF, 0xFFFFFF,
                                0x3C78F0, 0xF8C820, 0x0E1A48, 0x2A6CF0, 0x070E28, 0xF8C820, 0x12245C, 0xF0F4FF, 0xC0CCF0, 0x9AAAD8, 0x7088C8,
                                0xF8C820, 0xE0E8FF, 0x9AAAD8, 0x0A1438),
    THEME("CLASSIC",     false, false,0x0E1218, 0x2C3440, 0x2D96F0, 0x124682, 0x32384B, 0x1A1E2A, 0x8C96AF, 0x325082, 0x182032, 0x5A82B4, 0xD2EBFF,
                         0x5A96DC, 0x3A4268, 0x181C30, 0x284678, 0x0A0E18, 0x5A78AA, 0x18202E, 0xE6E8F0, 0xAAB2C8, 0x8C94AA, 0x5A7391,
                         0xFFD700, 0xD2DCEB, 0x96A0B4, 0x0E101C),
    THEME("SONIC",       false, false,0x0A1440, 0x2850B4, 0x3CA0FF, 0x1450B4, 0x28407C, 0x101E4C, 0x8CA0D2, 0x2E64D2, 0x10225C, 0x6EAAFF, 0xD2EBFF,
                         0x468CF0, 0x2C50A8, 0x0E1C50, 0x2C50A8, 0x08102E, 0x6EAAFF, 0x10225C, 0xEBF0FF, 0xB4C3EB, 0x96AAD7, 0x6482C8,
                         0xFFD700, 0xD2DCF5, 0x96AAD7, 0x0A1440),
    THEME("KNUCKLES",    false, false,0x2C080C, 0x8C1E28, 0xF04646, 0xA0141E, 0x6E282D, 0x3C0E14, 0xD29696, 0xBE2D37, 0x4A0E16, 0xF06E6E, 0xFFDCDC,
                         0xDC3C46, 0x962832, 0x3C0C12, 0x962832, 0x1E0608, 0xF06E6E, 0x4A0E16, 0xFAEBEB, 0xE6BEBE, 0xCD9696, 0xBE6E6E,
                         0xFFD700, 0xF5DCDC, 0xCD9696, 0x2C080C),
    THEME("SUPER SONIC", false, false,0x2A2004, 0xA08014, 0xFFDC3C, 0xB4880A, 0x786020, 0x3E2E08, 0xDCC88C, 0xDCAF1E, 0x4A3808, 0xFFE16E, 0xFFFAD2,
                         0xEBBE28, 0xAA8214, 0x3C2C06, 0xAA8214, 0x1C1402, 0xFFE16E, 0x4A3808, 0xFFFAE6, 0xF0E1B4, 0xD7C38C, 0xC8AA5A,
                         0xFFFFFF, 0xFFF5D2, 0xD7C38C, 0x2A2004),
    THEME("GREEN HILL",  false, false,0x082A10, 0x1E8C32, 0x5ADC64, 0x1E8C32, 0x2D6437, 0x0E3216, 0x96C8A0, 0x3CAA46, 0x103C1A, 0x82E68C, 0xDCFFDC,
                         0x46BE50, 0x328C3C, 0x0C3214, 0x328C3C, 0x06180A, 0x82E68C, 0x103C1A, 0xEBFAEB, 0xBEE6C3, 0x96C89B, 0x6EAA78,
                         0xFFD700, 0xDCF5DC, 0x96C89B, 0x082A10),
};
#undef THEME
enum { THEME_COUNT = sizeof(kThemes) / sizeof(kThemes[0]) };
static const Theme &T() { return kThemes[Port::theme >= 0 && Port::theme < THEME_COUNT ? Port::theme : 0]; }

// MANIA's four colours, in the order tabs and cells take them: blue, red, yellow, green.
static const u32 kMania[4] = { 0x2A6CF0, 0xE8333C, 0xF8C820, 0x34B24A };
static u32 Mix(u32 a, u32 b, float t) // 0xRRGGBB, t of the way from a to b
{
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8)
        out |= (u32)(((a >> sh) & 0xFF) * (1 - t) + ((b >> sh) & 0xFF) * t) << sh;
    return out;
}
// Cell `i`'s colours: its own under MANIA, the theme's otherwise.
static u32 CellEdge(int i) { return T().multi ? Hex(kMania[i & 3]) : T().cellEdge; }
static u32 CellFill(int i) { return T().multi ? Hex(Mix(kMania[i & 3], 0x0A1438, 0.8f)) : T().cellFill; }
static u32 CellLine(int i) { return T().multi ? Hex(Mix(kMania[i & 3], 0xFFFFFF, 0.4f)) : T().cellLine; }

// A C2D colour t of the way to another
static u32 MixC(u32 a, u32 b, float t)
{
    u32 out = 0xFF000000;
    for (int sh = 0; sh <= 16; sh += 8)
        out |= (u32)(((a >> sh) & 0xFF) * (1 - t) + ((b >> sh) & 0xFF) * t) << sh;
    return out;
}

// The shape of a cut panel: a rectangle without its top-right corner (c pixels along each side), where the title of a
// box seldom reaches.
static void CutShape(float x, float y, float w, float h, float c, u32 col, float z)
{
    C2D_DrawRectSolid(x, y, z, w - c, h, col);
    C2D_DrawRectSolid(x + w - c, y + c, z, c, h - c, col);
    C2D_DrawTriangle(x + w - c, y, col, x + w - c, y + c, col, x + w, y + c, col, z);
}

// Every box of the screen: framed (an edge, the fill `inset` pixels in), or under a cut theme filled with a colour
// between its edge and fill colours, its top-right corner cut and a shadow under it, after the game's notices.
static bool sNoShadow; // a box redrawn over itself (DrawInTopFrame): its shadow is there already, and a second one darkens

static void Panel(float x, float y, float z, float w, float h, u32 edge, u32 fill, int inset)
{
    if (!T().cut) {
        C2D_DrawRectSolid(x, y, z, w, h, edge);
        C2D_DrawRectSolid(x + inset, y + inset, z + 0.01f, w - 2 * inset, h - 2 * inset, fill);
        return;
    }
    const float c = h * 0.4f < 10 ? h * 0.4f : 10;
    const u32 solid = MixC(edge, fill, inset > 1 ? 0.75f : 0.45f); // the big boxes (inset 2: prompts, details) darker, for text
    if (!sNoShadow)
        CutShape(x + 2, y + 2, w, h, c, C2D_Color32(0, 0, 0, 110), z - 0.005f);
    CutShape(x, y, w, h, c, solid, z);
    C2D_DrawRectSolid(x, y, z + 0.005f, w - c, 1, MixC(solid, 0xFFFFFFFF, 0.45f)); // a light top edge
}

// MANIA's background: the diagonal stripes of the game's menus, a shade lighter than the navy.
static void DrawBackdrop(float top)
{
    if (!T().multi) // the MANIA themes
        return;
    const u32 c = Hex(Mix(0x0A1438, 0x2A6CF0, 0.14f));
    const float h = 240 - top;
    for (float k = -h; k < 320; k += 44)
    { // one band at 45 degrees, two triangles
        C2D_DrawTriangle(k, 240, c, k + 20, 240, c, k + h, top, c, 0.05f);
        C2D_DrawTriangle(k + 20, 240, c, k + h + 20, top, c, k + h, top, c, 0.05f);
    }
}

// Text that ends with "..." where it would run past `maxW` pixels: measured once, cut to the share of it that fits (on a
// UTF-8 character boundary), measured again until it fits.
static void TextFit(const char *s, float x, float y, float scale, u32 color, float maxW, float z)
{
    C2D_Text t;
    C2D_TextParse(&t, sTextBuf, s);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    if (w <= maxW) {
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
        return;
    }
    char cut[192];
    size_t len = strlen(s) < sizeof(cut) - 4 ? strlen(s) : sizeof(cut) - 4;
    for (int tries = 0; tries < 4 && len > 0; ++tries) {
        len = (size_t)(len * (maxW / w)) - 1; // its share, a little less for the dots
        if ((int)len < 1)
            len = 1;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) // not inside a multi-byte character
            --len;
        memcpy(cut, s, len);
        strcpy(cut + len, "...");
        C2D_TextParse(&t, sTextBuf, cut);
        C2D_TextGetDimensions(&t, scale, scale, &w, &h);
        if (w <= maxW)
            break;
    }
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
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
static void ValFtp(char *o, unsigned c, bool *on)
{
    snprintf(o, c, "%s", DevTools::FtpRunning() ? "ON" : DevTools::swFtp ? "FAILED" : "OFF");
    *on = DevTools::FtpRunning();
}
// OFF -> ON; ON -> OFF; a failed start (FAILED) -> tries again
static void TapFtp() { DevTools::SetFtp(!DevTools::FtpRunning()); }

static const Button kButtons[] = {
    { "RENDERER", TapRenderer, ValRenderer }, { "FRAME SKIP", TapFrameSkip, ValFrameSkip }, { "MAX SKIP", TapMaxSkip, ValMaxSkip },
    { "LOG MARK", TapMark, ValMark },         { "DRAW LISTS", TapDraw, ValDraw },            { "FLIP COPY", TapFlip, ValFlip },
    { "3D SCENES", TapGpu3D, ValGpu3D },      { "FORCE 3D", TapForce3D, ValForce3D },    { "DEPTH TINT", TapDepthTint, ValDepthTint },
    { "CAPTURE", TapCapture, ValCapture },    { "FTP", TapFtp, ValFtp },
};

#endif // MANIA_DEV_TOOLS

// Tabs, in the bar at the top (icons as in the owner's other ports). Values are saved: append only. kTabs is the
// order they show in; DEBUG only with the dev tools.
enum Tab { TAB_GAME, TAB_OPTIONS, TAB_DEBUG, TAB_UPDATES, TAB_ACH };
#if MANIA_DEV_TOOLS
static const int kTabs[] = { TAB_GAME, TAB_ACH, TAB_OPTIONS, TAB_UPDATES, TAB_DEBUG };
#else
static const int kTabs[] = { TAB_GAME, TAB_ACH, TAB_OPTIONS, TAB_UPDATES };
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
        case TAB_ACH: // a cup: bowl, handles, stem, base
            C2D_DrawRectSolid(cx - 4, cy - 7, z, 9, 7, col);
            C2D_DrawRectSolid(cx - 3, cy, z, 7, 1, col);
            C2D_DrawRectSolid(cx - 7, cy - 6, z, 2, 4, col);
            C2D_DrawRectSolid(cx + 6, cy - 6, z, 2, 4, col);
            C2D_DrawRectSolid(cx - 1, cy + 1, z, 3, 3, col);
            C2D_DrawRectSolid(cx - 4, cy + 4, z, 9, 2, col);
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
    C2D_DrawRectSolid(0, 0, 0.7f, W, HEADER_H, T().bg);
    if (T().multi) // the four colours in a band under the tabs
        for (int i = 0; i < 4; ++i)
            C2D_DrawRectSolid(i * W / 4.0f, HEADER_H - 2, 0.71f, W / 4.0f, 2, Hex(kMania[i]));
    else
        C2D_DrawRectSolid(0, HEADER_H - 1, 0.71f, W, 1, T().barLine);
    for (int i = 0; i < TAB_SHOWN; ++i) {
        float x, y;
        TabRect(i, &x, &y);
        const bool on = kTabs[i] == Port::tab;
        u32 edge = on ? T().tabOnEdge : T().tabOffEdge, fill = on ? T().tabOnFill : T().tabOffFill;
        if (T().multi) { // each tab its colour; the closed ones dimmed
            edge = Hex(on ? kMania[i & 3] : Mix(kMania[i & 3], 0x0A1438, 0.55f));
            fill = Hex(Mix(kMania[i & 3], 0x0A1438, on ? 0.45f : 0.85f));
        }
        Panel(x, y, 0.72f, TAB_W, TAB_H, edge, fill, 1);
        DrawTabIcon(kTabs[i], x + TAB_W / 2.0f, y + TAB_H / 2.0f, on ? Rgb(255, 255, 255) : T().tabIcon, 0.74f);
    }

    // Battery, then the clock, at the right end
    u8 level = 5, charging = 0;
    PTMU_GetBatteryLevel(&level);
    PTMU_GetBatteryChargeState(&charging);
    const float bx = W - 22, by = 9;
    C2D_DrawRectSolid(bx, by, 0.72f, 15, 8, T().battery);
    C2D_DrawRectSolid(bx + 15, by + 2, 0.72f, 2, 4, T().battery);
    C2D_DrawRectSolid(bx + 1, by + 1, 0.73f, 13, 6, T().bg);
    const u32 fill = charging ? Rgb(100, 220, 255) : level <= 1 ? Rgb(240, 100, 100) : level == 2 ? Rgb(240, 200, 80) : Rgb(110, 220, 130);
    C2D_DrawRectSolid(bx + 2, by + 2, 0.74f, 11.0f * (level > 5 ? 5 : level) / 5, 4, fill);
    char clock[8];
    const time_t now = time(NULL);
    const struct tm *tm = gmtime(&now); // the 3DS clock is local time already
    snprintf(clock, sizeof(clock), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    // Wi-Fi: three rising bars on the battery's baseline (as ../zelda-tmc-3ds), lit up to the signal's strength (0-3)
    const int wifi  = osGetWifiStrength();
    const float wx  = bx - 16;
    const u32 faint = MixC(T().battery, T().bg, 0.65f);
    for (int i = 0; i < 3; ++i) {
        const float h = 3 + i * 2;
        C2D_DrawRectSolid(wx + i * 4, by + 8 - h, 0.72f, 2, h, i < wifi ? T().battery : faint);
    }
    Text(clock, wx - 5, 6, 0.5f, T().clock, C2D_AlignRight, 0.75f);
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

// ---------------------------------------------------------------- the game's HUD art

enum { ITEM_TYPES = 18 }; // ITEMBOX_RING .. ITEMBOX_STOCK (Plus)
// The GAME tab draws TIME, SCORE, RINGS and LIVES with the game's own HUD sprites, as the top screen shows them:
// Global/HUD.bin (the labels SCORE / TIME / RINGS and their red flashing frames, the time's marks, the digits, the "x"
// and the characters' heads) and a ring from Global/Ring.bin. Those sheets are stage-scoped, 8-bit and drawn through
// the palette: the first time a stage has them loaded, the frames are copied through the palette into a texture of
// ours (RGBA5551, 256x128, 64 KB), which then outlives the stage. The item boxes' icons (Global/ItemBox.bin) come with
// them, for the row of picked items. Until then the boxes use the system font.

enum ArtId {
    ART_SCORE, ART_TIME, ART_TIME_RED, ART_RINGS, ART_RINGS_RED, ART_MARKS, ART_X, ART_DIGIT,
    ART_LIFE = ART_DIGIT + 10, // Sonic, Tails, Knuckles, Mighty, Ray (HUD_CharacterIndexFromID)
    ART_RING = ART_LIFE + 5,
    ART_ITEM, // the item boxes' contents icons, by ITEMBOX_* (ItemBox.bin animation 2)
    ART_COUNT = ART_ITEM + ITEM_TYPES,
};
enum { ART_W = 256, ART_H = 128 };
struct ArtFrame {
    Tex3DS_SubTexture sub;
    C2D_Image img;
    int16 w, h, px, py;
};
static ArtFrame sArt[ART_COUNT];
static C3D_Tex sArtTex;
static bool sArtReady, sArtFailed;

static const SpriteFrame *FindFrame(const char *file, int anim, int frame)
{
    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(file, hash);
    for (int32 i = 0; i < SPRFILE_COUNT; ++i) {
        const SpriteAnimation &sa = spriteAnimationList[i];
        if (sa.scope == SCOPE_NONE || !HASH_MATCH_MD5(sa.hash, hash))
            continue;
        if (!sa.animations || !sa.frames || anim >= sa.animCount || frame >= sa.animations[anim].frameCount)
            return NULL;
        const SpriteFrame *f = &sa.frames[sa.animations[anim].frameListOffset + frame];
        const GFXSurface &surf = gfxSurface[f->sheetID];
        if (!surf.pixels || f->sprX + f->width > surf.width || f->sprY + f->height > surf.height || f->width <= 0 || f->height <= 0)
            return NULL;
        return f;
    }
    return NULL;
}

// Once, when the sheets are loaded: false until then (and for good if the texture cannot be made). `pal`: the colours
// to copy through (default: the top lines' bank of the scene on screen).
static bool LoadArt(const uint16 *pal = NULL)
{
    if (sArtReady || sArtFailed)
        return sArtReady;
    struct Src {
        const char *file;
        int anim, frame;
    };
    Src src[ART_COUNT];
    static const int kElements[ART_DIGIT] = { 0, 1, 2, 3, 4, 12, 14 }; // HUD.bin animation 0 (HUD_Draw)
    for (int i = 0; i < ART_DIGIT; ++i)
        src[i] = { "Global/HUD.bin", 0, kElements[i] };
    for (int i = 0; i < 10; ++i)
        src[ART_DIGIT + i] = { "Global/HUD.bin", 1, i };
    for (int i = 0; i < 5; ++i)
        src[ART_LIFE + i] = { "Global/HUD.bin", 2, i };
    src[ART_RING] = { "Global/Ring.bin", 0, 0 };
    for (int i = 0; i < ITEM_TYPES; ++i)
        src[ART_ITEM + i] = { "Global/ItemBox.bin", 2, i };
    const SpriteFrame *frames[ART_COUNT];
    for (int i = 0; i < ART_COUNT; ++i)
        if (!(frames[i] = FindFrame(src[i].file, src[i].anim, src[i].frame)) && i < ART_ITEM)
            return false; // not in this scene (menus, the title): next time
    if (!frames[ART_ITEM])
        return false; // a stage without item boxes loaded: wait for one that has them

    // Shelves, left to right, a pixel apart
    int sx = 0, sy = 0, shelfH = 0, at[ART_COUNT][2];
    for (int i = 0; i < ART_COUNT; ++i) {
        if (!frames[i]) // an item icon this version lacks
            continue;
        const int w = frames[i]->width, h = frames[i]->height;
        if (sx + w > ART_W)
            sx = 0, sy += shelfH + 1, shelfH = 0;
        if (w > ART_W || sy + h > ART_H) {
            sArtFailed = true;
            DEV_LOG("hud art: does not fit %dx%d", ART_W, ART_H);
            return false;
        }
        at[i][0] = sx, at[i][1] = sy;
        sx += w + 1;
        shelfH = h > shelfH ? h : shelfH;
    }
    if (!C3D_TexInit(&sArtTex, ART_W, ART_H, GPU_RGBA5551)) {
        sArtFailed = true;
        return false;
    }
    u16 *tex = (u16 *)sArtTex.data;
    memset(tex, 0, ART_W * ART_H * 2);
    if (!pal)
        pal = fullPalette[gfxLineBuffer[0]]; // the top lines' bank: the HUD's colours
    for (int i = 0; i < ART_COUNT; ++i) {
        if (!frames[i])
            continue;
        const SpriteFrame &f    = *frames[i];
        const GFXSurface &surf  = gfxSurface[f.sheetID];
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                const uint8 index = surf.pixels[(f.sprY + y) * surf.width + f.sprX + x];
                if (!index)
                    continue;
                const uint16 c = pal[index]; // RGB565
                // 8x8 tiles, Morton order inside
                const u32 tx = at[i][0] + x, ty = at[i][1] + y;
                const u32 o  = (((ty >> 3) * (ART_W >> 3) + (tx >> 3)) << 6) | (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2)
                              | ((tx & 4) << 2) | ((ty & 4) << 3);
                tex[o] = (u16)(((c >> 11) << 11) | (((c >> 6) & 31) << 6) | ((c & 31) << 1) | 1);
            }
        ArtFrame &a = sArt[i];
        a.w = f.width, a.h = f.height, a.px = f.pivotX, a.py = f.pivotY;
        a.sub     = { (u16)f.width, (u16)f.height, at[i][0] / (float)ART_W, 1 - at[i][1] / (float)ART_H,
                      (at[i][0] + f.width) / (float)ART_W, 1 - (at[i][1] + f.height) / (float)ART_H };
        a.img.tex    = &sArtTex;
        a.img.subtex = &a.sub;
    }
    unsigned lit = 0;
    for (int i = 0; i < ART_W * ART_H; ++i)
        lit += tex[i] != 0;
    DEV_LOG("hud art: %u texels, %d rows used, digit %dx%d pivot %d,%d, head %dx%d, ring %dx%d", lit, sy + shelfH, sArt[ART_DIGIT].w, sArt[ART_DIGIT].h,
            sArt[ART_DIGIT].px, sArt[ART_DIGIT].py, sArt[ART_LIFE].w, sArt[ART_LIFE].h, sArt[ART_RING].w, sArt[ART_RING].h);
    C3D_TexFlush(&sArtTex);
    C3D_TexSetFilter(&sArtTex, GPU_NEAREST, GPU_NEAREST);
    sArtReady = true;
    return true;
}

// The colours a sprite file's first sheet was drawn with: the global colour table of its GIF (RGB565 out). Its frames
// index that table as they index the game's palette, and unlike the palette it does not depend on the scene (the
// menus' sprites use rows the game config does not hold).
static bool SheetPalette(const char *bin, uint16 *pal)
{
    char path[0x100], sheet[0x100];
    snprintf(path, sizeof(path), "Data/Sprites/%s", bin);
    FileInfo info;
    InitFileInfo(&info);
    if (!LoadFile(&info, path, FMODE_RB))
        return false;
    bool ok = ReadInt32(&info, false) == RSDK_SIGNATURE_SPR;
    if (ok) {
        ReadInt32(&info, false); // frame count
        ok = ReadInt8(&info) > 0;
        if (ok)
            ReadString(&info, sheet);
    }
    CloseFile(&info);
    if (!ok)
        return false;
    snprintf(path, sizeof(path), "Data/Sprites/%s", sheet);
    InitFileInfo(&info);
    if (!LoadFile(&info, path, FMODE_RB))
        return false;
    uint8 head[13], rgb[256 * 3];
    ReadBytes(&info, head, sizeof(head));
    const int n = 2 << (head[10] & 7);
    ok = !memcmp(head, "GIF", 3) && (head[10] & 0x80);
    if (ok)
        ReadBytes(&info, rgb, n * 3);
    CloseFile(&info);
    if (!ok)
        return false;
    for (int i = 0; i < 256; ++i)
        pal[i] = i < n ? (uint16)(((rgb[i * 3] >> 3) << 11) | ((rgb[i * 3 + 1] >> 2) << 5) | (rgb[i * 3 + 2] >> 3)) : 0;
    return true;
}

// The chaos emeralds as the save select draws them (UI/SaveSelect.bin animation 4: the seven, then the empty slot),
// copied at boot through their sheet's colours into a texture of ours; the big menu sheet itself goes with the first
// scene (stage scope).
enum { EMERALD_FRAMES = 8, EM_TEX_W = 128, EM_TEX_H = 32 };
static ArtFrame sEmerald[EMERALD_FRAMES];
static C3D_Tex sEmeraldTex;
static bool sEmeraldReady;

static void LoadEmeraldArt()
{
    const uint16 id = LoadSpriteAnimation("UI/SaveSelect.bin", SCOPE_STAGE);
    static uint16 pal[256];
    if (id >= SPRFILE_COUNT || !SheetPalette("UI/SaveSelect.bin", pal))
        return;
    const SpriteAnimation &sa = spriteAnimationList[id];
    if (!sa.animations || !sa.frames || sa.animCount <= 4 || sa.animations[4].frameCount < EMERALD_FRAMES)
        return;
    if (!C3D_TexInit(&sEmeraldTex, EM_TEX_W, EM_TEX_H, GPU_RGBA5551))
        return;
    u16 *tex = (u16 *)sEmeraldTex.data;
    memset(tex, 0, EM_TEX_W * EM_TEX_H * 2);
    int sx = 0;
    for (int i = 0; i < EMERALD_FRAMES; ++i) {
        const SpriteFrame &f   = sa.frames[sa.animations[4].frameListOffset + i];
        const GFXSurface &surf = gfxSurface[f.sheetID];
        if (!surf.pixels || f.width <= 0 || f.height <= 0 || f.height > EM_TEX_H || sx + f.width > EM_TEX_W ||
            f.sprX + f.width > surf.width || f.sprY + f.height > surf.height) {
            C3D_TexDelete(&sEmeraldTex);
            return;
        }
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                const uint8 index = surf.pixels[(f.sprY + y) * surf.width + f.sprX + x];
                if (!index)
                    continue;
                const uint16 c = pal[index];
                const u32 tx = sx + x, ty = y;
                const u32 o  = (((ty >> 3) * (EM_TEX_W >> 3) + (tx >> 3)) << 6) | (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2)
                              | ((tx & 4) << 2) | ((ty & 4) << 3);
                tex[o] = (u16)(((c >> 11) << 11) | (((c >> 6) & 31) << 6) | ((c & 31) << 1) | 1);
            }
        ArtFrame &a  = sEmerald[i];
        a.w = f.width, a.h = f.height, a.px = f.pivotX, a.py = f.pivotY;
        a.sub        = { (u16)f.width, (u16)f.height, sx / (float)EM_TEX_W, 1, (sx + f.width) / (float)EM_TEX_W, 1 - f.height / (float)EM_TEX_H };
        a.img.tex    = &sEmeraldTex;
        a.img.subtex = &a.sub;
        sx += f.width + 1;
    }
    C3D_TexFlush(&sEmeraldTex);
    C3D_TexSetFilter(&sEmeraldTex, GPU_NEAREST, GPU_NEAREST);
    sEmeraldReady = true;
}

void BottomUi::LoadArtAtBoot()
{
    // Once the game config (and with it the global palette rows the HUD uses) is loaded and before the first scene:
    // the three sprite files, kept for the whole game (global scope; the stages that load them get the same entry),
    // so the GAME tab has the game's art on the title screen and in the menus too. A few hundred KB of the STG pool.
    LoadSpriteAnimation("Global/HUD.bin", SCOPE_GLOBAL);
    LoadSpriteAnimation("Global/Ring.bin", SCOPE_GLOBAL);
    LoadSpriteAnimation("Global/ItemBox.bin", SCOPE_GLOBAL);
    LoadArt(globalPalette[0]); // no scene has set the palette yet: the game config's global rows, where the HUD's are
    LoadEmeraldArt();
    DEV_LOG("hud art at boot: %s, emeralds %s", sArtReady ? "ready" : "NOT READY", sEmeraldReady ? "ready" : "NOT READY");
}

// Frame `id` drawn as the game does at (x, y): its pivot from there, `s` times its size.
static void DrawArt(int id, float x, float y, float s, float z)
{
    const ArtFrame &a = sArt[id];
    C2D_DrawImageAt(a.img, x + a.px * s, y + a.py * s, z, NULL, s, s);
}

// HUD_DrawNumbersBase10: the digits from the right, 8 px apart, the last one at (x, y); `digits` 0: as many as needed.
static void DrawNumber(int value, int digits, float x, float y, float s, float z)
{
    if (value < 0)
        value = 0;
    if (!digits)
        for (int v = value; v > 0 || !digits; v /= 10)
            ++digits;
    for (int d = 1; digits--; d *= 10, x -= 8 * s)
        DrawArt(ART_DIGIT + value / d % 10, x, y, s, z);
}

// Where a number's last digit goes for its right edge at `right` and its bottom at `bottom`.
static void NumberAnchor(float right, float bottom, float s, float *x, float *y)
{
    const ArtFrame &d = sArt[ART_DIGIT];
    *x = right - (d.px + d.w) * s;
    *y = bottom - (d.py + d.h) * s;
}

// ---- the row of picked item boxes (under the emeralds)
// Each box opened shows its icon for ITEM_SHOW_MS; a power that lasts (a shield, invincibility, speed shoes, the hyper
// ring, super) keeps it as long as the player has it. Oldest on the left; a full row drops its oldest.
enum { ITEM_ROW = 8, ITEM_SHOW_MS = 2000 };
enum { IT_RING, IT_BLUE, IT_BUBBLE, IT_FIRE, IT_LIGHTNING, IT_INVINCIBLE, IT_SNEAKERS, IT_HYPERRING = 11, IT_SUPER = 14 }; // ITEMBOX_*
static struct {
    int type;
    u64 since; // osGetTime()
} sItems[ITEM_ROW];
static int sItemCount;
static bool sItemsReset = true; // after a scene load: the boxes already open are not news

static bool ItemIsShield(int t) { return t >= IT_BLUE && t <= IT_LIGHTNING; }
static bool ItemLasts(int t) { return ItemIsShield(t) || t == IT_INVINCIBLE || t == IT_SNEAKERS || t == IT_HYPERRING || t == IT_SUPER; }

static void ItemRemove(int i)
{
    for (; i + 1 < sItemCount; ++i)
        sItems[i] = sItems[i + 1];
    --sItemCount;
}

// Every frame: the boxes opened since the last frame join the row (a lasting power replaces its older icon).
static void PollItems()
{
    int got[4];
    const int n   = PortGame_PollItems(got, 4, sItemsReset);
    sItemsReset   = false;
    for (int k = 0; k < n; ++k) {
        const int t = got[k];
        DEV_LOG("item box: %d", t);
        if (t < 0 || t >= ITEM_TYPES)
            continue;
        if (ItemLasts(t))
            for (int i = sItemCount - 1; i >= 0; --i)
                if (sItems[i].type == t || (ItemIsShield(t) && ItemIsShield(sItems[i].type)))
                    ItemRemove(i);
        if (sItemCount == ITEM_ROW)
            ItemRemove(0);
        sItems[sItemCount].type    = t;
        sItems[sItemCount++].since = osGetTime();
        sDirty                     = true;
    }
}

// Drops the icons whose time is over: ITEM_SHOW_MS for all, longer for a power the player still has.
static void ExpireItems(const PortGameInfo &gi)
{
    const u64 now = osGetTime();
    for (int i = sItemCount - 1; i >= 0; --i) {
        const int t     = sItems[i].type;
        bool active     = false;
        if (ItemIsShield(t))
            active = gi.shield == t; // SHIELD_BLUE..SHIELD_LIGHTNING follow ITEMBOX_BLUESHIELD..LIGHTNINGSHIELD
        else if (t == IT_INVINCIBLE)
            active = gi.invincible > 0;
        else if (t == IT_SNEAKERS)
            active = gi.speedShoes > 0;
        else if (t == IT_HYPERRING)
            active = gi.hyperRing;
        else if (t == IT_SUPER)
            active = gi.super;
        if (!gi.playerValid || (!active && now - sItems[i].since >= ITEM_SHOW_MS))
            ItemRemove(i);
    }
}

static bool Flash() { return (svcGetSystemTick() / (SYSCLOCK_ARM11 * 8 / 60)) & 1; } // the HUD's 8-frame blink

// A stat box's frame, in its colour (MANIA: tint i).
static void BoxFrame(int i, float x, float y, float w, float h)
{
    Panel(x, y, 0.3f, w, h, T().multi ? CellEdge(i) : T().boxEdge, T().multi ? CellFill(i) : T().boxFill, 1);
}

// The game's label sprite at the box's top left (1x).
static void BoxLabel(int id, float x, float y) { DrawArt(id, x + 7 - sArt[id].px, y + 5 - sArt[id].py, 1, 0.33f); }

enum { ART_SCALE = 2 };

// TIME as the HUD lays it out (HUD_Draw): the marks at +52,-2 from the label, the hundredths' last digit at +97,+14,
// the seconds 9 px left of the hundredths, the minutes 9 px left of the seconds.
static void DrawTimeBox(int i, float x, float y, float w, float h, bool inStage)
{
    BoxFrame(i, x, y, w, h);
    BoxLabel(sceneInfo.minutes == 9 && Flash() ? ART_TIME_RED : ART_TIME, x, y);
    if (!inStage)
        return;
    const float s = ART_SCALE;
    float ox, oy; // the HUD's time position
    NumberAnchor(x + w - 8, y + h - 5, s, &ox, &oy);
    ox -= 97 * s, oy -= 14 * s;
    DrawArt(ART_MARKS, ox + 52 * s, oy - 2 * s, s, 0.33f);
    DrawNumber(sceneInfo.milliseconds, 2, ox + 97 * s, oy + 14 * s, s, 0.33f);
    DrawNumber(sceneInfo.seconds, 2, ox + (97 - 25) * s, oy + 14 * s, s, 0.33f);
    DrawNumber(sceneInfo.minutes, sceneInfo.minutes > 9 ? 2 : 1, ox + (97 - 50) * s, oy + 14 * s, s, 0.33f);
}

static struct {
    float x, y, w, h;
    bool shown;
    int8 m, sec, ms;
} sTimeBox; // the GAME tab's TIME box as last drawn, for DrawInTopFrame

// RINGS: a ring, the label (red every other 8 frames at none, as the HUD), the count.
static void DrawRingsBox(float x, float y, float w, float h, bool inStage, int rings)
{
    const float s = ART_SCALE;
    float nx, ny;
    BoxFrame(2, x, y, w, h);
    DrawArt(ART_RING, x + 8 - sArt[ART_RING].px * s, y + (h - sArt[ART_RING].h * s) / 2 - sArt[ART_RING].py * s, s, 0.33f);
    DrawArt(inStage && !rings && Flash() ? ART_RINGS_RED : ART_RINGS, x + 50 - sArt[ART_RINGS].px, y + 5 - sArt[ART_RINGS].py, 1, 0.33f);
    NumberAnchor(x + w - 8, y + h - 5, s, &nx, &ny);
    if (inStage)
        DrawNumber(rings, 0, nx, ny, s, 0.33f);
}

static struct {
    float x, y, w, h;
    bool shown, flash;
} sRingsBox; // the RINGS box as last drawn: at none its label blinks faster than the full redraws, DrawInTopFrame keeps it

static void StatBox(int i, float x, float y, float w, float h, const char *label, const char *value, u32 valueCol)
{
    Panel(x, y, 0.3f, w, h, T().multi ? CellEdge(i) : T().boxEdge, T().multi ? CellFill(i) : T().boxFill, 1);
    Text(Tr(label), x + 7, y + 4, 0.42f, T().dim, 0, 0.32f);
    Text(value, x + w - 8, y + 15, 0.85f, valueCol, C2D_AlignRight, 0.32f);
}

static void DrawGameTab()
{
    PortGameInfo gi;
    PortGame_GetInfo(&gi);
    char line[64];
    float y = HEADER_H + 6;

    // Where: the scene's name from the scene list, and the mode
    // Only where one plays (stages, the Special Stages, the bonus games, the extras): the scene lists' categories 1-6;
    // 0 is Presentation (logos, title, menus), 7 the cutscenes and 8 the videos.
    const bool playing = sceneInfo.activeCategory >= 1 && sceneInfo.activeCategory <= 6;
    const char *scene  = sceneInfo.listData && playing ? sceneInfo.listData[sceneInfo.listPos].name : "";
    snprintf(line, sizeof(line), "%s", scene);
    for (char *c = line; *c; ++c)
        *c = (char)toupper((unsigned char)*c);
    Text(line, 10, y, 0.62f, T().accent);
    static const char *const kModes[] = { "MANIA MODE", "ENCORE MODE", "TIME ATTACK", "COMPETITION" };
    Text(gi.gameMode >= 0 && gi.gameMode < 4 ? Tr(kModes[gi.gameMode]) : "", W - 10, y + 3, 0.45f, T().dim, C2D_AlignRight);
    y += 20;

    // Who
    const char *lead = CharName(gi.character), *side = CharName(gi.sidekick);
    if (lead && side)
        snprintf(line, sizeof(line), "%s & %s", lead, side);
    else
        snprintf(line, sizeof(line), "%s", lead ? lead : "-");
    Text(line, 10, y, 0.5f, Rgb(100, 220, 255));
    if (gi.saveSlot >= 0)
        snprintf(line, sizeof(line), "%s %d", Tr("SAVE"), gi.saveSlot + 1);
    else
        snprintf(line, sizeof(line), "%s", Tr("NO SAVE"));
    Text(line, W - 10, y + 1, 0.45f, T().dim, C2D_AlignRight);
    y += 20;

    // Time, rings, score, lives: with the game's HUD art once a stage has loaded it, else in the system font
    const bool inStage = gi.playerValid;
    sTimeBox.shown     = false;
    sRingsBox.shown    = false;
    if (LoadArt()) {
        const float bw = (W - 24) / 2.0f, bh = 50, s = ART_SCALE;
        float nx, ny;
        DrawTimeBox(0, 8, y, bw, bh, inStage);
        sTimeBox = { 8, y, bw, bh, inStage, (int8)sceneInfo.minutes, (int8)sceneInfo.seconds, (int8)sceneInfo.milliseconds };
        const float rx = 16 + bw;
        DrawRingsBox(rx, y, bw, bh, inStage, gi.rings);
        sRingsBox = { rx, y, bw, bh, inStage, Flash() };
        y += bh + 6;
        BoxFrame(1, 8, y, bw, bh);
        BoxLabel(ART_SCORE, 8, y);
        NumberAnchor(8 + bw - 8, y + bh - 5, s, &nx, &ny);
        if (inStage)
            DrawNumber(gi.score, 0, nx, ny, s, 0.33f);
        // LIVES: the head (Sonic's until a character is known) and the "x" as the HUD places them; the count right-aligned
        // like the rings above it
        BoxFrame(3, rx, y, bw, bh);
        int head = -1;
        for (int c = gi.character; c > 0; c >>= 1)
            ++head;
        if (head < 0 || head >= 5)
            head = 0;
        const ArtFrame &hf = sArt[ART_LIFE + head];
        const float lx = rx + 10 - hf.px * s, ly = y + (bh - hf.h * s) / 2 - hf.py * s;
        DrawArt(ART_LIFE + head, lx, ly, s, 0.33f);
        DrawArt(ART_X, lx, ly, s, 0.33f);
        NumberAnchor(rx + bw - 8, y + bh - 5, s, &nx, &ny);
        if (inStage)
            DrawNumber(gi.lives, 0, nx, ny, s, 0.33f);
        y += bh + 8;
    }
    else {
        const float bw = (W - 24) / 2.0f, bh = 40;
        snprintf(line, sizeof(line), "%d'%02d\"%02d", sceneInfo.minutes, sceneInfo.seconds, sceneInfo.milliseconds);
        StatBox(0, 8, y, bw, bh, "TIME", inStage ? line : "-", T().text);
        snprintf(line, sizeof(line), "%d", gi.rings);
        StatBox(2, 16 + bw, y, bw, bh, "RINGS", inStage ? line : "-", inStage && !gi.rings ? Rgb(240, 100, 100) : Rgb(255, 215, 0));
        y += bh + 6;
        snprintf(line, sizeof(line), "%d", gi.score);
        StatBox(1, 8, y, bw, bh, "SCORE", inStage ? line : "-", T().text);
        snprintf(line, sizeof(line), "%d", gi.lives);
        StatBox(3, 16 + bw, y, bw, bh, "LIVES", inStage ? line : "-", Rgb(110, 220, 130));
        y += bh + 8;
    }

    // Chaos emeralds of the save, in the game's order
    Text(Tr("EMERALDS"), 10, y + 3, 0.42f, T().dim);
    static const u32 kEmerald[7] = { Rgb(60, 200, 90),  Rgb(240, 210, 60), Rgb(70, 120, 240), Rgb(170, 90, 220),
                                     Rgb(190, 195, 205), Rgb(80, 220, 230), Rgb(235, 70, 70) };
    for (int i = 0; i < 7; ++i) {
        // Right-aligned with the boxes above (the label is longer in some languages); the save select's icons are 24 px
        // wide at twice their size
        const float ex = W - 8 - 12 - (6 - i) * 26, ey = y + 8;
        const bool got = gi.emeralds > 0 && (gi.emeralds & (1 << i));
        if (sEmeraldReady) { // the save select's: each one, or the empty slot, centred on its place
            const ArtFrame &a = sEmerald[got ? i : 7];
            C2D_DrawImageAt(a.img, ex - a.w, ey - a.h, 0.3f, NULL, 2, 2);
            continue;
        }
        const u32 col = got ? kEmerald[i] : T().tabOffEdge;
        C2D_DrawTriangle(ex, ey - 7, col, ex - 7, ey, col, ex + 7, ey, col, 0.3f);
        C2D_DrawTriangle(ex - 7, ey, col, ex + 7, ey, col, ex, ey + 7, col, 0.3f);
    }
    y += 20;

    // The item boxes picked (PollItems), their icons in a row at twice their size
    ExpireItems(gi);
    if (sArtReady) {
        float x = 10;
        for (int i = 0; i < sItemCount; ++i) {
            const ArtFrame &a = sArt[ART_ITEM + sItems[i].type];
            if (!a.w)
                continue;
            DrawArt(ART_ITEM + sItems[i].type, x - a.px * ART_SCALE, y - a.py * ART_SCALE, ART_SCALE, 0.33f);
            x += a.w * ART_SCALE + 6;
        }
    }
}

// ---------------------------------------------------------------- OPTIONS tab

enum { OPT_FRAMES, OPT_FPS, OPT_HUD, OPT_THEME, OPT_PLUS, OPT_COUNT }; // the renderer is DEBUG's; the language the game's
enum { OPT_W = 150, OPT_H = 30, OPT_PITCH = 34 };
// THEME opens a list over the page (the dropdown); its rows are hits OPT_LIST + i, a touch elsewhere OPT_LIST_OUT.
enum { OPT_LIST = 100, OPT_LIST_OUT = 99, LIST_ROW_H = 22 };
static bool sThemeOpen;

static void OptRect(int i, float *x, float *y)
{
    *x = 8 + (i % 2) * 154;
    *y = HEADER_H + 8 + (i / 2) * OPT_PITCH;
}

static void OptValue(int i, char *o, unsigned cap, u32 *col)
{
    const u32 good = Rgb(110, 220, 130), dim = T().dim, warn = Rgb(240, 200, 80);
    switch (i) {
        case OPT_FRAMES:
            snprintf(o, cap, "%s", Tr(Port::lock30 ? "30 FPS" : Port::frameSkip ? "AUTO" : "OFF"));
            *col = Port::lock30 ? good : Port::frameSkip ? good : warn;
            break;
        case OPT_FPS: {
            static const char *const k[] = { "OFF", "TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT" };
            snprintf(o, cap, "%s", Tr(k[Port::fps]));
            *col = Port::fps ? good : dim;
            break;
        }
        case OPT_HUD: {
            static const char *const k[] = { "SHOWN", "AUTO", "HIDDEN" };
            snprintf(o, cap, "%s", Tr(k[Port::hud]));
            *col = Port::hud == 2 ? warn : good;
            break;
        }
        case OPT_THEME:
            snprintf(o, cap, "%s", Tr(T().name));
            *col = T().accent;
            break;
        default: // OPT_PLUS: a change counts from the next start, until then in the warning colour
            snprintf(o, cap, "%s", Tr(Port::plus ? "ON" : "OFF"));
            *col = Port::plus != Port::PlusApplied() ? warn : Port::plus ? good : dim;
            break;
    }
}

static const char *const kOptName[OPT_COUNT] = { "FRAMES", "SHOW FPS", "TOP HUD", "THEME", "PLUS" };
static const char *const kOptHelp[OPT_COUNT] = {
    "AUTO SKIPS A LATE PICTURE TO KEEP THE SPEED, 30 FPS DRAWS EVERY OTHER ONE, OFF DRAWS THEM ALL (THE GAME SLOWS DOWN IF IT CANNOT KEEP UP)",
    "FRAME RATE ON THE TOP SCREEN: L LOGIC, R RENDER (MS)",
    "AUTO HIDES THE GAME'S HUD WHILE THE GAME TAB SHOWS IT",
    "THE COLOURS OF THIS SCREEN",
    "MIGHTY, RAY AND ENCORE MODE, ONLY IF YOU OWN SONIC MANIA PLUS. IT CHANGES AT THE NEXT START",
};
static int sLastOpt = -1;

// What the option does; FRAMES says what its current mode does.
static const char *OptHelp(int i)
{
    if (i != OPT_FRAMES)
        return kOptHelp[i];
    if (Port::lock30)
        return "DRAWS EVERY OTHER PICTURE; THE GAME STAYS AT FULL SPEED";
    return Port::frameSkip ? "SKIPS A LATE PICTURE SO THE GAME KEEPS ITS SPEED" : "DRAWS EVERY PICTURE; THE GAME MAY RUN SLOW IN SOME PLACES";
}

// The dropdown's box: over the THEME cell, moved up when it would run off the screen.
static void ThemeListRect(float *x, float *y, float *w, float *h)
{
    OptRect(OPT_THEME, x, y);
    *w = 150;
    *h = THEME_COUNT * LIST_ROW_H + 4;
    if (*y + *h > H - 4)
        *y = H - 4 - *h;
}

static int ThemeListHit(int px, int py)
{
    float x, y, w, h;
    ThemeListRect(&x, &y, &w, &h);
    if (px < x || px >= x + w || py < y + 2 || py >= y + h - 2)
        return OPT_LIST_OUT;
    return OPT_LIST + (int)((py - y - 2) / LIST_ROW_H);
}

static void DrawThemeList()
{
    float x, y, w, h;
    ThemeListRect(&x, &y, &w, &h);
    C2D_DrawRectSolid(0, HEADER_H, 0.6f, W, H - HEADER_H, C2D_Color32(0, 0, 0, 120));
    Panel(x - 1, y - 1, 0.61f, w + 2, h + 2, T().popEdge, T().popFill, 1);
    for (int i = 0; i < THEME_COUNT; ++i) {
        const Theme &t = kThemes[i];
        const float ry = y + 2 + i * LIST_ROW_H;
        const bool current = i == Port::theme, pressed = sPressed == OPT_LIST + i;
        if (pressed || current)
            C2D_DrawRectSolid(x + 2, ry, 0.63f, w - 4, LIST_ROW_H, pressed ? T().pressFill : T().cellFill);
        // The theme's own colours: its tab, its cells, its titles
        if (t.multi) // its four colours
            for (int k = 0; k < 4; ++k)
                C2D_DrawRectSolid(x + 6 + (k & 1) * 7, ry + 4 + (k >> 1) * 7, 0.64f, 7, 7, Hex(kMania[k]));
        else {
            C2D_DrawRectSolid(x + 6, ry + 4, 0.64f, 14, 14, t.tabOnEdge);
            C2D_DrawRectSolid(x + 8, ry + 6, 0.65f, 10, 10, t.cellFill);
            C2D_DrawRectSolid(x + 10, ry + 8, 0.66f, 6, 6, t.accent);
        }
        Text(Tr(t.name), x + 27, ry + 4, 0.48f, current ? T().accent : T().text, 0, 0.67f);
    }
}

static void TapOption(int i)
{
    if (sThemeOpen) { // a row picks its theme; any touch closes the list
        if (i >= OPT_LIST && i < OPT_LIST + THEME_COUNT)
            Port::theme = i - OPT_LIST;
        sThemeOpen = false;
        sLastOpt   = OPT_THEME;
        return;
    }
    switch (i) {
        case OPT_FRAMES: // AUTO -> 30 FPS -> OFF -> AUTO
            if (Port::lock30)
                Port::lock30 = false, Port::frameSkip = false;
            else if (Port::frameSkip)
                Port::lock30 = true;
            else
                Port::frameSkip = true;
            break;
        case OPT_FPS: Port::fps = (Port::fps + 1) % 5; break;
        case OPT_HUD: Port::hud = (Port::hud + 1) % 3; break;
        case OPT_THEME: sThemeOpen = true; break;
        default: Port::plus = !Port::plus; break; // OPT_PLUS
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
        // MANIA: the colours run along the rows and shift by one each row, so no two neighbours match
        const int tint = i + i / 2;
        Panel(x, y, 0.3f, OPT_W, OPT_H, pressed ? T().pressEdge : CellEdge(tint), pressed ? T().pressFill : CellFill(tint), 1);
        if (!T().cut)
            C2D_DrawRectSolid(x + 1, y + 1, 0.32f, 148, 1, CellLine(tint));
        u32 col;
        OptValue(i, val, sizeof(val), &col);
        Text(Tr(kOptName[i]), x + 7, y + 3, 0.42f, T().text, 0, 0.33f);
        if (i == OPT_FPS && Port::fps) {
            // A little top screen with the counter's corner filled in, as sm-3ds shows it
            const float sx = x + 7, sy = y + 15, sw = 26, sh = 14;
            C2D_DrawRectSolid(sx, sy, 0.33f, sw, sh, T().dim);
            C2D_DrawRectSolid(sx + 1, sy + 1, 0.34f, sw - 2, sh - 2, T().loadBg);
            const int corner = Port::fps - 1;
            C2D_DrawRectSolid((corner & 1) ? sx + sw - 10 : sx + 2, (corner & 2) ? sy + sh - 6 : sy + 2, 0.35f, 8, 4, col);
            Text(val, sx + sw + 6, y + 15, 0.5f, col, 0, 0.33f);
        }
        else
            Text(val, x + 7, y + 15, 0.5f, col, 0, 0.33f);
    }
    // FRAMES OFF: the game slows down where it cannot keep up; said as long as it stays so (FRAMES' own help says it
    // while it is up)
    if (!Port::lock30 && !Port::frameSkip && sLastOpt != OPT_FRAMES)
        Text(Tr("FRAMES OFF: THE GAME MAY RUN SLOW IN SOME PLACES"), W / 2.0f, H - 60, 0.4f, Rgb(240, 200, 80), C2D_AlignCenter | C2D_WordWrap,
             0.33f, W - 24);
    // What the last option touched does
    if (sLastOpt >= 0)
        Text(Tr(OptHelp(sLastOpt)), 12, HEADER_H + 8 + (OPT_COUNT + 1) / 2 * OPT_PITCH + 4, 0.4f, Rgb(240, 200, 80), C2D_WordWrap, 0.33f, W - 24);
    Text("SONIC MANIA 3DS", W / 2.0f, H - 30, 0.45f, T().footer, C2D_AlignCenter);
    Text("V" PORT_VERSION_LABEL, W / 2.0f, H - 17, 0.4f, T().footer, C2D_AlignCenter);
    if (sThemeOpen)
        DrawThemeList();
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

enum { NOTE_COLS = 42, NOTE_MAX = 256, NOTE_ROW_H = 11 };
static unsigned sNoteRows = 12; // rows a page holds: set by the box the notes are drawn in (the tab's, the prompt's)
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

static unsigned NotePages() { return sNoteCount ? (sNoteCount + sNoteRows - 1) / sNoteRows : 1; }

static void Button(float x, float y, float w, float h, const char *label, bool pressed, u32 edge, u32 textCol, float z)
{
    Panel(x, y, z, w, h, pressed ? T().pressEdge : edge, pressed ? T().pressFill : T().cellFill, 1);
    Text(Tr(label), x + w / 2, y + h / 2 - 7, 0.5f, textCol, C2D_AlignCenter, z + 0.02f);
}

static bool Inside(int px, int py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }

// The notes, paged, in the box (x, y, w) and its PREV / CLOSE / NEXT row at `by` (hit ids prev, prev + 1, prev + 2).
static void DrawNotes(const UpdateStatus &st, float x, float y, float w, float by, int prev, float z)
{
    LoadNotes(st);
    // As many rows as the box holds above the buttons (the prompt's box is shorter than the tab's)
    sNoteRows = (unsigned)((by - y - 26) / NOTE_ROW_H);
    if (sNoteRows < 1)
        sNoteRows = 1;
    const unsigned pages = NotePages();
    if (sNotePage >= pages)
        sNotePage = pages - 1;
    char head[80], ver[48];
    UpperCopy(ver, sizeof(ver), st.version);
    snprintf(head, sizeof(head), "%s %s   %u/%u", Tr("WHAT'S NEW"), ver, sNotePage + 1, pages);
    Text(head, x + w / 2, y, 0.45f, T().accent, C2D_AlignCenter, z);
    Panel(x, y + 16, z, w, by - y - 20, T().noteEdge, T().noteFill, 1);
    for (unsigned i = sNotePage * sNoteRows; i < sNoteCount && i < (sNotePage + 1) * sNoteRows; ++i) {
        // A release's heading (Update_FormatNotes) in the title colour
        const bool head = sNotes[i][0] == UPDATE_NOTES_HEADING;
        // 0.35: 42 columns of the system font fit the prompt's box (0.38 ran past its edge on the console)
        Text(sNotes[i] + head, x + 5, y + 19 + (i - sNotePage * sNoteRows) * NOTE_ROW_H, 0.35f, head ? T().accent : Rgb(220, 224, 235), 0, z + 0.02f);
    }
    const float bw = (w - 8) / 3;
    Button(x, by, bw, 24, "PREV", sPressed == prev, T().cellEdge, sNotePage ? T().text : Rgb(90, 100, 115), z);
    Button(x + bw + 4, by, bw, 24, "CLOSE", sPressed == prev + 1, T().cellEdge, T().text, z);
    Button(x + 2 * (bw + 4), by, bw, 24, "NEXT", sPressed == prev + 2, T().cellEdge,
           sNotePage + 1 < pages ? T().text : Rgb(90, 100, 115), z);
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
    const u32 good = Rgb(110, 220, 130), dim = T().dim, warn = Rgb(240, 200, 80), bad = Rgb(240, 100, 100);
    static const char *const kName[3] = { "CHANNEL", "AUTO UPDATE", "RELEASE" };
    for (int i = 0; i < 3; ++i) {
        float x, w;
        UCellRect(i, &x, &w);
        const bool pressed = i == sPressed;
        Panel(x, UCELL_Y, 0.3f, w, UCELL_H, pressed ? T().pressEdge : CellEdge(i), pressed ? T().pressFill : CellFill(i), 1);
        u32 col = T().text;
        if (i == 0)
            snprintf(val, sizeof(val), "%s", Tr(Port::betas ? "+ BETAS" : "STABLE"));
        else if (i == 1)
            snprintf(val, sizeof(val), "%s", Tr(Port::autoUpdate ? "ON" : "OFF")), col = Port::autoUpdate ? good : dim;
        else if (st.version[0])
            UpperCopy(val, sizeof(val), st.version);
        else
            snprintf(val, sizeof(val), "%s", Tr("NONE")), col = dim;
        Text(Tr(kName[i]), x + 6, UCELL_Y + 3, 0.4f, T().label, 0, 0.33f);
        Text(val, x + 6, UCELL_Y + 15, 0.45f, col, 0, 0.33f);
    }

    // Status: what the updater says (or the question before an install), with what is installed.
    Panel(8, USTATUS_Y, 0.3f, W - 16, USTATUS_H, T().boxEdge, T().boxFill, 1);
    Text(Tr("STATUS"), 14, USTATUS_Y + 4, 0.4f, dim, 0, 0.32f);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s V%s", Tr("INSTALLED"), PORT_VERSION_LABEL);
    Text(msg, W - 14, USTATUS_Y + 4, 0.38f, dim, C2D_AlignRight, 0.32f);
    UpperCopy(msg, sizeof(msg), sUpdConfirm ? "DOWNLOAD AND INSTALL THIS VERSION?" : st.message[0] ? st.message : "CHECK LOOKS FOR A NEW VERSION");
    const u32 col = st.state == UPDATE_ERROR ? bad : (st.state == UPDATE_AVAILABLE || sUpdConfirm) ? warn
                    : (st.state == UPDATE_DONE || st.state == UPDATE_CURRENT) ? good : T().text;
    Text(Tr(msg), 14, USTATUS_Y + 20, 0.5f, col, C2D_WordWrap, 0.32f, W - 28);
    const bool working = st.state == UPDATE_DOWNLOADING || st.state == UPDATE_VERIFYING || st.state == UPDATE_INSTALLING;
    if (working) {
        const float bx = 14, by = USTATUS_Y + USTATUS_H - 14, bw = W - 28;
        C2D_DrawRectSolid(bx, by, 0.33f, bw, 8, Rgb(44, 54, 70));
        C2D_DrawRectSolid(bx, by, 0.34f, bw * (st.progress > 100 ? 100 : st.progress) / 100.0f, 8, Rgb(60, 160, 100));
    }
    const char *hint = sUpdConfirm ? "UNSAVED PROGRESS IS LOST" : sUpdHint ? sUpdHint : st.version[0] ? "TAP RELEASE FOR WHAT'S NEW" : "";
    Text(Tr(hint), W / 2.0f, USTATUS_Y + USTATUS_H + 8, 0.4f, sUpdConfirm ? warn : T().footer, C2D_AlignCenter, 0.32f);

    Button(8, UACTION_Y, W - 16, 26, UpdateAction(st), sPressed == UHIT_ACTION, sUpdConfirm ? warn : T().cellEdge, T().text, 0.3f);
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
    Panel(PBOX_X - 2, PBOX_Y - 2, 0.82f, PBOX_W + 4, PBOX_H + 4, T().popEdge, T().popFill, 2);
    const float cx = W / 2.0f, x = PBOX_X + 8, w = PBOX_W - 16, by = kPromptBy;
    const u32 text = T().text, dim = T().dim, warn = Rgb(240, 200, 80);
    char line[96], ver[48];
    UpperCopy(ver, sizeof(ver), st.version);
    if (prompt == UPDATE_PROMPT_ASK && sPromptNotes) {
        DrawNotes(st, x, PBOX_Y + 6, w, by, PHIT_PREV, z + 0.02f);
        return;
    }
    if (prompt == UPDATE_PROMPT_ASK) {
        Text(Tr("UPDATE AVAILABLE"), cx, PBOX_Y + 8, 0.6f, T().accent, C2D_AlignCenter, 0.95f);
        snprintf(line, sizeof(line), "%s: %s", Tr("NEW VERSION"), ver);
        Text(line, cx, PBOX_Y + 34, 0.5f, text, C2D_AlignCenter, 0.95f);
        Text(Tr("INSTALL IT NOW?"), cx, PBOX_Y + 52, 0.5f, text, C2D_AlignCenter, 0.95f);
        Text(Tr("UNSAVED PROGRESS IS LOST"), cx, PBOX_Y + 70, 0.42f, warn, C2D_AlignCenter, 0.95f);
        snprintf(line, sizeof(line), "(%s: %s)", Tr("UPDATES TAB"), Tr("AUTO UPDATE"));
        Text(line, cx, PBOX_Y + 86, 0.4f, dim, C2D_AlignCenter, 0.95f);
        Button(cx - 75, PBOX_Y + 108, 150, 24, "WHAT'S NEW", sPressed == PHIT_NEWS, T().cellEdge, Rgb(100, 220, 255), 0.9f);
        const float bw = (w - 8) / 2;
        Button(x, by, bw, 24, "LATER", sPressed == PHIT_LATER, T().cellEdge, text, 0.9f);
        Button(x + bw + 8, by, bw, 24, "INSTALL", sPressed == PHIT_INSTALL, warn, text, 0.9f);
        return;
    }
    if (prompt == UPDATE_PROMPT_ERROR) {
        Text(Tr("UPDATE FAILED"), cx, PBOX_Y + 8, 0.6f, Rgb(240, 100, 100), C2D_AlignCenter, 0.95f);
        UpperCopy(line, sizeof(line), st.message);
        Text(Tr(line), cx, PBOX_Y + 44, 0.5f, text, C2D_AlignCenter, 0.95f);
        Text(Tr("THE GAME KEEPS RUNNING AS IT WAS."), cx, PBOX_Y + 66, 0.42f, dim, C2D_AlignCenter, 0.95f);
        Button(cx - 60, by, 120, 24, "OK", sPressed == PHIT_LATER, T().cellEdge, text, 0.9f);
        return;
    }
    // WORKING / RESTART: one bar for the download and the install, then the 10 s before the game starts again.
    float fill;
    if (prompt == UPDATE_PROMPT_RESTART) {
        const unsigned secs = Port::RelaunchSecondsLeft(&fill);
        snprintf(line, sizeof(line), "%u S", secs);
        Text(Tr("RESTARTING THE GAME"), cx, PBOX_Y + 40, 0.5f, text, C2D_AlignCenter, 0.95f);
    }
    else {
        fill = (st.progress > 100 ? 100 : st.progress) / 100.0f;
        snprintf(line, sizeof(line), "%u%%", st.progress > 100 ? 100 : st.progress);
        char msg[96];
        UpperCopy(msg, sizeof(msg), st.message);
        Text(Tr(msg), cx, PBOX_Y + 40, 0.5f, text, C2D_AlignCenter, 0.95f);
    }
    Text(Tr("UPDATING"), cx, PBOX_Y + 8, 0.6f, T().accent, C2D_AlignCenter, 0.95f);
    const float px = x + 6, py = PBOX_Y + 66, pw = w - 12, ph = 18;
    C2D_DrawRectSolid(px, py, 0.86f, pw, ph, Rgb(44, 54, 70));
    C2D_DrawRectSolid(px, py, 0.87f, pw * fill, ph, prompt == UPDATE_PROMPT_RESTART ? Rgb(235, 140, 50) : Rgb(60, 160, 100));
    Text(line, cx, py + 2, 0.5f, Rgb(255, 255, 255), C2D_AlignCenter, 0.95f);
    Text(Tr("DO NOT TURN THE CONSOLE OFF."), cx, PBOX_Y + 96, 0.42f, warn, C2D_AlignCenter, 0.95f);
    const bool canCancel = prompt == UPDATE_PROMPT_WORKING && st.state == UPDATE_DOWNLOADING;
    Button(cx - 70, by, 140, 24, canCancel ? "CANCEL" : "PLEASE WAIT", canCancel && sPressed == PHIT_LATER, T().cellEdge,
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

// ---------------------------------------------------------------- ACHIEVEMENTS tab
// The PC game's 18 achievements, which the decompilation's dummy core keeps on the SD card (UserCore.cpp registers them
// with their names and descriptions, Achievements.bin holds which are unlocked; the game unlocks them through the API).
// The tab: the count, where the unlock notice shows (Port::achNotice: the game's own on the top screen, ours over the
// tab bar, none) and its sound, then the list, which scrolls (drag it, or the bar on the right); a tap opens one.
// Steam's icons are not in the game's data: the game's ring stands for each, grey while locked. Unlock times are ours
// (achievement-dates.txt): the dummy core keeps no date.

enum { ACH_MAX = 32, ACH_ROW = 32, ACH_TOP = HEADER_H + 34, ACH_CELL_W = 92 };
static bool sAchSeen[ACH_MAX], sAchSeeded;
static time_t sAchDate[ACH_MAX]; // 0: unknown (unlocked before this build, or imported)
static float sAchScroll;
static int sAchTouchY = -1, sAchTouchStart;
static bool sAchDragging, sAchBar;
static int sAchDetail = -1;            // the one opened, -1: the list
static int sToastAch = -1;             // OPTIONS NOTICE BOTTOM: the one being announced
static u64 sToastUntil;
static const char *kAchDates = "sdmc:/3ds/Sonic Mania 3DS/achievement-dates.txt";

static int AchCount() { return (int)achievementList.size() < ACH_MAX ? (int)achievementList.size() : ACH_MAX; }

static void LoadAchDates()
{
    FILE *f = fopen(kAchDates, "r");
    if (!f)
        return;
    char id[48];
    long long when;
    while (fscanf(f, " %47s %lld", id, &when) == 2)
        for (int i = 0; i < AchCount(); ++i)
            if (achievementList[i].identifier == id)
                sAchDate[i] = (time_t)when;
    fclose(f);
}

static void SaveAchDates()
{
    FILE *f = fopen(kAchDates, "w");
    if (!f)
        return;
    for (int i = 0; i < AchCount(); ++i)
        if (sAchDate[i])
            fprintf(f, "%s %lld\n", achievementList[i].identifier.c_str(), (long long)sAchDate[i]);
    fclose(f);
}

// Every frame: an achievement unlocked since the last one gets its date, the sound and (NOTICE BOTTOM) our notice.
static void PollAchievements()
{
    const int n = AchCount();
    if (!n)
        return;
    if (!sAchSeeded) { // the ones unlocked before this start are not news
        LoadAchDates();
        for (int i = 0; i < n; ++i)
            sAchSeen[i] = achievementList[i].achieved;
        sAchSeeded = true;
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (!achievementList[i].achieved || sAchSeen[i])
            continue;
        sAchSeen[i] = true;
        sAchDate[i] = time(NULL);
        SaveAchDates();
        const uint16 sfx = GetSfx("Global/Twinkle.wav"); // a sparkle of the game's own (loaded for the whole game)
        DEV_LOG("achievement: %s (sound %d)", achievementList[i].identifier.c_str(), sfx < SFX_COUNT ? (int)sfx : -1);
        if (Port::achSound && sfx < SFX_COUNT)
            PlaySfx(sfx, 0, 0xFF);
        if (Port::achNotice == 2) {
            sToastAch   = i;
            sToastUntil = svcGetSystemTick() + 3 * (u64)SYSCLOCK_ARM11;
        }
        sDirty = true;
    }
}

// The achievement's mark: the game's ring (grey while locked) once a stage has loaded it, else the tab's ring.
static void DrawAchMark(float cx, float cy, float s, bool unlocked, float z)
{
    if (sArtReady) {
        const ArtFrame &a = sArt[ART_RING];
        if (unlocked)
            C2D_DrawImageAt(a.img, cx - a.w * s / 2, cy - a.h * s / 2, z, NULL, s, s);
        else { // its own shading in grey: the texture's luminance times a mid grey
            C2D_ImageTint tint;
            C2D_PlainImageTint(&tint, C2D_Color32(210, 215, 225, 255), 1);
            C2D_SetTintMode(C2D_TintLuma);
            C2D_DrawImageAt(a.img, cx - a.w * s / 2, cy - a.h * s / 2, z, &tint, s, s);
            C2D_SetTintMode(C2D_TintSolid);
        }
    }
    else
        DrawTabIcon(TAB_GAME, cx, cy, unlocked ? Rgb(255, 215, 0) : Rgb(90, 96, 110), z);
}

static void AchDateText(int i, char *out, unsigned cap)
{
    if (!sAchDate[i]) {
        out[0] = 0;
        return;
    }
    const struct tm *tm = gmtime(&sAchDate[i]); // the 3DS clock is local time already
    if (tm)
        snprintf(out, cap, "%02d/%02d/%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
    else
        out[0] = 0;
}

static float AchMaxScroll()
{
    const float h = (float)AchCount() * ACH_ROW + 4, view = H - ACH_TOP;
    return h > view ? h - view : 0;
}

static void AchCellRect(int c, float *x) { *x = W - 8 - (2 - c) * (ACH_CELL_W + 4) + 4; } // 0 NOTICE, 1 SOUND

static void DrawAchievementsTab()
{
    const int n = AchCount();
    char line[96];
    if (sAchScroll > AchMaxScroll())
        sAchScroll = AchMaxScroll();
    if (sAchScroll < 0)
        sAchScroll = 0;

    // The list, under the header row (which covers what scrolls up past it)
    for (int i = 0; i < n; ++i) {
        const float y = ACH_TOP + 2 + i * ACH_ROW - sAchScroll;
        if (y + ACH_ROW < ACH_TOP || y > H)
            continue;
        const AchievementInfo &a = achievementList[i];
        const bool got = a.achieved;
        Panel(6, y, 0.3f, W - 20, ACH_ROW - 3, got ? CellEdge(i) : T().tabOffEdge, got ? CellFill(i) : T().tabOffFill, 1);
        DrawAchMark(22, y + (ACH_ROW - 3) / 2.0f, 1, got, 0.32f);
        AchDateText(i, line, sizeof(line));
        // Within the row: the name stops short of the date, the description at the row's edge
        TextFit(a.name.c_str(), 38, y + 2, 0.45f, got ? T().accent : T().dim, (line[0] ? W - 92 : W - 20) - 38 - 4, 0.33f);
        if (line[0])
            Text(line, W - 20, y + 3, 0.36f, T().dim, C2D_AlignRight, 0.33f);
        TextFit(Tr(a.description.c_str()), 38, y + 16, 0.36f, got ? T().text : T().dim, W - 20 - 38 - 4, 0.33f);
    }
    // Scroll bar
    if (AchMaxScroll() > 0) {
        const float view = H - ACH_TOP, total = view + AchMaxScroll(), thumb = view * view / total;
        C2D_DrawRectSolid(W - 8, ACH_TOP, 0.4f, 4, view, T().tabOffFill);
        C2D_DrawRectSolid(W - 8, ACH_TOP + (view - thumb) * sAchScroll / AchMaxScroll(), 0.41f, 4, thumb,
                          sAchBar || sAchDragging ? T().pressFill : T().cellLine);
    }

    // Header row: the count, NOTICE and SOUND
    C2D_DrawRectSolid(0, HEADER_H, 0.5f, W, ACH_TOP - HEADER_H, T().bg);
    int got = 0;
    for (int i = 0; i < n; ++i)
        got += achievementList[i].achieved ? 1 : 0;
    snprintf(line, sizeof(line), "%d/%d", got, n);
    Text(line, 10, HEADER_H + 5, 0.7f, T().accent, 0, 0.55f);
    Text(Tr("UNLOCKED"), 10, HEADER_H + 22, 0.36f, T().dim, 0, 0.55f);
    static const char *const kNotice[3] = { "OFF", "TOP", "BOTTOM" };
    const char *names[2] = { "NOTICE", "SOUND" };
    const char *vals[2]  = { kNotice[Port::achNotice < 0 || Port::achNotice > 2 ? 1 : Port::achNotice], Port::achSound ? "ON" : "OFF" };
    for (int c = 0; c < 2; ++c) {
        float x;
        AchCellRect(c, &x);
        const bool pressed = sPressed == 100 + c;
        Panel(x, HEADER_H + 4, 0.55f, ACH_CELL_W, 26, pressed ? T().pressEdge : CellEdge(c + 1), pressed ? T().pressFill : CellFill(c + 1), 1);
        Text(Tr(names[c]), x + 5, HEADER_H + 5, 0.34f, T().label, 0, 0.57f);
        Text(Tr(vals[c]), x + 5, HEADER_H + 15, 0.42f, T().text, 0, 0.57f);
    }

    // One opened: its ring at three times, name, description, when
    if (sAchDetail >= 0 && sAchDetail < n) {
        const AchievementInfo &a = achievementList[sAchDetail];
        C2D_DrawRectSolid(0, HEADER_H, 0.8f, W, H - HEADER_H, C2D_Color32(0, 0, 0, 150));
        Panel(PBOX_X - 2, PBOX_Y - 2, 0.82f, PBOX_W + 4, PBOX_H + 4, T().popEdge, T().popFill, 2);
        DrawAchMark(W / 2.0f, PBOX_Y + 30, 3, a.achieved, 0.86f);
        Text(a.name.c_str(), W / 2.0f, PBOX_Y + 60, 0.55f, a.achieved ? T().accent : T().dim, C2D_AlignCenter, 0.9f);
        Text(Tr(a.description.c_str()), W / 2.0f, PBOX_Y + 82, 0.45f, T().text, C2D_AlignCenter | C2D_WordWrap, 0.9f, PBOX_W - 24);
        AchDateText(sAchDetail, line, sizeof(line));
        char status[96];
        if (!a.achieved)
            snprintf(status, sizeof(status), "%s", Tr("LOCKED"));
        else if (line[0])
            snprintf(status, sizeof(status), "%s %s", Tr("UNLOCKED"), line);
        else
            snprintf(status, sizeof(status), "%s", Tr("UNLOCKED"));
        Text(status, W / 2.0f, PBOX_Y + PBOX_H - 58, 0.42f, a.achieved ? Rgb(110, 220, 130) : T().dim, C2D_AlignCenter, 0.9f);
        Button(W / 2.0f - 60, PBOX_Y + PBOX_H - 32, 120, 24, "CLOSE", sPressed == 102, T().cellEdge, T().text, 0.9f);
    }
}

// Touch on the tab: the cells act on touch, the list scrolls by dragging and opens a row on a release without a drag.
// Returns what is pressed (100, 101 cells, 102 the detail's CLOSE) for the highlight.
static int AchInput(u32 held, u32 down, const touchPosition &t)
{
    if (sAchDetail >= 0) {
        const bool onClose = Inside(t.px, t.py, W / 2.0f - 60, PBOX_Y + PBOX_H - 32, 120, 24);
        if (down & KEY_TOUCH) {
            sAchDetail = -1; // CLOSE, or anywhere
            sDirty     = true;
        }
        return (held & KEY_TOUCH) && onClose ? 102 : -1;
    }
    if (down & KEY_TOUCH) {
        if (t.py < ACH_TOP) {
            for (int c = 0; c < 2; ++c) {
                float x;
                AchCellRect(c, &x);
                if (!Inside(t.px, t.py, x, HEADER_H + 4, ACH_CELL_W, 26))
                    continue;
                if (c == 0)
                    Port::achNotice = (Port::achNotice + 1) % 3;
                else
                    Port::achSound = !Port::achSound;
                SaveAll();
                sDirty = true;
                return 100 + c;
            }
            return -1;
        }
        sAchTouchY = sAchTouchStart = t.py;
        sAchDragging = false;
        sAchBar      = t.px >= W - 20 && AchMaxScroll() > 0;
    }
    if ((held & KEY_TOUCH) && sAchTouchY >= 0) {
        if (sAchBar)
            sAchScroll = (t.py - ACH_TOP) * AchMaxScroll() / (H - ACH_TOP);
        else {
            if (t.py - sAchTouchStart > 4 || sAchTouchStart - t.py > 4)
                sAchDragging = true;
            if (sAchDragging)
                sAchScroll += sAchTouchY - t.py;
        }
        sAchTouchY = t.py;
        sDirty     = true;
    }
    else if (!(held & KEY_TOUCH) && sAchTouchY >= 0) { // released
        if (!sAchDragging && !sAchBar) {
            const int row = (int)((sAchTouchStart - ACH_TOP - 2 + sAchScroll) / ACH_ROW);
            if (row >= 0 && row < AchCount())
                sAchDetail = row;
        }
        sAchTouchY   = -1;
        sAchDragging = sAchBar = false;
        sDirty       = true;
    }
    if (held & KEY_TOUCH)
        for (int c = 0; c < 2; ++c) {
            float x;
            AchCellRect(c, &x);
            if (Inside(t.px, t.py, x, HEADER_H + 4, ACH_CELL_W, 26))
                return 100 + c;
        }
    return -1;
}

// NOTICE BOTTOM: the unlock over the tab bar, three seconds, on any tab.
static void DrawAchToast()
{
    if (sToastAch < 0)
        return;
    if (svcGetSystemTick() >= sToastUntil || sToastAch >= AchCount()) {
        sToastAch = -1;
        return;
    }
    const float x = 4, y = 2, w = W - 8, h = 40;
    Panel(x - 1, y - 1, 0.9f, w + 2, h + 2, T().accent, T().popFill, 1);
    DrawAchMark(x + 20, y + h / 2, 1.5f, true, 0.92f);
    Text(Tr("ACHIEVEMENT!"), x + 40, y + 4, 0.42f, T().accent, 0, 0.93f);
    Text(achievementList[sToastAch].name.c_str(), x + 40, y + 19, 0.55f, T().text, 0, 0.93f);
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
    C2D_DrawRectSolid(0, BTN_TOP - BTN_GAP / 2, 0.7f, W, H - BTN_TOP + BTN_GAP / 2, T().bg);
    C2D_DrawRectSolid(0, BTN_TOP - BTN_GAP / 2, 0.71f, W, 1, T().barLine);
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

static unsigned sDrawCount;
static u64 sDrawTicks;

void BottomUi::DrawStats(unsigned *count, float *ms)
{
    *count = sDrawCount;
    *ms    = sDrawTicks * (1000.0f / SYSCLOCK_ARM11);
}

static void Draw()
{
    char val[32];
    const u64 tDraw = svcGetSystemTick();
    ++sDrawCount;
    // Any full redraw shows the updater's state as it is now. Only the UPDATES tab and the prompt took it before, so
    // after the boot's update check every other tab was redrawn every frame (7-11 ms a frame on the New 3DS).
    {
        UpdateStatus st;
        Updater_GetStatus(&st);
        sDrawnRevision = st.revision;
    }

    C2D_TextBufClear(sTextBuf);
    C3D_FrameBegin(0); // SYNCDRAW would wait for a vblank: up to 16 ms of the game's frame, four times a second
    C2D_Prepare();
    // The top-screen presentation turns blending off and the state is sticky: citro2d's glyph alpha needs it.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_TargetClear(sTarget, T().bg);
    C2D_SceneBegin(sTarget);
    DrawBackdrop(HEADER_H);

    switch (Port::tab) {
        case TAB_OPTIONS: DrawOptionsTab(); break;
        case TAB_UPDATES: DrawUpdatesTab(); break;
        case TAB_ACH: DrawAchievementsTab(); break;
#if MANIA_DEV_TOOLS
        case TAB_DEBUG: DrawDebugTab(); break;
#endif
        default: DrawGameTab(); break;
    }
    DrawTabBar();
    DrawAchToast();
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
        Panel(bx - 2, by - 2, 0.82f, bw + 4, bh + 4, T().popEdge, T().popFill, 2);
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
    sDrawTicks += svcGetSystemTick() - tDraw;
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

void BottomUi::DrawInTopFrame()
{
    // The GAME tab's TIME box again, alone, when the time has changed: the rest of the screen stays as the last full
    // redraw left it (the target is not cleared). It rides on the top screen's frame, so it costs no frame of its own:
    // a frame of its own waits for the GPU to finish the top picture, which the four redraws a second can afford and
    // sixty could not.
    // And the RINGS box while there are none, at each turn of its label's blink.
    if (!sReady || !(sTimeBox.shown || sRingsBox.shown) || Port::tab != TAB_GAME || UpdatePromptShown())
        return;
#if MANIA_DEV_TOOLS
    float p;
    unsigned secs;
    char notice[8];
    if (!DevTools::swBottomUi || DevTools::InstallNotice(notice, sizeof(notice), &p, &secs))
        return;
#endif
    const bool timeMoved = sTimeBox.shown && (sTimeBox.m != sceneInfo.minutes || sTimeBox.sec != sceneInfo.seconds || sTimeBox.ms != sceneInfo.milliseconds);
    PortGameInfo gi;
    bool blink = false;
    if (sRingsBox.shown && Flash() != sRingsBox.flash) {
        PortGame_GetInfo(&gi);
        blink = gi.playerValid && !gi.rings;
        sRingsBox.flash = Flash();
    }
    if (!timeMoved && !blink)
        return;
    sTimeBox.m   = (int8)sceneInfo.minutes;
    sTimeBox.sec = (int8)sceneInfo.seconds;
    sTimeBox.ms  = (int8)sceneInfo.milliseconds;
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_SceneBegin(sTarget);
    sNoShadow = true;
    if (timeMoved)
        DrawTimeBox(0, sTimeBox.x, sTimeBox.y, sTimeBox.w, sTimeBox.h, true);
    if (blink)
        DrawRingsBox(sRingsBox.x, sRingsBox.y, sRingsBox.w, sRingsBox.h, true, 0);
    sNoShadow = false;
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
    bool active, boot; // boot: the start of the game (the top screen gets a colour)
    char title[64], step[48], file[64];
    float lo, hi, progress;
    u64 start, lastDraw;
} sLoad;

// ---------------------------------------------------------------- the top screen while the game starts
// One flat colour, MANIA's navy (the game's menus): the boot redraws only as its loaders report, too seldom for an
// animation (one was tried: it crawled).
static const u32 kBootTop = 0x0A1438;

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
    C2D_TargetClear(sTarget, T().loadBg);
    C2D_SceneBegin(sTarget);
    DrawBackdrop(0);

    const float bx = 20, by = 62, bw = W - 40, bh = 116;
    Panel(bx - 1, by - 1, 0.3f, bw + 2, bh + 2, T().noteEdge, T().noteFill, 1);
    Text(Tr(sLoad.title), W / 2.0f, by + 10, 0.55f, T().accent, C2D_AlignCenter, 0.4f);
    Text(Tr(sLoad.step), W / 2.0f, by + 34, 0.5f, T().text, C2D_AlignCenter, 0.4f);
    Text(sLoad.file, W / 2.0f, by + 52, 0.4f, T().dim, C2D_AlignCenter, 0.4f);
    const float px = bx + 14, py = by + 72, pw = bw - 28, ph = 16;
    C2D_DrawRectSolid(px, py, 0.35f, pw, ph, Rgb(44, 54, 70));
    const float f = sLoad.progress < 0 ? 0 : sLoad.progress > 1 ? 1 : sLoad.progress;
    C2D_DrawRectSolid(px, py, 0.36f, pw * f, ph, Rgb(60, 160, 100));
    snprintf(line, sizeof(line), "%u%%", (unsigned)(f * 100));
    Text(line, W / 2.0f, py + 1, 0.45f, Rgb(255, 255, 255), C2D_AlignCenter, 0.4f);
    const float secs = (float)(svcGetSystemTick() - sLoad.start) / SYSCLOCK_ARM11;
    snprintf(line, sizeof(line), "%.1f S", secs);
    Text(line, bx + bw - 8, by + bh - 14, 0.38f, T().footer, C2D_AlignRight, 0.4f);
    Text("SONIC MANIA 3DS", W / 2.0f, H - 18, 0.42f, T().footer, C2D_AlignCenter);
    // The top screen's GPU target is made by the first frame the game presents, after this load: made here if not yet
    if (sLoad.boot && DEV_SW_GPUPRESENT && (C3DPresent::Ready() || C3DPresent::Init())) {
        C3D_RenderTarget *top = (C3D_RenderTarget *)C3DPresent::Target();
        C2D_SceneBegin(top);
        C2D_DrawRectSolid(0, 0, 0.01f, 400, 240, Hex(kBootTop)); // not C2D_TargetClear: its colour came out wrong on this RGB565 target
    }
    C3D_FrameEnd(0);
    sLoad.lastDraw = svcGetSystemTick();
#if MANIA_DEV_TOOLS
    DevTools::ShotFromFile(); // the emulator's way to a picture of the boot
#endif
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
    sLoad.boot = !strcmp(title, "Starting Sonic Mania"); // RetroEngine.cpp's boot load
    sLoad.step[0] = sLoad.file[0] = 0;
    sLoad.lo = sLoad.progress = 0;
    sLoad.hi                  = 1;
    sLoad.start               = svcGetSystemTick();
    sLoad.active              = true;
    MaybeDrawLoading(true);
}

void BottomUi::LoadPhase(const char *step, float lo, float hi)
{
#if MANIA_DEV_TOOLS
    DevTools::HeapCheck(step);
#endif
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
#if MANIA_DEV_TOOLS
    if (DevTools::heapCheck) {
        char where[64];
        snprintf(where, sizeof(where), "%s %d/%d", sLoad.step, i, n);
        DevTools::HeapCheck(where);
    }
#endif
    if (!sLoad.active || n <= 0)
        return;
    sLoad.progress = sLoad.lo + (sLoad.hi - sLoad.lo) * i / n;
    MaybeDrawLoading(false);
}

void BottomUi::LoadDetail(const char *file)
{
#if MANIA_DEV_TOOLS
    DevTools::HeapCheck(file ? file : "file");
#endif
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
    sItemsReset  = true;
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

int BottomUi::Ask(const char *title, const char *const *lines, int lineCount, const char *yes, const char *no)
{
    if (!sReady)
        return -1;
    const float bx = PBOX_X, by = PBOX_Y, bw = PBOX_W, bh = PBOX_H, cy = by + bh - 32;
    const float btnW = no ? (bw - 24) / 2 : 120, yesX = no ? bx + 8 : W / 2.0f - 60, noX = bx + 16 + btnW;
    int pressed = -1;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown(), held = hidKeysHeld();
        int hit = -1;
        if (held & KEY_TOUCH) {
            touchPosition t;
            hidTouchRead(&t);
            if (Inside(t.px, t.py, yesX, cy, btnW, 24))
                hit = 1;
            else if (no && Inside(t.px, t.py, noX, cy, btnW, 24))
                hit = 0;
        }
        if (down & KEY_A)
            return 1;
        if (no && (down & KEY_B))
            return 0;
        if (!(held & KEY_TOUCH) && pressed >= 0)
            return pressed; // released on a button
        pressed = hit;
#if MANIA_DEV_TOOLS
        static unsigned polls;
        if (++polls % 30 == 0) { // the emulator's way in (no FTP yet)
            const int fromFile = DevTools::AskFromFile();
            if (fromFile >= 0 && (fromFile || no))
                return fromFile;
        }
#endif

        C2D_TextBufClear(sTextBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_Prepare();
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        C2D_TargetClear(sTarget, T().bg);
        C2D_SceneBegin(sTarget);
        DrawBackdrop(0);
        Panel(bx - 2, by - 2, 0.82f, bw + 4, bh + 4, T().popEdge, T().popFill, 2);
        Text(title, W / 2.0f, by + 10, 0.6f, T().accent, C2D_AlignCenter, 0.95f);
        for (int i = 0; i < lineCount; ++i)
            Text(lines[i], bx + 12, by + 38 + i * 34, 0.45f, i == lineCount - 1 && lineCount > 1 ? T().dim : T().text, C2D_WordWrap, 0.95f, bw - 24);
        Button(yesX, cy, btnW, 24, yes, pressed == 1, T().cellEdge, T().text, 0.9f);
        if (no)
            Button(noX, cy, btnW, 24, no, pressed == 0, T().cellEdge, T().text, 0.9f);
        C3D_FrameEnd(0);
    }
    return -1;
}

static volatile int sTouchX = -1, sTouchY; // a tap from SITE TOUCH (the FTP thread), taken by the next Update

void BottomUi::Touch(int x, int y)
{
    sTouchY = y;
    sTouchX = x;
}

void BottomUi::Update()
{
    if (!sReady)
        return;
    PollItems(); // every frame, whatever the tab: a box opened while another tab is open still shows
    PollAchievements();
#if MANIA_DEV_TOOLS
    if (!DevTools::swBottomUi)
        return;
    const bool debugTab = Port::tab == TAB_DEBUG;
#else
    const bool debugTab = false;
#endif
    u32 down = hidKeysDown(), held = hidKeysHeld();
    touchPosition t;
    hidTouchRead(&t);
    if (sTouchX >= 0) { // held for this one frame: the next one sees it lifted
        t.px = (u16)sTouchX;
        t.py = (u16)sTouchY;
        sTouchX = -1;
        down |= KEY_TOUCH;
        held |= KEY_TOUCH;
    }
    // ACHIEVEMENTS handles its own touches (a drag scrolls, a release opens), except on the tab bar and under a prompt
    const bool achTab = Port::tab == TAB_ACH && !UpdatePromptShown() && !((down & KEY_TOUCH) && t.py < HEADER_H);
    const int achHit  = achTab ? AchInput(held, down, t) : -1;
    int hit = -1;
    if (held & KEY_TOUCH) {
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
                    Port::tab  = kTabs[i];
                    sLastOpt   = -1;
                    sThemeOpen = false;
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
        else if (Port::tab == TAB_OPTIONS && sThemeOpen)
            hit = ThemeListHit(t.px, t.py);
        else if (Port::tab == TAB_OPTIONS) {
            for (int i = 0; i < OPT_COUNT; ++i) {
                float x, y;
                OptRect(i, &x, &y);
                if (t.px >= x && t.px < x + OPT_W && t.py >= y && t.py < y + OPT_H)
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
        else if (achTab)
            ; // AchInput acted already
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
    if (achTab)
        hit = achHit;
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
    char notice[8];
    busy = busy || DevTools::InstallNotice(notice, sizeof(notice), &p, &secs) != 0;
#endif
    if (sDirty || UpdatesChanged() || now - sLastDraw > (busy ? SYSCLOCK_ARM11 / 10 : SYSCLOCK_ARM11 / 4)) {
        Draw();
        sLastDraw = now;
        sDirty    = false;
    }
}

#endif
