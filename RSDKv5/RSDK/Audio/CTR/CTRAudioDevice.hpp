#define LockAudioDevice() { }
#define UnlockAudioDevice() { }

namespace RSDK
{

  class AudioDevice : public AudioDeviceBase
  {
    public:
      static bool32 Init();
      static void Release();

      // After the firmware was put on the SD card: ndspInit again (Init found none). True when the sound is up.
      static bool RetryDsp();

      static void FrameInit();

      static void HandleStreamLoad(ChannelInfo* channel, bool32 async);

    private:
      static void InitAudioChannels();
      static void InitMixBuffer() { };
  };

} // namespace RSDK
