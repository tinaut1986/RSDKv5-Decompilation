using namespace RSDK;

#include <thread>

#if !SAMPLE_USE_S16
#error "ERROR: Incompatible sample format for 3DS builds."
#endif

#if SAMPLE_USE_S16
#define TARGET_OUT_FORMAT (NDSP_FORMAT_STEREO_PCM16)
#endif

// heavily referenced:  https://github.com/devkitPro/3ds-examples/blob/master/audio/opus-decoding/source/main.c

#define SAMPLE_RATE         (AUDIO_FREQUENCY)
#define SAMPLES_PER_BUF     (SAMPLE_RATE * 60 / 1000)
#define CHANNELS_PER_SAMPLE (2)
#define THREAD_STACK_SZ     (32 * 1024)
#define WAVEBUF_COUNT       (3)

static const int WAVEBUF_SIZE = SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE *
                                sizeof(int16);

Thread audioThreadHandle;
Handle audioThreadRequest;

volatile bool threadRunning;
volatile u64 ctrAudioMixTicks; // time spent mixing (SITE C3D: the audio thread's share of its core)
volatile u64 ctrAudioDecodeTicks; // time the music thread spent decoding the Ogg music
volatile u32 ctrAudioUnderruns;   // wakes that found every buffer played (the DSP ran dry)
volatile u32 ctrMusicStarved;     // music buffers the mixer found the decoder had not filled (heard as a gap)
int ctrAudioCore;               // the core the music thread runs on
u32 ctrAudioCore1Limit;         // the share of core 1 APT granted (%)

// ---------------------------------------------------------------- the music decoder
// Decoding the Ogg music (stb_vorbis) was ~3/4 of the audio thread's time, and the audio thread ~20 % of core 0's on
// the 2DS (2026-10-23, Special Stage 2 run). The music is decoded ahead by a thread of its own on core 1 (the system
// core, of which an application gets the share APT grants) into a ring; the mixing stays on core 0 and only copies
// from it. The ring (~0.37 s) rides out the system's own work on core 1. The share stays at 30 % (the exheader's
// MaxCpu, 0x9E): at 80 % the system services there starved (a Special Stage load took 70-87 s instead of 28, and
// the GPU's replies were slower). The whole audio thread on core 1 at 30 % did not keep up (the music crackled).
// Only one stream decodes at a time (the engine has one vorbisInfo); LoadStream swaps it under sMusicLock.

#define MUSIC_RING (32768) // int16 samples (16384 stereo frames, 0.37 s); a power of two (the counters wrap)
// The ring lives at the end of the stream's stb_vorbis buffer (music storage, LoadStream), not in the heap: a static
// 64 KB array left zlib no heap to inflate a scene's layouts (the Special Stage drawn with tile 0, 2026-10-23).
#define MUSIC_RING_BYTES (MUSIC_RING * sizeof(int16))
static int16 *volatile sMusicRing;
static volatile u32 sMusicWrite, sMusicRead; // samples written / read since boot (the decoder / the mixer)
static volatile bool sMusicEnded;            // the stream ended (no loop): nothing comes after sMusicWrite
static volatile bool sMusicLoop;             // the stream loops back to streamLoopPoint
static volatile int32 sMusicLocAtWrite;      // stb_vorbis's position (frames) at sMusicWrite
static LightLock sMusicLock;                 // vorbisInfo: the decoder vs LoadStream
static LightEvent sMusicWake;                // the mixer took a buffer
static Thread sMusicThread;
static volatile bool sMusicRun;

// Decodes up to `frames` stereo frames into the ring (sMusicLock held). False when the stream ended.
static bool MusicDecode(unsigned frames)
{
  int16 *const ring = sMusicRing;
  int empty = 0;
  while (frames && vorbisInfo && ring) {
    const u32 at = sMusicWrite % MUSIC_RING;
    unsigned n = (MUSIC_RING - at) / 2;
    if (n > frames)
      n = frames;
    const u64 t0 = svcGetSystemTick();
    const int got = stb_vorbis_get_samples_short_interleaved(vorbisInfo, 2, ring + at, (int)n * 2);
    ctrAudioDecodeTicks += svcGetSystemTick() - t0;
    if (got <= 0) {
      if (sMusicLoop && ++empty < 2 && stb_vorbis_seek_frame(vorbisInfo, streamLoopPoint))
        continue;
      sMusicEnded = true;
      return false;
    }
    empty = 0;
    int16 *p = ring + at;
    for (int i = 0; i < got * 2; ++i)
      p[i] >>= 1; // the engine plays the music at half volume (UpdateStreamBuffer)
    __sync_synchronize(); // the samples before the counter that publishes them
    sMusicLocAtWrite = vorbisInfo->current_loc_valid ? vorbisInfo->current_loc : 0;
    sMusicWrite += (u32)got * 2;
    frames -= (unsigned)got;
  }
  return true;
}

static void MusicThread(void *)
{
  while (sMusicRun) {
    bool more = false;
    LightLock_Lock(&sMusicLock);
    if (vorbisInfo && !sMusicEnded) {
      const u32 room = MUSIC_RING - (sMusicWrite - sMusicRead);
      if (room >= 2048) {
        MusicDecode(1024); // a chunk at a time: LoadStream may be waiting for the lock
        more = true;
      }
    }
    LightLock_Unlock(&sMusicLock);
    if (!more)
      LightEvent_WaitTimeout(&sMusicWake, 20 * 1000000LL);
  }
}

static void CtrMusicStart(s32 prio)
{
  LightLock_Init(&sMusicLock);
  LightEvent_Init(&sMusicWake, RESET_ONESHOT);
  u32 core1Limit = 0;
  const Result rc = APT_SetAppCpuTimeLimit(30);
  if (R_SUCCEEDED(rc))
    APT_GetAppCpuTimeLimit(&core1Limit);
  sMusicRun    = true;
  ctrAudioCore = core1Limit ? 1 : 0;
  sMusicThread = threadCreate(MusicThread, NULL, THREAD_STACK_SZ, prio, ctrAudioCore, false);
  if (!sMusicThread && ctrAudioCore) {
    ctrAudioCore = 0;
    sMusicThread = threadCreate(MusicThread, NULL, THREAD_STACK_SZ, prio, 0, false);
  }
  ctrAudioCore1Limit = core1Limit;
  DEV_LOG("audio: music decoder on core %d (core 1 share %lu %%, APT result %08lX)", ctrAudioCore, (unsigned long)core1Limit,
          (unsigned long)rc);
}

static void CtrMusicStop()
{
  sMusicRun = false;
  LightEvent_Signal(&sMusicWake);
  if (sMusicThread) {
    threadJoin(sMusicThread, UINT64_MAX);
    threadFree(sMusicThread);
    sMusicThread = NULL;
  }
}

// LoadStream: the stream about to be swapped (the decoder must not touch vorbisInfo or its buffer meanwhile, and the
// mixer plays silence until the new ring is set).
static void CtrMusicLock()
{
  LightLock_Lock(&sMusicLock);
  sMusicRing = NULL;
}

// LoadStream, after opening the new stream (lock held): drop what was buffered, decode the first half of the ring
// here (the loading thread, core 0: the decoder's share of core 1 would take a while to fill it), unlock.
static void CtrMusicBegin(bool loop, uint8 *ring)
{
  sMusicRing  = (int16 *)ring;
  sMusicLoop  = loop;
  sMusicEnded = false;
  sMusicRead  = sMusicWrite;
  MusicDecode(MUSIC_RING / 4);
  LightLock_Unlock(&sMusicLock);
  LightEvent_Signal(&sMusicWake); // fill the rest of the ring now
}

// The mixer's UpdateStreamBuffer: the next MIX_BUFFER_SIZE samples of the music from the ring.
static void CtrMusicRead(ChannelInfo *channel)
{
  int16 *dst        = channel->samplePtr;
  const int16 *ring = sMusicRing;
  if (!ring) { // a stream being swapped
    memset(dst, 0, MIX_BUFFER_SIZE * sizeof(int16));
    return;
  }
  const u32 avail = sMusicWrite - sMusicRead;
  const bool ended = sMusicEnded;
  __sync_synchronize(); // the counter before the samples it published
  const u32 n = avail < MIX_BUFFER_SIZE ? avail : MIX_BUFFER_SIZE;
  const u32 at = sMusicRead % MUSIC_RING, first = MUSIC_RING - at < n ? MUSIC_RING - at : n;
  memcpy(dst, ring + at, first * sizeof(int16));
  memcpy(dst + first, ring, (n - first) * sizeof(int16));
  if (n < MIX_BUFFER_SIZE) {
    memset(dst + n, 0, (MIX_BUFFER_SIZE - n) * sizeof(int16));
    if (ended && avail == n) {
      channel->state   = CHANNEL_IDLE;
      channel->soundID = -1;
    }
    else
      ++ctrMusicStarved;
  }
  sMusicRead += n;
  LightEvent_Signal(&sMusicWake);
}

// GetChannelPos: the music's position at what the mixer takes next (the decoder is ahead by what the ring holds).
static uint32 CtrMusicPos()
{
  const int32 pos = sMusicLocAtWrite - (int32)((sMusicWrite - sMusicRead) / 2);
  return pos > 0 ? (uint32)pos : 0;
}

ndspWaveBuf wbuf[WAVEBUF_COUNT];
int16* audioBuffer = NULL;

void AudioCallback(void *const nul_) {
  (void) nul_;

  if (!threadRunning)
    return;

  svcSignalEvent(audioThreadRequest);
}

void AudioThread(void* arg);

inline s32 GetThreadPriority() {
  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  prio -= 1;
  prio = prio < 0x18 ? 0x18 : prio;
  prio = prio > 0x3f ? 0x3f : prio;

  return prio;
}

// 3DS samples are SIGNED
inline s16 floatToS16(float f) {
  float fOut = (f * 32768);
  return (s16) fOut;
}

Result ctrNdspRc;   // ndspInit's answer: it fails without sdmc:/3ds/dspfirm.cdc (Port::EnsureDspFirmware)

static bool sMixerUp;   // the audio thread and the ndsp channel run (they need ndspInit to have worked)

// The part of the audio that talks to ndsp: the channel, the audio thread that fills its wave bufs, the callback.
static void StartMixer()
{
  ndspChnReset(0);
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  ndspChnSetRate(0, SAMPLE_RATE);
  ndspChnSetFormat(0, TARGET_OUT_FORMAT);

  // set up ndsp wavebufs
  memset(&wbuf, 0, sizeof(wbuf));
  int16* buf = audioBuffer;
  for (int i = 0; i < WAVEBUF_COUNT; i++) {
    wbuf[i].data_vaddr = buf;
    wbuf[i].status     = NDSP_WBUF_DONE;

    buf += WAVEBUF_SIZE / sizeof(buf[0]);
  }

  // create audio thread
  // (run on same core as main thread, at a higher priority: it preempts the game)
  threadRunning = true;
  audioThreadHandle = threadCreate(AudioThread, 0,
                                          THREAD_STACK_SZ, GetThreadPriority(),
                                          0, false);
  if (!audioThreadHandle) {
    PrintLog(PRINT_NORMAL, "ERROR: failed to init audio thread\n");
  }

  // create event for audio thread 
  svcCreateEvent(&audioThreadRequest, RESET_ONESHOT);
  
  // set audio callback (simply triggers audioThreadRequest event)
  ndspSetCallback(AudioCallback, NULL);
  sMixerUp = true;
}

bool32 AudioDevice::Init()
{
  // taken from 3DS audio example
  ctrNdspRc = ndspInit();

  AudioDeviceBase::InitAudioChannels();

  
  // allocate space for audio buffer
  const size_t bufferSize = WAVEBUF_SIZE * WAVEBUF_COUNT;
  audioBuffer = (int16*) linearAlloc(bufferSize);
  if (!audioBuffer) {
    PrintLog(PRINT_NORMAL, "ERROR: failed to allocate audio buffer\n");
    return false;
  }

  CtrMusicStart(GetThreadPriority());
  // Without the DSP firmware ndspInit fails and the mixer must stay off: it would drive an ndsp that is not up.
  if (R_SUCCEEDED(ctrNdspRc))
    StartMixer();
  return true;
}

bool AudioDevice::RetryDsp()
{
  if (sMixerUp)
    return true;
  ctrNdspRc = ndspInit();
  if (R_FAILED(ctrNdspRc))
    return false;
  StartMixer();   // the buffer and the music thread of Init are still there
  return true;
}

void AudioDevice::Release()
{
  CtrMusicStop();
  if (sMixerUp) {
    // close thread
    threadRunning = false;
    svcSignalEvent(audioThreadRequest);

    threadJoin(audioThreadHandle, UINT64_MAX);
    threadFree(audioThreadHandle);

    // close event handle 
    svcCloseHandle(audioThreadRequest);

    // ... then de-init audio stuff 
    ndspChnReset(0);
    ndspExit();
    sMixerUp = false;
  }
  linearFree(audioBuffer);

  AudioDeviceBase::Release();
}

void AudioDevice::FrameInit() {
  //svcSignalEvent(audioThreadRequest);
}

void AudioDevice::HandleStreamLoad(ChannelInfo* channel, bool32 async)
{
  if (async) {
    std::thread thread(LoadStream, channel);
    thread.detach();
  } else {
    LoadStream(channel);
  }
}

void AudioThread(void* arg) {
  PrintLog(PRINT_NORMAL, "CTRAudioDevice: audio thread created\n");

  while (threadRunning) {
    unsigned done = 0;
    for (size_t i = 0; i < WAVEBUF_COUNT; i++)
      done += wbuf[i].status == NDSP_WBUF_DONE;
    if (done == WAVEBUF_COUNT)
      ++ctrAudioUnderruns;
    for (size_t i = 0; i < WAVEBUF_COUNT; i++) {
      if (wbuf[i].status != NDSP_WBUF_DONE)
        continue;

      const u64 t0 = svcGetSystemTick();
      AudioDevice::ProcessAudioMixing(wbuf[i].data_pcm16, 
                                      SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE);
      ctrAudioMixTicks += svcGetSystemTick() - t0;
      wbuf[i].nsamples = SAMPLES_PER_BUF;
      ndspChnWaveBufAdd(0, &wbuf[i]);
      DSP_FlushDataCache(wbuf[i].data_pcm16, WAVEBUF_SIZE);
    }

    svcWaitSynchronization(audioThreadRequest, UINT64_MAX);
    svcClearEvent(audioThreadRequest);
  }
}
