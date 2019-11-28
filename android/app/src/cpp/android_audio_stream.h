#pragma once
#include "common/audio_stream.h"
#include <cstdint>

class AndroidAudioStream final : public AudioStream
{
public:
  AndroidAudioStream();
  ~AndroidAudioStream();

  static std::unique_ptr<AudioStream> Create();

protected:
  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;

  // static void AudioCallback(void* userdata, uint8_t* stream, int len);

  bool m_is_open = false;
};
