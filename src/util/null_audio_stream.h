#pragma once
#include "audio_stream.h"

class NullAudioStream final : public AudioStream
{
public:
  NullAudioStream();
  ~NullAudioStream();

protected:
  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void FramesAvailable() override;
};
