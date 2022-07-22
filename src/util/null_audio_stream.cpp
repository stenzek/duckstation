#include "null_audio_stream.h"

NullAudioStream::NullAudioStream() = default;

NullAudioStream::~NullAudioStream() = default;

bool NullAudioStream::OpenDevice()
{
  return true;
}

void NullAudioStream::PauseDevice(bool paused) {}

void NullAudioStream::CloseDevice() {}

void NullAudioStream::FramesAvailable()
{
  // drop any buffer as soon as they're available
  DropFrames(GetSamplesAvailable());
}

std::unique_ptr<AudioStream> AudioStream::CreateNullAudioStream()
{
  return std::make_unique<NullAudioStream>();
}
