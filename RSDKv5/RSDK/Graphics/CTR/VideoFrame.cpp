#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Graphics/CTR/VideoFrame.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

using namespace RSDK;

namespace
{

uint16 *sSlots[VideoFrame::SLOTS]; // W x H RGB565 each
volatile int32 sShown  = -1;       // the slot on the screen
int32 sTarget          = 0;        // where SetYUV converts
uint16 *sDimmed        = NULL;     // the shown picture at a lower brightness (image fades), made on demand
float sDimmedAt        = -1.0f;
volatile uint32 sVersion = 0;      // changes with the picture
uint64 sConvertTicks   = 0;
uint32 sConversions    = 0;
uint32 sY2RFrames      = 0;        // frames the Y2R hardware converted (the rest went through the CPU)

// The Y2R service: opened with the first frame it can convert, closed by Release; a failure leaves the rest of the
// video to the CPU.
enum Y2RState { Y2R_CLOSED, Y2R_OPEN, Y2R_FAILED };
Y2RState sY2R  = Y2R_CLOSED;
Handle sY2REnd = 0;
// Y2R reads a copy of the planes (the decoder writes its own buffers again with the next frame), so the conversion
// runs while the decoder thread decodes the next frame. Busy until WaitConversion / ConversionDone sees it end.
uint8 *sStaging     = NULL;
u32 sStagingSize    = 0;
bool sY2RBusy       = false;
uint16 *sY2ROut     = NULL;
u32 sY2ROutBytes    = 0;

struct Frame {
    int32 width, height;
    const uint8 *y, *u, *v;
    int32 strideY, strideU, strideV;
    int32 shiftX, shiftY;
};

// BT.601 limited range. Generic path: 8.8 fixed point, the sum clamped by tables (offset CLAMP_OFS) that give the
// channel already reduced to 5 or 6 bits. Fast path: the three channels in the bytes of one word (R low), luma
// already scaled, the chroma offsets added and subtracted with the ARM11's saturating byte instructions.
enum { CLAMP_OFS = 384, CLAMP_LEN = 1024 };
int32 sY[256], sRV[256], sGU[256], sGV[256], sBU[256];
uint8 sClamp5[CLAMP_LEN], sClamp6[CLAMP_LEN];
uint32 sYRep[256];                                       // the scaled luma (0..255) in the three channel bytes
int16 sRoff[256], sGoffU[256], sGoffV[256], sBoff[256]; // chroma offsets (G's in 8.8)
bool sTables = false;

void BuildTables()
{
    for (int32 i = 0; i < 256; ++i) {
        sY[i]  = 298 * (i - 16) + 128; // + 128 rounds the >> 8
        sRV[i] = 409 * (i - 128);
        sGU[i] = -100 * (i - 128);
        sGV[i] = -208 * (i - 128);
        sBU[i] = 516 * (i - 128);

        int32 yl  = (298 * (i - 16) + 128) >> 8;
        yl        = yl < 0 ? 0 : (yl > 255 ? 255 : yl);
        sYRep[i]  = (uint32)yl * 0x010101u;
        sRoff[i]  = (int16)((409 * (i - 128) + 128) >> 8);
        sGoffU[i] = (int16)(-100 * (i - 128));
        sGoffV[i] = (int16)(-208 * (i - 128));
        sBoff[i]  = (int16)((516 * (i - 128) + 128) >> 8);
    }
    for (int32 i = 0; i < CLAMP_LEN; ++i) {
        int32 c    = i - CLAMP_OFS;
        c          = c < 0 ? 0 : (c > 255 ? 255 : c);
        sClamp5[i] = (uint8)(c >> 3);
        sClamp6[i] = (uint8)(c >> 2);
    }
    sTables = true;
}

inline uint32 Uqadd8(uint32 a, uint32 b)
{
    uint32 r;
    __asm__("uqadd8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

inline uint32 Uqsub8(uint32 a, uint32 b)
{
    uint32 r;
    __asm__("uqsub8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

// 0x00BBGGRR -> RGB565
inline uint32 Pack565(uint32 w) { return ((w & 0xF8) << 8) | ((w >> 5) & 0x7E0) | ((w >> 19) & 0x1F); }

inline uint32 Clamp255(int32 v) { return (uint32)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// The rectangle a width x height picture takes on the screen, scaled to fit and centred.
void Fit(int32 width, int32 height, int32 *dw, int32 *dh, int32 *ox, int32 *oy)
{
    if (width * VideoFrame::H > height * VideoFrame::W) { // wider than the screen: fit the width
        *dw = VideoFrame::W;
        *dh = (height * VideoFrame::W + width / 2) / width;
    }
    else {
        *dh = VideoFrame::H;
        *dw = (width * VideoFrame::H + height / 2) / height;
    }
    if (*dw > VideoFrame::W)
        *dw = VideoFrame::W;
    if (*dh > VideoFrame::H)
        *dh = VideoFrame::H;
    *ox = (VideoFrame::W - *dw) / 2;
    *oy = (VideoFrame::H - *dh) / 2;
}

// A 4:2:0 frame at the screen's size on the CPU: each 2x2 block shares one chroma sample, so its offsets are worked
// out once for four pixels; two pixels go out as one word.
void Convert420Native(const Frame &p, uint16 *dst, int32 ox, int32 oy)
{
    for (int32 sy = 0; sy < p.height; sy += 2) {
        const uint8 *y0 = p.y + sy * p.strideY;
        const uint8 *y1 = y0 + p.strideY;
        const uint8 *ur = p.u + (sy >> 1) * p.strideU;
        const uint8 *vr = p.v + (sy >> 1) * p.strideV;
        uint32 *o0      = (uint32 *)(dst + (oy + sy) * VideoFrame::W + ox);
        uint32 *o1      = o0 + VideoFrame::W / 2;
        for (int32 cx = 0; cx < (p.width >> 1); ++cx) {
            const int32 cu   = ur[cx], cv = vr[cx];
            const int32 r    = sRoff[cv];
            const int32 g    = (sGoffU[cu] + sGoffV[cv] + 128) >> 8;
            const int32 b    = sBoff[cu];
            const uint32 add = Clamp255(r) | (Clamp255(g) << 8) | (Clamp255(b) << 16);
            const uint32 sub = Clamp255(-r) | (Clamp255(-g) << 8) | (Clamp255(-b) << 16);
            const uint32 a   = Uqsub8(Uqadd8(sYRep[y0[2 * cx]], add), sub);
            const uint32 c   = Uqsub8(Uqadd8(sYRep[y0[2 * cx + 1]], add), sub);
            const uint32 d   = Uqsub8(Uqadd8(sYRep[y1[2 * cx]], add), sub);
            const uint32 e   = Uqsub8(Uqadd8(sYRep[y1[2 * cx + 1]], add), sub);
            o0[cx]           = Pack565(a) | (Pack565(c) << 16);
            o1[cx]           = Pack565(d) | (Pack565(e) << 16);
        }
    }
}

// Any size and subsampling: nearest sample, the 8.8 sums and clamp tables.
void ConvertScaled(const Frame &p, uint16 *dst, int32 dw, int32 dh, int32 ox, int32 oy)
{
    static int16 xmap[VideoFrame::W];
    for (int32 dx = 0; dx < dw; ++dx) xmap[dx] = (int16)((dx * p.width + p.width / 2) / dw);

    for (int32 dy = 0; dy < dh; ++dy) {
        const int32 sy  = (dy * p.height + p.height / 2) / dh;
        const uint8 *yr = p.y + sy * p.strideY;
        const uint8 *ur = p.u + (sy >> p.shiftY) * p.strideU;
        const uint8 *vr = p.v + (sy >> p.shiftY) * p.strideV;
        uint16 *out     = dst + (oy + dy) * VideoFrame::W + ox;
        for (int32 dx = 0; dx < dw; ++dx) {
            const int32 sx = xmap[dx];
            const int32 cx = sx >> p.shiftX;
            const int32 yy = sY[yr[sx]];
            const int32 cu = ur[cx], cv = vr[cx];
            const int32 r  = ((yy + sRV[cv]) >> 8) + CLAMP_OFS;
            const int32 g  = ((yy + sGU[cu] + sGV[cv]) >> 8) + CLAMP_OFS;
            const int32 b  = ((yy + sBU[cu]) >> 8) + CLAMP_OFS;
            out[dx]        = (uint16)((sClamp5[r] << 11) | (sClamp6[g] << 5) | sClamp5[b]);
        }
    }
}

bool OpenY2R()
{
    if (sY2R == Y2R_OPEN)
        return true;
    if (sY2R == Y2R_FAILED)
        return false;
    sY2R = Y2R_FAILED;
    if (R_FAILED(y2rInit()))
        return false;
    if (R_FAILED(Y2RU_SetTransferEndInterrupt(true)) || R_FAILED(Y2RU_GetTransferEndEvent(&sY2REnd))) {
        y2rExit();
        return false;
    }
    sY2R = Y2R_OPEN;
    return true;
}

// The end of a started conversion: wait (or only look, waitNs 0). True once it is over (or none was running).
bool EndY2R(s64 waitNs)
{
    if (!sY2RBusy)
        return true;
    const Result rc = svcWaitSynchronization(sY2REnd, waitNs);
    if (R_FAILED(rc)) {
        if (waitNs == 0)
            return false; // still running
        Y2RU_StopConversion(); // no answer in time: the frame is lost, the rest of the video goes through the CPU
        sY2R = Y2R_FAILED;
    }
    else
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)sY2ROut, sY2ROutBytes);
    sY2RBusy = false;
    return true;
}

void CloseY2R()
{
    EndY2R(100 * 1000000LL);
    free(sStaging);
    sStaging     = NULL;
    sStagingSize = 0;
    if (sY2R == Y2R_OPEN) {
        bool busy = false;
        if (R_SUCCEEDED(Y2RU_IsBusyConversion(&busy)) && busy)
            Y2RU_StopConversion();
        y2rExit();
    }
    sY2R    = Y2R_CLOSED;
    sY2REnd = 0;
}

// A 4:2:0 frame W pixels wide through Y2R into dst (from row oy): the planes are copied (without the decoder's
// borders) and the conversion started; WaitConversion / ConversionDone tell when it is over. False: not started.
bool ConvertY2R(const Frame &p, uint16 *dst, int32 oy)
{
    if (p.width != VideoFrame::W || (p.height & 7) || p.shiftX != 1 || p.shiftY != 1 || !OpenY2R())
        return false;
    const int32 cw = p.width >> 1, ch = p.height >> 1;
    const u32 ySize = (u32)(p.width * p.height), cSize = (u32)(cw * ch);
    EndY2R(100 * 1000000LL); // the previous one reads the staging copy
    if (sY2R != Y2R_OPEN)
        return false;
    if (sStagingSize < ySize + 2 * cSize) {
        free(sStaging);
        sStaging     = (uint8 *)malloc(ySize + 2 * cSize);
        sStagingSize = sStaging ? ySize + 2 * cSize : 0;
        if (!sStaging)
            return false;
    }
    uint8 *sy = sStaging, *su = sStaging + ySize, *sv = su + cSize;
    for (int32 r = 0; r < p.height; ++r) memcpy(sy + r * p.width, p.y + r * p.strideY, (size_t)p.width);
    for (int32 r = 0; r < ch; ++r) {
        memcpy(su + r * cw, p.u + r * p.strideU, (size_t)cw);
        memcpy(sv + r * cw, p.v + r * p.strideV, (size_t)cw);
    }

    Y2RU_ConversionParams params;
    memset(&params, 0, sizeof(params));
    params.input_format         = INPUT_YUV420_INDIV_8;
    params.output_format        = OUTPUT_RGB_16_565;
    params.rotation             = ROTATION_NONE;
    params.block_alignment      = BLOCK_LINE;
    params.input_line_width     = (s16)p.width;
    params.input_lines          = (s16)p.height;
    params.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING; // Theora's luma is 16..235
    params.alpha                = 0xFF;

    // The copy out of the CPU's cache before the DMA reads it; the picture's lines dropped from it once written
    // (EndY2R), before C3DPresent copies them
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)sStaging, ySize + 2 * cSize);
    uint16 *out    = dst + oy * VideoFrame::W;
    const u32 outN = (u32)(p.width * p.height * 2);

    const bool ok = R_SUCCEEDED(Y2RU_SetConversionParams(&params)) && R_SUCCEEDED(Y2RU_SetSendingY(sy, ySize, (s16)p.width, 0))
                    && R_SUCCEEDED(Y2RU_SetSendingU(su, cSize, (s16)cw, 0)) && R_SUCCEEDED(Y2RU_SetSendingV(sv, cSize, (s16)cw, 0))
                    && R_SUCCEEDED(Y2RU_SetReceiving(out, outN, (s16)(p.width * 2 * 8), 0)) // 8 lines a transfer
                    && R_SUCCEEDED(Y2RU_StartConversion());
    if (!ok) {
        Y2RU_StopConversion();
        sY2R = Y2R_FAILED; // the rest of this video on the CPU
        return false;
    }
    sY2RBusy     = true;
    sY2ROut      = out;
    sY2ROutBytes = outN;
    ++sY2RFrames;
    return true;
}

} // namespace

uint16 *VideoFrame::Slot(int32 slot)
{
    if (slot < 0 || slot >= SLOTS)
        return NULL;
    if (!sSlots[slot]) {
        sSlots[slot] = (uint16 *)malloc(W * H * sizeof(uint16));
        if (sSlots[slot])
            memset(sSlots[slot], 0, W * H * sizeof(uint16)); // the black around the picture
    }
    return sSlots[slot];
}

void VideoFrame::SetTarget(int32 slot) { sTarget = slot; }

void VideoFrame::SetYUV(int32 width, int32 height, const uint8 *y, const uint8 *u, const uint8 *v, int32 strideY, int32 strideU, int32 strideV,
                        int32 chromaShiftX, int32 chromaShiftY)
{
    uint16 *dst = Slot(sTarget);
    if (width <= 0 || height <= 0 || !dst)
        return;
    if (!sTables)
        BuildTables();
    const u64 t0 = svcGetSystemTick();

    const Frame p = { width, height, y, u, v, strideY, strideU, strideV, chromaShiftX, chromaShiftY };
    int32 dw, dh, ox, oy;
    Fit(width, height, &dw, &dh, &ox, &oy);
    const bool native = dw == width && dh == height && chromaShiftX == 1 && chromaShiftY == 1;
    if (native && ox == 0 && ConvertY2R(p, dst, oy)) {
    }
    else if (native && !(width & 1) && !(height & 1) && !(ox & 1))
        Convert420Native(p, dst, ox, oy);
    else
        ConvertScaled(p, dst, dw, dh, ox, oy);

    sConvertTicks += svcGetSystemTick() - t0;
    ++sConversions;
}

void VideoFrame::SetImage(int32 width, int32 height, const uint32 *argb)
{
    uint16 *dst = Slot(0);
    if (width <= 0 || height <= 0 || !argb || !dst)
        return;

    int32 dw, dh, ox, oy;
    Fit(width, height, &dw, &dh, &ox, &oy);
    memset(dst, 0, W * H * sizeof(uint16));

    // Each screen pixel is the average of the source pixels under it (a 1024x512 image shrinks 2.56 times)
    for (int32 dy = 0; dy < dh; ++dy) {
        const int32 y0 = dy * height / dh;
        int32 y1       = (dy + 1) * height / dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        uint16 *out = dst + (oy + dy) * W + ox;
        for (int32 dx = 0; dx < dw; ++dx) {
            const int32 x0 = dx * width / dw;
            int32 x1       = (dx + 1) * width / dw;
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32 r = 0, g = 0, b = 0, n = 0;
            for (int32 sy = y0; sy < y1; ++sy) {
                const uint32 *row = argb + sy * width;
                for (int32 sx = x0; sx < x1; ++sx) {
                    const uint32 c = row[sx];
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    ++n;
                }
            }
            r /= n, g /= n, b /= n;
            out[dx] = (uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }

    Show(0);
}

void VideoFrame::Show(int32 slot)
{
    if (slot < 0 || slot >= SLOTS || !sSlots[slot])
        return;
    sShown    = slot;
    sDimmedAt = -1.0f;
    ++sVersion;
}

int32 VideoFrame::Shown() { return sShown; }

const uint16 *VideoFrame::Picture(float dim)
{
    const int32 shown = sShown;
    if (shown < 0 || !sSlots[shown])
        return NULL;
    const uint16 *pic = sSlots[shown];
    if (dim >= 1.0f)
        return pic;

    if (!sDimmed) {
        sDimmed = (uint16 *)malloc(W * H * sizeof(uint16));
        if (!sDimmed)
            return pic;
    }
    if (dim != sDimmedAt) {
        const uint32 k = dim <= 0.0f ? 0 : (uint32)(dim * 256.0f);
        for (int32 i = 0; i < W * H; ++i) {
            const uint32 c = pic[i];
            const uint32 r = (((c >> 11) & 0x1F) * k) >> 8;
            const uint32 g = (((c >> 5) & 0x3F) * k) >> 8;
            const uint32 b = ((c & 0x1F) * k) >> 8;
            sDimmed[i]     = (uint16)((r << 11) | (g << 5) | b);
        }
        sDimmedAt = dim;
    }
    return sDimmed;
}

uint32 VideoFrame::Version() { return sVersion; }

void VideoFrame::WaitConversion() { EndY2R(100 * 1000000LL); }
bool VideoFrame::ConversionDone() { return EndY2R(0); }

uint32 VideoFrame::Prepare()
{
    const u64 t0 = svcGetSystemTick();
    for (int32 i = 0; i < SLOTS; ++i) Slot(i);
    OpenY2R();
    return (uint32)((svcGetSystemTick() - t0) * 1000000ULL / SYSCLOCK_ARM11);
}

bool VideoFrame::HardwareReady() { return sY2R == Y2R_OPEN; }

void VideoFrame::Release()
{
    bool any = sDimmed || sY2R == Y2R_FAILED;
    for (int32 i = 0; i < SLOTS; ++i) any = any || sSlots[i];
    if (!any)
        return;
    // Y2R stays open: opening it with a video's first frame made that video's first frames late (an ending's second
    // clip starts the moment the first one ends). A failed one is tried again with the next video.
    EndY2R(100 * 1000000LL);
    if (sY2R == Y2R_FAILED)
        CloseY2R();
    for (int32 i = 0; i < SLOTS; ++i) {
        free(sSlots[i]);
        sSlots[i] = NULL;
    }
    free(sDimmed);
    sDimmed       = NULL;
    sDimmedAt     = -1.0f;
    sShown        = -1;
    sTarget       = 0;
    sConvertTicks = 0;
    sConversions  = 0;
    sY2RFrames    = 0;
    ++sVersion;
}

void VideoFrame::ResetStats()
{
    sConvertTicks = 0;
    sConversions  = 0;
    sY2RFrames    = 0;
}

uint64 VideoFrame::ConvertTicks() { return sConvertTicks; }
uint32 VideoFrame::Conversions() { return sConversions; }
uint32 VideoFrame::HardwareConversions() { return sY2RFrames; }

#endif // RETRO_PLATFORM == RETRO_3DS
