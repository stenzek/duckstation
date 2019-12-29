#include "null_audio_stream.h"

NullAudioStream::NullAudioStream() = default;

NullAudioStream::~NullAudioStream() = default;

bool NullAudioStream::OpenDevice()
{
  return true;
}

void NullAudioStream::PauseDevice(bool paused) {}

void NullAudioStream::CloseDevice() {}

void NullAudioStream::BufferAvailable()
{
  // drop any buffer as soon as they're available
  DropBuffer();
}

std::unique_ptr<AudioStream> NullAudioStream::Create()
{
  return std::unique_ptr<AudioStream>(new NullAudioStream());
}
