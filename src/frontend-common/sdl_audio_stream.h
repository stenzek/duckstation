#pragma once
#include "common/audio_stream.h"
#include <cstdint>

class SDLAudioStream final : public AudioStream
{
public:
  SDLAudioStream();
  ~SDLAudioStream();

protected:
  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void BufferAvailable() override;

  static void AudioCallback(void* userdata, uint8_t* stream, int len);

  bool m_is_open = false;
};
