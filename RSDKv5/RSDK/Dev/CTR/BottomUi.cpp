#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/BottomUi.hpp"

#if RETRO_PLATFORM == RETRO_3DS && MANIA_DEV_TOOLS

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "RSDK/Graphics/CTR/C3DPresent.hpp"

using namespace RSDK;

enum { W = 320, H = 240 };

static bool sReady;
static C3D_RenderTarget *sTarget;
static C2D_TextBuf sTextBuf;
static bool sDirty = true;
static u64 sLastDraw;
static int sPressed = -1;

static inline u32 Rgb(int r, int g, int b) { return C2D_Color32(r, g, b, 255); }

struct Button {
    const char *label;
    void (*tap)();
    void (*value)(char *out, unsigned cap, bool *on);
};

static void ValFrameSkip(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swFrameSkip ? "ON" : "OFF"); *on = DevTools::swFrameSkip; }
static void TapFrameSkip() { DevTools::swFrameSkip = !DevTools::swFrameSkip; }
static void ValMaxSkip(char *o, unsigned c, bool *on) { snprintf(o, c, "%d", DevTools::swMaxSkip); *on = true; }
static void TapMaxSkip() { DevTools::swMaxSkip = DevTools::swMaxSkip % 4 + 1; }
static void ValDraw(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swDraw ? "ON" : "OFF"); *on = DevTools::swDraw; }
static void TapDraw() { DevTools::swDraw = !DevTools::swDraw; }
static void ValFlip(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swFlipCopy ? "ON" : "OFF"); *on = DevTools::swFlipCopy; }
static void TapFlip() { DevTools::swFlipCopy = !DevTools::swFlipCopy; }
static void ValPresent(char *o, unsigned c, bool *on) { snprintf(o, c, "%s", DevTools::swGpuPresent ? "GPU" : "CPU"); *on = DevTools::swGpuPresent; }
static void TapPresent() { DevTools::swGpuPresent = !DevTools::swGpuPresent; }
static unsigned sMarks;
static void ValMark(char *o, unsigned c, bool *on) { snprintf(o, c, "%u", sMarks); *on = false; }
static void TapMark()
{
    ++sMarks;
    DevTools::Mark();
}

static const Button kButtons[] = {
    { "FRAME SKIP", TapFrameSkip, ValFrameSkip }, { "MAX SKIPPED", TapMaxSkip, ValMaxSkip }, { "DRAW LISTS", TapDraw, ValDraw },
    { "FLIP COPY", TapFlip, ValFlip },            { "PRESENT", TapPresent, ValPresent },     { "LOG MARK", TapMark, ValMark },
};
enum { BTN_COUNT = sizeof(kButtons) / sizeof(kButtons[0]), BTN_COLS = 2, BTN_W = 150, BTN_H = 32, BTN_GAP = 5, BTN_TOP = 112 };

static void ButtonRect(int i, float *x, float *y)
{
    *x = BTN_GAP + (i % BTN_COLS) * (BTN_W + BTN_GAP);
    *y = BTN_TOP + (i / BTN_COLS) * (BTN_H + BTN_GAP);
}

static void Text(const char *s, float x, float y, float scale, u32 color, u32 flags = 0)
{
    C2D_Text t;
    C2D_TextParse(&t, sTextBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

static void Draw()
{
    DevTools::Stats st;
    DevTools::GetStats(&st);
    char line[96], val[32];

    C2D_TextBufClear(sTextBuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_Prepare();
    // The top-screen presentation turns blending off and the state is sticky: citro2d's glyph alpha needs it.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_TargetClear(sTarget, Rgb(14, 18, 24));
    C2D_SceneBegin(sTarget);

    // Header
    C2D_DrawRectSolid(0, 0, 0.1f, W, 22, Rgb(30, 64, 120));
    Text("SONIC MANIA 3DS", 8, 2, 0.6f, Rgb(255, 255, 255));
    Text("dev tools", W - 8, 4, 0.45f, Rgb(170, 200, 255), C2D_AlignRight);

    // Frame rate: green at 59+, yellow from 50, red below
    const u32 fpsColor = st.fps >= 59 ? Rgb(90, 220, 110) : st.fps >= 50 ? Rgb(240, 210, 70) : Rgb(235, 90, 80);
    snprintf(line, sizeof(line), "%.0f", st.fps);
    Text(line, 10, 24, 1.1f, fpsColor);
    Text("fps", 74, 42, 0.5f, Rgb(160, 170, 180));
    snprintf(line, sizeof(line), "frame %.1f ms   skipped %u/s", st.frameMs, st.skipped);
    Text(line, 118, 30, 0.45f, Rgb(210, 215, 220));
    snprintf(line, sizeof(line), "scene %d   %s", st.scene, DevTools::swGpuPresent ? "GPU present" : "CPU present");
    Text(line, 118, 46, 0.45f, Rgb(160, 170, 180));

    // Stacked bar of one 16.7 ms frame: update | draw | flip
    const float budget = 16.7f, barX = 8, barY = 72, barW = W - 16, barH = 14;
    C2D_DrawRectSolid(barX, barY, 0.2f, barW, barH, Rgb(34, 40, 48));
    const u32 colors[3] = { Rgb(80, 140, 235), Rgb(240, 150, 60), Rgb(130, 140, 150) };
    float x = barX;
    for (int i = 0; i < 3; ++i) {
        float w = st.stageMs[i] / budget * barW;
        if (x + w > barX + barW)
            w = barX + barW - x;
        if (w > 0)
            C2D_DrawRectSolid(x, barY, 0.3f, w, barH, colors[i]);
        x += w;
    }
    snprintf(line, sizeof(line), "update %.1f   draw %.1f   flip %.1f   idle %.1f", st.stageMs[0], st.stageMs[1], st.stageMs[2], st.stageMs[12]);
    Text(line, 8, 88, 0.42f, Rgb(200, 205, 210));
    snprintf(line, sizeof(line), "layers %.1f  sprites %.1f  3d %.1f  faces %.1f", st.stageMs[3], st.stageMs[4], st.stageMs[7], st.stageMs[6]);
    Text(line, 8, 99, 0.38f, Rgb(140, 150, 160));
    snprintf(line, sizeof(line), "FTP %s", DevTools::FtpStatus());
    Text(line, 8, 224, 0.4f, Rgb(120, 200, 140));

    // Buttons
    for (int i = 0; i < BTN_COUNT; ++i) {
        float bx, by;
        ButtonRect(i, &bx, &by);
        bool on = false;
        val[0] = 0;
        kButtons[i].value(val, sizeof(val), &on);
        u32 fill = on ? Rgb(28, 88, 52) : Rgb(40, 46, 56);
        if (i == sPressed)
            fill = on ? Rgb(50, 130, 80) : Rgb(70, 82, 100);
        C2D_DrawRectSolid(bx, by, 0.4f, BTN_W, BTN_H, fill);
        C2D_DrawRectSolid(bx, by, 0.45f, 3, BTN_H, on ? Rgb(90, 220, 110) : Rgb(90, 100, 115));
        Text(kButtons[i].label, bx + 10, by + 2, 0.42f, Rgb(190, 198, 206));
        Text(val, bx + 10, by + 15, 0.55f, Rgb(255, 255, 255));
    }

    // An install in progress is a modal message: the screen is dimmed and a box with the step, a bar and the
    // percentage (or the seconds left before the restart) in it takes the middle.
    float progress;
    unsigned seconds;
    char title[48];
    const int phase = DevTools::InstallNotice(title, sizeof(title), &progress, &seconds);
    if (phase) {
        C2D_DrawRectSolid(0, 0, 0.6f, W, H, C2D_Color32(0, 0, 0, 170));
        const float bx = 30, by = 80, bw = W - 60, bh = 84;
        C2D_DrawRectSolid(bx - 2, by - 2, 0.62f, bw + 4, bh + 4, Rgb(90, 120, 170));
        C2D_DrawRectSolid(bx, by, 0.64f, bw, bh, Rgb(24, 32, 46));
        Text(title, W / 2.0f, by + 8, 0.6f, Rgb(255, 255, 255), C2D_AlignCenter);
        const float px = bx + 14, py = by + 44, pw = bw - 28, ph = 24;
        C2D_DrawRectSolid(px, py, 0.66f, pw, ph, Rgb(44, 54, 70));
        const float fill = progress < 0 ? 0 : progress > 1 ? 1 : progress;
        C2D_DrawRectSolid(px, py, 0.68f, pw * fill, ph, phase == 3 ? Rgb(235, 140, 50) : Rgb(60, 160, 100));
        if (phase == 3)
            snprintf(val, sizeof(val), "%us", seconds);
        else if (progress >= 0)
            snprintf(val, sizeof(val), "%u%%", (unsigned)(fill * 100));
        else
            snprintf(val, sizeof(val), "...");
        Text(val, W / 2.0f, py + 3, 0.55f, Rgb(255, 255, 255), C2D_AlignCenter);
    }

    C3D_FrameEnd(0);
}

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
    sTextBuf = C2D_TextBufNew(2048);
    if (!sTextBuf)
        return;
    sReady = true;
    sDirty = true;
}

bool BottomUi::Ready() { return sReady; }

void BottomUi::Update()
{
    if (!sReady)
        return;
    const u32 down = hidKeysDown(), held = hidKeysHeld();
    int hit = -1;
    if (held & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        for (int i = 0; i < BTN_COUNT; ++i) {
            float bx, by;
            ButtonRect(i, &bx, &by);
            if (t.px >= bx && t.px < bx + BTN_W && t.py >= by && t.py < by + BTN_H)
                hit = i;
        }
    }
    if ((down & KEY_TOUCH) && hit >= 0) {
        kButtons[hit].tap();
        DevTools::SaveSettings();
        sDirty = true;
    }
    if (hit != sPressed) {
        sPressed = hit;
        sDirty   = true;
    }
    const u64 now = svcGetSystemTick();
    // Quicker while an install is going on, so the bar moves.
    float p;
    unsigned secs;
    char t[8];
    const bool installing = DevTools::InstallNotice(t, sizeof(t), &p, &secs) != 0;
    if (sDirty || now - sLastDraw > (installing ? SYSCLOCK_ARM11 / 10 : SYSCLOCK_ARM11 / 4)) {
        Draw();
        sLastDraw = now;
        sDirty    = false;
    }
}

#endif
