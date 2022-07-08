#pragma once
#include "util/audio_stream.h"
#include <cstdint>

class SDLAudioStream final : public AudioStream
{
public:
  SDLAudioStream();
  ~SDLAudioStream();

  static std::unique_ptr<SDLAudioStream> Create();

protected:
  ALWAYS_INLINE bool IsOpen() const { return (m_device_id != 0); }

  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void FramesAvailable() override;

  static void AudioCallback(void* userdata, uint8_t* stream, int len);

  u32 m_device_id = 0;
};
