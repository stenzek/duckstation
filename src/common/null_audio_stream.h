#pragma once
#include "audio_stream.h"

class NullAudioStream final : public AudioStream
{
public:
  NullAudioStream();
  ~NullAudioStream();

  static std::unique_ptr<AudioStream> Create();

protected:
  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void BufferAvailable() override;
};
