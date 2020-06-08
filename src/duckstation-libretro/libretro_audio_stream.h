#pragma once
#include "common/audio_stream.h"
#include <cstdint>
#include <vector>

class LibretroAudioStream final : public AudioStream
{
public:
  LibretroAudioStream();
  ~LibretroAudioStream();

protected:
  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void FramesAvailable() override;

private:
  // TODO: Optimize this buffer away.
  std::vector<SampleType> m_output_buffer;
};
