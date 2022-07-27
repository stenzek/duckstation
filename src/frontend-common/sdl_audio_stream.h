#pragma once
#include "util/audio_stream.h"
#include <cstdint>

class SDLAudioStream final : public AudioStream
{
public:
  SDLAudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch);
  ~SDLAudioStream();

  void SetPaused(bool paused) override;
  void SetOutputVolume(u32 volume) override;

  bool OpenDevice(u32 latency_ms);
  void CloseDevice();

protected:
  ALWAYS_INLINE bool IsOpen() const { return (m_device_id != 0); }

  static void AudioCallback(void* userdata, uint8_t* stream, int len);

  u32 m_device_id = 0;
};
