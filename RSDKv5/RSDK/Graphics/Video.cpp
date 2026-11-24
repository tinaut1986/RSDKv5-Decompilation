#include "RSDK/Core/RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_3DS
#include "RSDK/Dev/CTR/DevTools.hpp"
#include <errno.h>
#include "RSDK/Graphics/CTR/VideoFrame.hpp"
#endif

using namespace RSDK;

FileInfo VideoManager::file;

ogg_sync_state VideoManager::oy;
ogg_page VideoManager::og;
ogg_packet VideoManager::op;
ogg_stream_state VideoManager::vo;
ogg_stream_state VideoManager::to;
th_info VideoManager::ti;
th_comment VideoManager::tc;
th_dec_ctx *VideoManager::td    = NULL;
th_setup_info *VideoManager::ts = NULL;

th_pixel_fmt VideoManager::pixelFormat;
ogg_int64_t VideoManager::granulePos = 0;
bool32 VideoManager::initializing    = false;

#if RETRO_PLATFORM == RETRO_3DS
// One line in the log for each video played: what it cost to decode and to convert a frame, and how often the
// picture fell behind the music. The numbers to judge a video's size by on the Old 3DS.
namespace
{
struct VideoStats {
    char name[0x40];
    bool32 loose;        // the converted file next to Data.rsdk, not the pack's
    bool32 threaded;     // decoded by the decoder thread (else a frame a pass on the main thread)
    uint32 decoded;      // frames decoded
    uint32 duplicates;   // packets that repeat the previous frame
    uint32 undecoded;    // packets dropped undecoded while far behind (up to the next keyframe)
    uint32 skipped;      // decoded after their time was over: not converted
    uint32 shown;        // put on the screen
    uint32 dropped;      // converted, but a later frame was due by the time they would have been shown
    uint32 late;         // shown after their time was over
    uint64 decodeTicks;
    uint32 maxDecodeUs;
    uint64 startTick;
    uint64 lastTick;     // the previous pass (the clock the picture follows without music)
    uint32 clockPasses;  // passes timed by the clock: no music stream to follow
    uint32 passes;       // ProcessVideo calls
    uint32 lateBins[4];  // frames shown after their time by < 17, < 33 ms, less than a frame, more
    uint32 gapBins[3];   // passes that came > 25, > 50, > 100 ms after the previous one
    uint32 clockSnaps;   // times the smoothed clock jumped to the music's position (more than 250 ms apart)
    bool32 followingMusic; // the clock has taken the music's position once
    // The decoder thread's first steps: when each began (ms after LoadVideo), how long it took, frames decoded after it
    uint16 traceAt[16], traceMs[16], traceDecoded[16];
    uint8 traceResult[16];
    int32 traceCount;
    uint16 prefilled, prefillMs; // frames queued by LoadVideo before the thread started, and the time it took
    uint16 prefillStepMs[8];
    int32 prefillSteps;
    uint32 stepUs[5];            // the last DecodeStep: queueing, reading, decoding, waiting for Y2R, converting
    uint32 prefillPhase[5];      // stepUs after the prefill's first step
    uint16 firstFrameMs;         // LoadVideo's first frame (decoded and shown before the prefill)
    uint32 prepareUs;            // VideoFrame::Prepare: the slots and Y2R
    uint32 recentUs[8];  // the last decodes' times (the decoder thread), for the trouble spots
    uint32 recentAt;
};
VideoStats videoStats;

// Where the picture fell behind (frames decoded after their time, or shown more than a frame late), merged when less
// than 0.3 s apart: the stretches the player sees stop and jump. Written under the queue's lock.
struct VideoTrouble {
    float from, to;      // the video's clock (s)
    uint16 skipped, late;
    uint16 keyframes;    // keyframes decoded in the stretch
    uint32 maxDecodeUs;  // the slowest of the decodes just before
};
enum { VIDEO_TROUBLES = 24 };
VideoTrouble videoTroubles[VIDEO_TROUBLES];
int32 videoTroubleCount;

uint32 RecentMaxDecodeUs()
{
    uint32 m = 0;
    for (int32 i = 0; i < 8; ++i) m = videoStats.recentUs[i] > m ? videoStats.recentUs[i] : m;
    return m;
}

void NoteTrouble(double t, bool skipped, bool keyframe)
{
    VideoTrouble *last = videoTroubleCount ? &videoTroubles[videoTroubleCount - 1] : NULL;
    if (!last || t > last->to + 0.3) {
        if (videoTroubleCount >= VIDEO_TROUBLES)
            return;
        last = &videoTroubles[videoTroubleCount++];
        memset(last, 0, sizeof(*last));
        last->from = (float)t;
    }
    last->to = (float)t;
    if (skipped)
        ++last->skipped;
    else
        ++last->late;
    if (keyframe)
        ++last->keyframes;
    const uint32 m = RecentMaxDecodeUs();
    if (m > last->maxDecodeUs)
        last->maxDecodeUs = m;
}

// The frames decoded and converted ahead (VideoFrame's slots), oldest first. The decoder thread fills it; the main
// thread shows each at its time. One slot more than the queue holds is the one on the screen.
struct VideoQueue {
    int32 slot[VideoFrame::SLOTS];
    double start[VideoFrame::SLOTS]; // a frame's time on the screen (s, the video's clock)
    int32 head, count;
    double lastEnd;       // the end of the last frame queued
    volatile double streamEnd; // the end of the last packet read, duplicates too (a still stretch is coded as such)
    int64 packets;        // video packets read: a frame each, as libtheora counts them on the PC (the engine never
                          // sets the decoder's granule position, so the jumps in the pages' granule positions, such
                          // as the intro's first frame "held" 7 s, are not in the game: following them made the
                          // intro 7.5 s longer than its music). Counted here, as packets dropped undecoded count too.
    bool seeking;         // far behind: dropping packets up to the next keyframe still ahead of the clock
    int32 convSlot;       // a frame still being converted (Y2R), queued once done; -1: none
    double convStart, convEnd;
    volatile bool ended;  // no packet left in the file
    volatile bool stop;   // the main thread wants the decoder thread to stop
    volatile s32 clockMs; // the video's clock, for the decoder to skip frames already over
    LightLock lock;       // the queue and the slot on the screen
    LightEvent room;      // a slot was freed
    Thread thread;
};
VideoQueue videoQueue;

uint32 TicksToUs(uint64 ticks) { return (uint32)(ticks * 1000000ULL / SYSCLOCK_ARM11); }
} // namespace

static void StopVideoDecoder();
static void EndVideoConversions();
#endif

bool32 RSDK::LoadVideo(const char *filename, double startDelay, bool32 (*skipCallback)())
{
    if (ENGINE_VERSION == 5 && sceneInfo.state == ENGINESTATE_VIDEOPLAYBACK)
        return false;
#if RETRO_REV0U
    if (ENGINE_VERSION == 3 && RSDK::Legacy::gameMode == RSDK::Legacy::v3::ENGINE_VIDEOWAIT)
        return false;
#endif

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Video/%s", filename);

    InitFileInfo(&VideoManager::file);
#if RETRO_PLATFORM == RETRO_3DS
    // A decoder thread still running (a scene changed under a video, which only the dev tools do) would read the
    // new file too: it stops first.
    StopVideoDecoder();
    EndVideoConversions();
    // A copy converted for the 3DS (smaller, cheaper to decode) in Video/ next to Data.rsdk wins over the pack's
    bool32 opened = false;
    {
        char loosePath[0x100];
        sprintf_s(loosePath, sizeof(loosePath), "%sVideo/%s", SKU::userFileDir, filename);
        VideoManager::file.externalFile = true;
        errno                           = 0;
        opened                          = LoadFile(&VideoManager::file, loosePath, FMODE_RB);
        const int32 looseErrno          = errno;
        memset(&videoStats, 0, sizeof(videoStats));
        videoTroubleCount = 0;
        memset(&videoQueue, 0, sizeof(videoQueue));
        VideoFrame::ResetStats();
        LightLock_Init(&videoQueue.lock);
        LightEvent_Init(&videoQueue.room, RESET_ONESHOT);
        videoQueue.convSlot = -1;
        // Here, not in the decoder thread with the first frame. Nothing is written to the log until the video ends:
        // a write to the SD card held the next read of the video ~170 ms, and the start lost a dozen frames.
        videoStats.prepareUs = VideoFrame::Prepare();
        videoStats.loose = opened;
        if (!opened) {
            InitFileInfo(&VideoManager::file);
            opened = LoadFile(&VideoManager::file, fullFilePath, FMODE_RB);
        }
        snprintf(videoStats.name, sizeof(videoStats.name), "%s", filename);
        videoStats.startTick = svcGetSystemTick();
        videoStats.lastTick  = videoStats.startTick;
        if (!videoStats.loose)
            DEV_LOG("video %s: no %s (errno %d), %s", filename, loosePath, looseErrno, opened ? "the pack's" : "not in the pack either");
    }
    if (opened) {
#else
    if (LoadFile(&VideoManager::file, fullFilePath, FMODE_RB)) {
#endif
        // Init
        ogg_sync_init(&VideoManager::oy);

        th_comment_init(&VideoManager::tc);
        th_info_init(&VideoManager::ti);

        int32 theora_p = 0;
        char *buffer   = NULL;

        // Parse header stuff
        bool32 finishedHeader = false;
        while (!finishedHeader) {
            buffer    = ogg_sync_buffer(&VideoManager::oy, 0x1000);
            int32 ret = (int32)ReadBytes(&VideoManager::file, buffer, 0x1000);
            ogg_sync_wrote(&VideoManager::oy, 0x1000);

            if (ret == 0)
                break;

            while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) {
                ogg_stream_state test;

                /* is this a mandated initial header? If not, stop parsing */
                if (!ogg_page_bos(&VideoManager::og)) {
                    /* don't leak the page; get it into the appropriate stream */
                    ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
                    finishedHeader = true;
                    break;
                }

                ogg_stream_init(&test, ogg_page_serialno(&VideoManager::og));
                ogg_stream_pagein(&test, &VideoManager::og);
                ogg_stream_packetout(&test, &VideoManager::op);

                // identify codec
                if (!theora_p && th_decode_headerin(&VideoManager::ti, &VideoManager::tc, &VideoManager::ts, &VideoManager::op) >= 0) {
                    // theora
                    memcpy(&VideoManager::to, &test, sizeof(test));
                    theora_p = 1;
                }
                else {
                    // we dont care (possibly vorbis)
                    ogg_stream_clear(&test);
                }
            }
        }

        if (theora_p) {
            VideoManager::ts = NULL;
            theora_p         = 1;
            while (theora_p && theora_p < 3) {
                int32 ret;

                while (theora_p && (theora_p < 3) && (ret = ogg_stream_packetout(&VideoManager::to, &VideoManager::op))) {
                    if (ret < 0) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: failed to parse theora stream headers. corrupted stream?");
#endif

                        theora_p = 0;
                        break;
                    }

                    if (!th_decode_headerin(&VideoManager::ti, &VideoManager::tc, &VideoManager::ts, &VideoManager::op)) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: failed to parse theora stream headers. corrupted stream?");
#endif

                        theora_p = 0;
                        break;
                    }

                    theora_p++;
                }

                if (!theora_p)
                    break;

                /* The header pages/packets will arrive before anything else we
                   care about, or the stream is not obeying spec */

                if (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) {
                    ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
                }
                else {
                    buffer    = ogg_sync_buffer(&VideoManager::oy, 0x1000);
                    int32 ret = (int32)ReadBytes(&VideoManager::file, buffer, 0x1000);
                    ogg_sync_wrote(&VideoManager::oy, 0x1000);
                    if (ret == 0) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: Reached end of file while searching for codec headers.");
#endif

                        theora_p = 0;
                    }
                }
            }

            if (!theora_p) {
                th_info_clear(&VideoManager::ti);
                th_comment_clear(&VideoManager::tc);
                th_setup_free(VideoManager::ts);
            }
            else {
                VideoManager::td          = th_decode_alloc(&VideoManager::ti, VideoManager::ts);
                VideoManager::pixelFormat = VideoManager::ti.pixel_fmt;

                int32 ppLevelMax = 0;
                th_decode_ctl(VideoManager::td, TH_DECCTL_GET_PPLEVEL_MAX, &ppLevelMax, sizeof(int32));
                int32 ppLevel = 0;
                th_decode_ctl(VideoManager::td, TH_DECCTL_SET_PPLEVEL, &ppLevel, sizeof(int32));

                th_setup_free(VideoManager::ts);

                engine.storedShaderID     = videoSettings.shaderID;
                videoSettings.screenCount = 0;

                if (ENGINE_VERSION == 5)
                    engine.storedState = sceneInfo.state;
#if RETRO_REV0U
                else if (ENGINE_VERSION == 3)
                    engine.storedState = RSDK::Legacy::gameMode;
#endif
                engine.displayTime         = 0.0;
                VideoManager::initializing = true;
                VideoManager::granulePos   = 0;

                engine.displayTime     = 0.0;
                engine.videoStartDelay = 0.0;
                if (AudioDevice::audioState == 1)
                    engine.videoStartDelay = startDelay;

                switch (VideoManager::pixelFormat) {
                    default: break;
                    case TH_PF_420: videoSettings.shaderID = SHADER_YUV_420; break;
                    case TH_PF_422: videoSettings.shaderID = SHADER_YUV_422; break;
                    case TH_PF_444: videoSettings.shaderID = SHADER_YUV_444; break;
                }

                engine.skipCallback = NULL;
                ProcessVideo();
                engine.skipCallback = skipCallback;

                changedVideoSettings = false;
                if (ENGINE_VERSION == 5)
                    sceneInfo.state = ENGINESTATE_VIDEOPLAYBACK;
#if RETRO_REV0U
                else if (ENGINE_VERSION == 3)
                    RSDK::Legacy::gameMode = RSDK::Legacy::v3::ENGINE_VIDEOWAIT;
#endif

                return true;
            }
        }

        CloseFile(&VideoManager::file);
    }

    return false;
}

#if RETRO_PLATFORM == RETRO_3DS
// The frame just decoded into the target slot (VideoFrame converts it now). The picture's own size (pic_width x
// pic_height), not the coded frame's: a 400x200 video is coded as 400x208.
static void ShowVideoFrame()
{
    th_ycbcr_buffer yuv;
    th_decode_ycbcr_out(VideoManager::td, yuv);

    const th_info &ti   = VideoManager::ti;
    const int32 w       = (int32)ti.pic_width;
    const int32 h       = (int32)ti.pic_height;
    const int32 px      = (int32)(ti.pic_x & 0xFFFFFFFE);
    const int32 py      = (int32)(ti.pic_y & 0xFFFFFFFE);
    const int32 dataPos = px + py * yuv[0].stride;
    switch (VideoManager::pixelFormat) {
        default: break;

        case TH_PF_444:
            RenderDevice::SetupVideoTexture_YUV444(w, h, &yuv[0].data[dataPos], &yuv[1].data[px + py * yuv[1].stride],
                                                   &yuv[2].data[px + py * yuv[2].stride], yuv[0].stride, yuv[1].stride, yuv[2].stride);
            break;

        case TH_PF_422:
            RenderDevice::SetupVideoTexture_YUV422(w, h, &yuv[0].data[dataPos], &yuv[1].data[yuv[1].stride * py + (px >> 1)],
                                                   &yuv[2].data[yuv[2].stride * py + (px >> 1)], yuv[0].stride, yuv[1].stride, yuv[2].stride);
            break;

        case TH_PF_420:
            RenderDevice::SetupVideoTexture_YUV420(w, h, &yuv[0].data[dataPos], &yuv[1].data[yuv[1].stride * (py >> 1) + (px >> 1)],
                                                   &yuv[2].data[yuv[2].stride * (py >> 1) + (px >> 1)], yuv[0].stride, yuv[1].stride,
                                                   yuv[2].stride);
            break;
    }
}

// The next packet of the video stream into VideoManager::op; false at the end of the file.
// Read 64 KB at a time, not the engine's 4 KB: a read from the SD card costs ~10 ms however small, and ffmpeg puts a
// second of video in each Ogg page, so the second frame waited for ~15 reads (120-185 ms) and a clip's start lost a
// dozen frames. Only what was read is handed to Ogg (the engine told it 4 KB came even at the end of the file).
static bool32 NextVideoPacket()
{
    while (ogg_stream_packetout(&VideoManager::to, &VideoManager::op) <= 0) {
        char *buffer       = ogg_sync_buffer(&VideoManager::oy, 0x10000);
        const size_t bytes = ReadBytes(&VideoManager::file, buffer, 0x10000);
        if (!bytes)
            return false;

        ogg_sync_wrote(&VideoManager::oy, (long)bytes);

        while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
    }
    return true;
}

// The frame being converted into the queue, once the conversion is over (wait: until it is).
static void QueueConverted(bool wait)
{
    VideoQueue &q = videoQueue;
    if (q.convSlot < 0)
        return;
    if (wait)
        VideoFrame::WaitConversion();
    else if (!VideoFrame::ConversionDone())
        return;
    LightLock_Lock(&q.lock);
    const int32 at = (q.head + q.count) % VideoFrame::SLOTS;
    q.slot[at]     = q.convSlot;
    q.start[at]    = q.convStart;
    q.lastEnd      = q.convEnd;
    ++q.count;
    LightLock_Unlock(&q.lock);
    q.convSlot = -1;
}

// Decodes the next frame into a free slot; it is queued once converted (the Y2R conversion runs on while the next
// frame decodes). False when there is nothing to do now (no free slot) or the file has ended.
static bool32 DecodeStep()
{
    VideoQueue &q = videoQueue;
    const u64 p0 = svcGetSystemTick();
    QueueConverted(false);
    if (q.ended)
        return false;

    int32 freeSlot = -1;
    LightLock_Lock(&q.lock);
    if (q.count + (q.convSlot >= 0) < VideoFrame::SLOTS - 1) {
        bool used[VideoFrame::SLOTS] = {};
        if (VideoFrame::Shown() >= 0)
            used[VideoFrame::Shown()] = true;
        if (q.convSlot >= 0)
            used[q.convSlot] = true;
        for (int32 i = 0; i < q.count; ++i) used[q.slot[(q.head + i) % VideoFrame::SLOTS]] = true;
        for (int32 i = 0; i < VideoFrame::SLOTS && freeSlot < 0; ++i) {
            if (!used[i] && VideoFrame::Slot(i))
                freeSlot = i;
        }
    }
    LightLock_Unlock(&q.lock);
    if (freeSlot < 0)
        return false;

    const u64 p1 = svcGetSystemTick();
    if (!NextVideoPacket()) {
        QueueConverted(true);
        q.ended = true;
        FILE *f = VideoManager::file.usingFileBuffer ? NULL : (FILE *)VideoManager::file.file;
        DEV_LOG("video %s: end of the file at %d of %d bytes (eof %d, error %d, errno %d), %u decoded", videoStats.name,
                VideoManager::file.readPos, VideoManager::file.fileSize, f ? feof(f) : -1, f ? ferror(f) : -1, errno, videoStats.decoded);
        return false;
    }

    const th_info &ti      = VideoManager::ti;
    const double frameTime = ti.fps_numerator ? (double)ti.fps_denominator / ti.fps_numerator : 1.0 / 30.0;
    const int32 keyframe   = th_packet_iskeyframe(&VideoManager::op);
    const double end       = (double)(++q.packets) * frameTime; // this frame's end
    const double clock     = q.clockMs / 1000.0;
    q.streamEnd            = end;

    // Far behind (a video too heavy for the console, such as the pack's 1024x512 ones on an Old 3DS): the packets up
    // to the next keyframe still ahead of the clock are dropped without decoding, so the picture keeps to the music
    // in jumps instead of running later and later (and the video lasting far longer than its music).
    if (q.seeking) {
        if (keyframe != 1 || end <= clock + 0.1) {
            ++videoStats.undecoded;
            return true;
        }
        q.seeking = false;
    }

    const u64 t0     = svcGetSystemTick();
    videoStats.stepUs[0] = TicksToUs(p1 - p0);
    videoStats.stepUs[1] = TicksToUs(t0 - p1);
    const int32 ret  = th_decode_packetin(VideoManager::td, &VideoManager::op, &VideoManager::granulePos);
    const u64 dt     = svcGetSystemTick() - t0;
    const uint32 dUs = TicksToUs(dt);
    videoStats.decodeTicks += dt;
    if (dUs > videoStats.maxDecodeUs)
        videoStats.maxDecodeUs = dUs;
    videoStats.recentUs[videoStats.recentAt++ & 7] = dUs;
    if (ret == TH_DUPFRAME) { // the picture stays for another frame
        ++videoStats.duplicates;
        return true;
    }
    if (ret != 0) // a bad packet
        return true;
    ++videoStats.decoded;

    if (end <= clock) {
        ++videoStats.skipped; // already over: not worth converting
        LightLock_Lock(&q.lock);
        NoteTrouble(clock, true, keyframe == 1);
        LightLock_Unlock(&q.lock);
        if (clock - end > 0.5)
            q.seeking = true;
        return true;
    }
    const u64 c0 = svcGetSystemTick();
    videoStats.stepUs[2] = TicksToUs(c0 - t0);
    QueueConverted(true); // the previous frame's conversion reads the staging copy this one is about to fill
    const u64 c1 = svcGetSystemTick();
    VideoFrame::SetTarget(freeSlot);
    ShowVideoFrame();
    videoStats.stepUs[3] = TicksToUs(c1 - c0);
    videoStats.stepUs[4] = TicksToUs(svcGetSystemTick() - c1);
    q.convSlot  = freeSlot;
    q.convStart = end - frameTime;
    q.convEnd   = end;
    if (VideoManager::initializing)
        QueueConverted(true); // the first frame goes on the screen at once
    return true;
}

// Below the main thread on the same core: it decodes while the main thread sleeps until its next tick (the
// kernel gives the core back to the main thread as soon as that wakes).
static void VideoDecodeThread(void *)
{
    VideoQueue &q = videoQueue;
    while (!q.stop) {
        const u64 t0     = svcGetSystemTick();
        const bool32 got = DecodeStep();
        if (videoStats.traceCount < 16) {
            const int32 i              = videoStats.traceCount++;
            videoStats.traceAt[i]      = (uint16)(TicksToUs(t0 - videoStats.startTick) / 1000);
            videoStats.traceMs[i]      = (uint16)(TicksToUs(svcGetSystemTick() - t0) / 1000);
            videoStats.traceDecoded[i] = (uint16)videoStats.decoded;
            videoStats.traceResult[i]  = got ? 1 : 0;
        }
        if (!got) {
            if (q.ended)
                break;
            // Nothing to decode until a slot is free: a frame still converting is queued as soon as it is done
            LightEvent_WaitTimeout(&q.room, q.convSlot >= 0 ? 2 * 1000000LL : 20 * 1000000LL);
        }
    }
}

static void StartVideoDecoder()
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (prio < 0x3F)
        videoQueue.thread = threadCreate(VideoDecodeThread, NULL, 0x10000, prio + 1, 0, false);
    videoStats.threaded = videoQueue.thread != NULL;
    if (!videoQueue.thread)
        DEV_LOG("video %s: no decoder thread, a frame a pass on the main thread", videoStats.name);
}

static void StopVideoDecoder()
{
    VideoQueue &q = videoQueue;
    if (!q.thread)
        return;
    q.stop = true;
    LightEvent_Signal(&q.room);
    threadJoin(q.thread, U64_MAX);
    threadFree(q.thread);
    q.thread = NULL;
}

// Before the decoder is freed: no conversion may still be running
static void EndVideoConversions()
{
    VideoFrame::WaitConversion();
    videoQueue.convSlot = -1;
}

// Unlike the original (which decodes each frame on the main thread once its time has come, so a slow frame makes
// the next ones late), a thread decodes and converts a few frames ahead (VideoQueue) and the main thread puts each
// on the screen at its time; a frame the decoder finishes after its time is not converted, so a slow stretch costs
// pictures, not sync.
void RSDK::ProcessVideo()
{
    VideoQueue &q   = videoQueue;
    bool32 finished = false;
    const u64 now   = svcGetSystemTick();

    if (VideoManager::initializing) {
        // LoadVideo: the first frame on the screen now, then the decoder
        q.clockMs        = -1000000;
        const u64 first0 = svcGetSystemTick();
        while (!q.count && DecodeStep()) {
        }
        videoStats.firstFrameMs = (uint16)(TicksToUs(svcGetSystemTick() - first0) / 1000);
        if (q.count) {
            VideoFrame::Show(q.slot[q.head]);
            q.head  = (q.head + 1) % VideoFrame::SLOTS;
            --q.count;
            ++videoStats.shown;
        }
        VideoManager::initializing = false;

        // A few frames more now, on this thread: the decoder thread gets little of the core for the next 0.1-0.2 s
        // (the main thread finishes the scene's frame), and an ending's second clip starts with its music already
        // running, so it began a quarter of a second behind and lost 8-11 frames. Decoding runs faster than the
        // frames last (12-20 ms for 33 or 42), so the time spent here is won back.
        const u64 t0 = svcGetSystemTick();
        for (;;) {
            if (q.count + (q.convSlot >= 0) >= 6 || TicksToUs(svcGetSystemTick() - t0) >= 120000)
                break;
            const u64 s0     = svcGetSystemTick();
            const bool32 got = DecodeStep();
            if (!videoStats.prefillSteps)
                memcpy(videoStats.prefillPhase, videoStats.stepUs, sizeof(videoStats.stepUs));
            if (videoStats.prefillSteps < 8)
                videoStats.prefillStepMs[videoStats.prefillSteps++] = (uint16)(TicksToUs(svcGetSystemTick() - s0) / 1000);
            if (!got)
                break;
        }
        QueueConverted(true);
        videoStats.prefilled  = (uint16)q.count;
        videoStats.prefillMs  = (uint16)(TicksToUs(svcGetSystemTick() - t0) / 1000);

        q.clockMs = 0;
        StartVideoDecoder();
        return;
    }

    // The video's clock runs on the real time that passed (not 1/60 s a pass: a slow pass would slow the video down)
    // and is pulled gently towards the music's position. That position moves in steps of a mixer buffer (23 ms, at
    // times several at once): followed as it is, two frames 41.7 ms apart came due in one pass and one was lost.
    const double streamPos = GetVideoStreamPos();
    const double dt        = (double)(now - videoStats.lastTick) / SYSCLOCK_ARM11;
    if (streamPos <= -1.0) {
        engine.displayTime += dt;
        ++videoStats.clockPasses;
    }
    else if (!videoStats.followingMusic) {
        // The first pass with music (the video's first, or the music just started): the clock takes its position as
        // it is. Moved on by the real time instead, it ran ahead by however long the music took to load (240 ms for
        // an ending's: its first 9 frames were then over and skipped).
        engine.displayTime        = streamPos;
        videoStats.followingMusic = true;
    }
    else {
        const double prev = engine.displayTime;
        double next       = prev + dt;
        const double off  = streamPos - next;
        if (off > 0.25 || off < -0.25) {
            next = streamPos; // a jump (the music started, or the video fell far behind): follow it
            ++videoStats.clockSnaps;
        }
        else
            next += off * 0.05; // ~1/3 s to close a gap at 60 passes a second
        engine.displayTime = next > prev || off > 0.25 || off < -0.25 ? next : prev;
    }

    if (engine.skipCallback && engine.skipCallback())
        finished = true;

    if (videoStats.passes) {
        const uint32 gapUs = TicksToUs(now - videoStats.lastTick);
        videoStats.gapBins[0] += gapUs > 25000;
        videoStats.gapBins[1] += gapUs > 50000;
        videoStats.gapBins[2] += gapUs > 100000;
    }
    videoStats.lastTick = now;
    ++videoStats.passes;

    const th_info &ti      = VideoManager::ti;
    const double frameTime = ti.fps_numerator ? (double)ti.fps_denominator / ti.fps_numerator : 1.0 / 30.0;
    const double clock     = engine.displayTime - engine.videoStartDelay;
    q.clockMs              = (s32)(clock * 1000.0);

    // The newest queued frame whose time has come goes on the screen (any older one due too is dropped). Shown under
    // the lock: the decoder thread must not take the slot as free in between.
    bool32 freed = false;
    LightLock_Lock(&q.lock);
    int32 popped       = -1;
    double poppedStart = 0.0;
    while (q.count && q.start[q.head] <= clock) {
        if (popped >= 0)
            ++videoStats.dropped;
        popped      = q.slot[q.head];
        poppedStart = q.start[q.head];
        q.head      = (q.head + 1) % VideoFrame::SLOTS;
        --q.count;
        freed = true;
    }
    if (popped >= 0) {
        VideoFrame::Show(popped);
        ++videoStats.shown;
        const double lateBy = clock - poppedStart;
        ++videoStats.lateBins[lateBy < 0.017 ? 0 : lateBy < 0.033 ? 1 : lateBy < frameTime ? 2 : 3];
        if (lateBy >= frameTime) {
            ++videoStats.late;
            NoteTrouble(clock, false, false);
        }
    }
    const bool32 drained = q.ended && !q.count && q.convSlot < 0 && clock >= (q.streamEnd > q.lastEnd ? q.streamEnd : q.lastEnd);
    LightLock_Unlock(&q.lock);
    if (freed)
        LightEvent_Signal(&q.room);

    if (!q.thread)
        DecodeStep(); // no thread: a frame a pass
    if (drained)
        finished = true;

    if (finished) {
        StopVideoDecoder();
        EndVideoConversions();
        DEV_LOG("video %s: ends with the music at %.2f s (the video's start delay %.2f s, its last frame's end %.2f s)", videoStats.name,
                GetVideoStreamPos(), engine.videoStartDelay, q.streamEnd > q.lastEnd ? q.streamEnd : q.lastEnd);

        const uint32 frames = videoStats.decoded ? videoStats.decoded : 1;
        const uint32 convs  = VideoFrame::Conversions() ? VideoFrame::Conversions() : 1;
        DEV_LOG("video %s (%s, %ux%u %.2f fps, %s): %u decoded (+%u repeats, %u dropped undecoded), %u skipped, %u shown, %u dropped, %u late in %u ms (%u passes, %u on "
                "the clock, %u clock jumps)",
                videoStats.name, videoStats.loose ? "Video/" : "pack", ti.pic_width, ti.pic_height,
                ti.fps_denominator ? (double)ti.fps_numerator / ti.fps_denominator : 0.0, videoStats.threaded ? "thread" : "main",
                videoStats.decoded, videoStats.duplicates, videoStats.undecoded, videoStats.skipped, videoStats.shown, videoStats.dropped, videoStats.late,
                TicksToUs(svcGetSystemTick() - videoStats.startTick) / 1000, videoStats.passes, videoStats.clockPasses, videoStats.clockSnaps);
        DEV_LOG("video %s: decode avg %u us max %u us, convert avg %u us (%u by Y2R); shown late by <17/<33/<frame/more ms: "
                "%u/%u/%u/%u; passes after >25/>50/>100 ms: %u/%u/%u",
                videoStats.name, TicksToUs(videoStats.decodeTicks) / frames, videoStats.maxDecodeUs,
                TicksToUs(VideoFrame::ConvertTicks()) / convs, VideoFrame::HardwareConversions(), videoStats.lateBins[0],
                videoStats.lateBins[1], videoStats.lateBins[2], videoStats.lateBins[3], videoStats.gapBins[0], videoStats.gapBins[1],
                videoStats.gapBins[2]);

        {
            char line[0x100];
            int32 at = snprintf(line, sizeof(line), "video %s: slots and Y2R %u us (Y2R %s), first frame %u ms, %u ahead in %u ms (steps",
                                videoStats.name, videoStats.prepareUs, VideoFrame::HardwareReady() ? "open" : "closed", videoStats.firstFrameMs,
                                videoStats.prefilled, videoStats.prefillMs);
            for (int32 i = 0; i < videoStats.prefillSteps; ++i) at += snprintf(line + at, sizeof(line) - at, " %u", videoStats.prefillStepMs[i]);
            at += snprintf(line + at, sizeof(line) - at, "; first: queue %u read %u decode %u wait %u convert %u us)", videoStats.prefillPhase[0],
                           videoStats.prefillPhase[1], videoStats.prefillPhase[2], videoStats.prefillPhase[3], videoStats.prefillPhase[4]);
            DEV_LOG("%s", line);
            at = snprintf(line, sizeof(line), "video %s thread (ms after LoadVideo: took, decoded):", videoStats.name);
            for (int32 i = 0; i < videoStats.traceCount && at < (int32)sizeof(line) - 24; ++i)
                at += snprintf(line + at, sizeof(line) - at, " %u:%u,%u%s", videoStats.traceAt[i], videoStats.traceMs[i],
                               videoStats.traceDecoded[i], videoStats.traceResult[i] ? "" : "w");
            DEV_LOG("%s", line);
        }
        for (int32 i = 0; i < videoTroubleCount; i += 3) {
            char line[0x100];
            int32 at = snprintf(line, sizeof(line), "video %s behind:", videoStats.name);
            for (int32 j = i; j < i + 3 && j < videoTroubleCount; ++j) {
                const VideoTrouble &t = videoTroubles[j];
                at += snprintf(line + at, sizeof(line) - at, " %.2f-%.2f s (%u skipped, %u late, %u key, decode up to %u ms)", t.from, t.to,
                               t.skipped, t.late, t.keyframes, t.maxDecodeUs / 1000);
            }
            DEV_LOG("%s", line);
        }

        CloseFile(&VideoManager::file);

        // Flush everything out
        while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);

        ogg_stream_clear(&VideoManager::to);
        th_decode_free(VideoManager::td);
        th_comment_clear(&VideoManager::tc);
        th_info_clear(&VideoManager::ti);
        ogg_sync_clear(&VideoManager::oy);

        videoSettings.shaderID    = engine.storedShaderID;
        videoSettings.screenCount = 1;
        if (ENGINE_VERSION == 5)
            sceneInfo.state = engine.storedState;
    }
}
#else
void RSDK::ProcessVideo()
{
    bool32 finished = false;
    double curTime  = 0;
    if (!VideoManager::initializing) {
        double streamPos = GetVideoStreamPos();

        if (streamPos <= -1.0)
            engine.displayTime += (1.0 / 60.0); // deltaTime frame-step
        else
            engine.displayTime = streamPos;

        curTime = th_granule_time(VideoManager::td, VideoManager::granulePos);

#if RETRO_USE_MOD_LOADER
        RunModCallbacks(MODCB_ONVIDEOSKIPCB, (void *)engine.skipCallback);
#endif
        if (engine.skipCallback && engine.skipCallback()) {
            finished = true;
        }
    }

    if (!finished && (VideoManager::initializing || engine.displayTime >= engine.videoStartDelay + curTime)) {
        while (ogg_stream_packetout(&VideoManager::to, &VideoManager::op) <= 0) {
            char *buffer = ogg_sync_buffer(&VideoManager::oy, 0x1000);
            // if we're playing and reached the end of file
            if (!ReadBytes(&VideoManager::file, buffer, 0x1000) && !VideoManager::initializing) {
                finished = true;
                break;
            }

            ogg_sync_wrote(&VideoManager::oy, 0x1000);

            while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
        }

        if (!finished && !th_decode_packetin(VideoManager::td, &VideoManager::op, &VideoManager::granulePos)) {
            th_ycbcr_buffer yuv;
            th_decode_ycbcr_out(VideoManager::td, yuv);

            int32 dataPos = (VideoManager::ti.pic_x & 0xFFFFFFFE) + (VideoManager::ti.pic_y & 0xFFFFFFFE) * yuv[0].stride;
            switch (VideoManager::pixelFormat) {
                default: break;

                case TH_PF_444:
                    RenderDevice::SetupVideoTexture_YUV444(yuv[0].width, yuv[0].height, &yuv[0].data[dataPos], &yuv[1].data[dataPos],
                                                           &yuv[2].data[dataPos], yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_422:
                    RenderDevice::SetupVideoTexture_YUV422(yuv[0].width, yuv[0].height, &yuv[0].data[dataPos],
                                                           &yuv[1].data[yuv[1].stride * VideoManager::ti.pic_y + (VideoManager::ti.pic_x >> 1)],
                                                           &yuv[2].data[yuv[1].stride * VideoManager::ti.pic_y + (VideoManager::ti.pic_x >> 1)],
                                                           yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_420:
                    RenderDevice::SetupVideoTexture_YUV420(
                        yuv[0].width, yuv[0].height, &yuv[0].data[dataPos],
                        &yuv[1].data[yuv[1].stride * (VideoManager::ti.pic_y >> 1) + (VideoManager::ti.pic_x >> 1)],
                        &yuv[2].data[yuv[1].stride * (VideoManager::ti.pic_y >> 1) + (VideoManager::ti.pic_x >> 1)], yuv[0].stride, yuv[1].stride,
                        yuv[2].stride);
                    break;
            }
        }

        VideoManager::initializing = false;
    }

    if (finished) {
        CloseFile(&VideoManager::file);

        // Flush everything out
        while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);

        ogg_stream_clear(&VideoManager::to);
        th_decode_free(VideoManager::td);
        th_comment_clear(&VideoManager::tc);
        th_info_clear(&VideoManager::ti);
        ogg_sync_clear(&VideoManager::oy);

        videoSettings.shaderID    = engine.storedShaderID;
        videoSettings.screenCount = 1;
        if (ENGINE_VERSION == 5)
            sceneInfo.state = engine.storedState;
#if RETRO_REV0U
        else if (ENGINE_VERSION == 3)
            RSDK::Legacy::gameMode = engine.storedState;
#endif
    }
}
#endif // RETRO_PLATFORM == RETRO_3DS
